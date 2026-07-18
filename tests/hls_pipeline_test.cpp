// hls_pipeline_test.cpp — Slice 0 (Phase 2 prep): thin replay net, HLS half.
//
// A FakeHttp injected via core::setHttp() drives the REAL StreamSource HLS/AAC
// pipeline end-to-end and off-network: open() -> hlsConnect (master resolve +
// variant poll + live-edge prime) -> producerWorkerAAC (producer thread) ->
// hlsRawRead segment pump -> FDK decode -> int16 SPSC ring -> readFrames
// (consumer). This is the Phase-0 Tier-2 record/replay idea realized cheaply on
// the slice-4 IHttp seam: the fake serves synthetic manifests + REAL ADTS audio
// (FDK-encoded sine, generated at test start), so the whole decode/buffer path
// runs exactly as production.
//
// Covered contracts (the semantics Phase 2's Source interface must preserve):
//   1. open(): master -> variant resolve, initial poll, prime-to-live (2 segs)
//   2. prebuffer: buffering() until PREBUFFER_SEC accumulated, then audio flows
//      (readFrames delivers real energy, position advances)
//   3. live-window stall -> ring drains -> underrun drops back to buffering,
//      output silent (never dribbles broken audio)
//   4. window resumes -> producer re-polls, refetches, prebuffer self-heals
//   5. timed-ID3 (TIT2/TPE1) in a segment surfaces through nowPlaying()
//   6. PROMPT CLOSE mid-fetch: stop_ rides hlsHttpGet as the cancel token — a
//      close() during a blocked segment fetch must return well under the old
//      8s worst case (the headless twin of the slice-4 "mid-segment stop <1s"
//      7of9 gate) and the fake must actually observe the cancel.
//
// HONEST LIMITS: the ICY/continuous path (rawRead -> InternetReadFile) is raw
// WinINet by design — permanently outside any seam — so it CANNOT be replayed
// headlessly; it stays covered by the live gates. Real network jitter, ad-pod
// re-pin timing, and device interplay also stay with the live gates. What this
// proves is the producer/ring/prebuffer/cancel SEMANTICS.
// PORTABLE since Phase 3 slice 2: the HLS pump is fully seam-routed, so this
// runs against WinINet on Windows and libcurl on Linux (both matrix jobs).
//
// The synthetic origin + ADTS fixture live in hls_fixture.h (factored in Phase 4
// slice d so plugin_hls_parity_test reuses the identical fixture through the
// loaded plugin boundary).

#define MINIAUDIO_IMPLEMENTATION            // StreamSource.cpp holds decls only
#include "miniaudio.h"

#include "hls_fixture.h"   // encodeAdtsSine / makeId3 / FakeHls / rmsOf / waitFor
#include "StreamSource.h"
#include "core/ISource.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Paced drain of n frames through readFrames (the audio-callback contract),
// via core::ISource — the slice-A interface dispatch is what's under test.
static std::vector<float> drainFrames(core::ISource& src, int frames) {
    std::vector<float> all;
    all.reserve((size_t)frames * 2);
    float buf[512 * 2];
    int left = frames;
    while (left > 0) {
        int n = left < 512 ? left : 512;
        src.readFrames(buf, (uint32_t)n);
        all.insert(all.end(), buf, buf + (size_t)n * 2);
        left -= n;
        port::sleepMs(1);
    }
    return all;
}

int main() {
    // Build the fixture first: ~2s of a -6 dBFS 440 Hz sine per segment.
    FakeHls fake;
    fake.seg_audio = encodeAdtsSine(2.0, 440.0, 0.5);
    fake.id3       = makeId3("Bjork", "Joga");
    CHECK(fake.seg_audio.size() > 10000);                 // encoder actually produced ADTS
    CHECK(fake.seg_audio.size() >= 2 && fake.seg_audio[0] == 0xFF
          && (fake.seg_audio[1] & 0xF6) == 0xF0);         // ADTS sync at byte 0

    core::setHttp(&fake);
    {
        StreamSource ss(core::http());                    // slice c: HTTP injected (== fake via setHttp)
        core::ISource& s = ss;                            // the slice-A contract

        // ── 0. The core::ISource contract surface (slice A) ───────────────────
        {
            auto sc = s.caps();
            CHECK(!sc.seekable && !sc.finite && sc.live); // declared, matching reality
            CHECK(!s.seekTo(3.0));                        // live edge only, always false
            CHECK(s.durationSec() == 0.0);                // no known end
        }

        // ── 1+2. open -> prime -> prebuffer -> audio flows ────────────────────
        CHECK(ss.open(FakeHls::MASTER));                  // .m3u8 -> HLS mode, AAC worker
        CHECK(ss.isOpen());
        // Prime kept the last 2 of the 4 advertised segments = 4s of audio > 3s
        // prebuffer target; the producer fetches + decodes them off-thread.
        CHECK(waitFor(8000, [&]{ return ss.isPrebuffered(); }));
        CHECK(!ss.buffering());

        auto sec1 = drainFrames(ss, 50 * 512);            // ~0.58s of audio... keep draining
        auto sec2 = drainFrames(ss, 50 * 512);
        CHECK(rmsOf(sec1) > 0.15);                        // real decoded sine, not silence
        CHECK(rmsOf(sec2) > 0.15);
        CHECK(ss.positionSec() >= 1);                     // wall-clock position advanced

        // ── 3. live window stalls -> underrun -> back to buffering, silent ────
        // (window_top untouched: every poll returns the same 4 segments, new=0)
        {
            uint32_t t0 = port::tickMs();
            bool underrun = false;
            float buf[512 * 2];
            while ((long)(port::tickMs() - t0) < 15000) { // drain fast, no pacing
                ss.readFrames(buf, 512);
                if (ss.buffering()) { underrun = true; break; }
            }
            CHECK(underrun);
            auto quiet = drainFrames(ss, 10 * 512);       // while buffering: silence only
            CHECK(rmsOf(quiet) == 0.0);
        }

        // ── 4+5. window resumes (with an ID3-tagged segment) -> self-heal ─────
        {
            std::lock_guard<std::mutex> lk(fake.mtx);
            fake.id3_seq    = fake.window_top + 1;        // first new segment carries ID3
            fake.window_top += 4;                         // +8s of audio available
        }
        CHECK(waitFor(10000, [&]{ return ss.isPrebuffered(); }));
        auto healed = drainFrames(ss, 50 * 512);
        CHECK(rmsOf(healed) > 0.15);                      // audio resumed after self-heal
        CHECK(waitFor(5000, [&]{ return ss.nowPlaying() == "Bjork - Joga"; }));

        // ── 5b. abi-cluster keep-draining: the pause contract, both ways ──────
        // While record-active, a playback pause interrupts NOTHING: readFrames
        // keeps draining REAL frames and the producer keeps consuming the
        // network (segment_gets advances). With record-active off, the same
        // paused stream reverts to the old contract byte-for-byte: silence
        // pads + a frozen producer. Both pinned in one run.
        {
            { std::lock_guard<std::mutex> lk(fake.mtx); fake.window_top += 6; }  // +12s of fresh window
            ss.setRecordActive(true);
            ss.pause(true);
            int gets0 = fake.segment_gets.load();
            auto held = drainFrames(ss, 400 * 512);       // ~4.6s drained WHILE PAUSED
            CHECK(rmsOf(held) > 0.15);                    // real audio through the pause
            CHECK(waitFor(8000, [&]{ return fake.segment_gets.load() > gets0; }));  // network consumed

            ss.setRecordActive(false);                    // still paused: the OLD contract
            port::sleepMs(300);                           // let any in-flight fetch settle
            auto silent = drainFrames(ss, 20 * 512);
            CHECK(rmsOf(silent) == 0.0);                  // silence pads again
            int gets1 = fake.segment_gets.load();
            port::sleepMs(800);
            CHECK(fake.segment_gets.load() == gets1);     // producer frozen again

            ss.pause(false);                              // resume: normal playback
            CHECK(waitFor(10000, [&]{ return ss.isPrebuffered(); }));
            auto resumed = drainFrames(ss, 50 * 512);
            CHECK(rmsOf(resumed) > 0.15);
        }

        // ── 5c. copy/remux tee (abi-cluster slice B) ──────────────────────────
        // The plugin-side byte ring: disarmed = zero reads; armed = the EXACT
        // post-transport bytes (segment payloads, ID3 stripped — the tee'd
        // stream must be a byte-aligned run of seg_audio repetitions); codec
        // reports AAC; a segment-fetch death -> reconnect -> self-heal fires
        // the discont bit the muxer resyncs on.
        {
            uint8_t tb[8192]; int32_t codec = -1, disc = -1;
            CHECK(ss.encodedCaps() == 2);                 // AAC ADTS (connect decided)
            CHECK(ss.readEncoded(tb, sizeof tb, &codec, &disc) == 0);  // disarmed
            CHECK(codec == 2);

            ss.setEncodedCapture(true);
            { std::lock_guard<std::mutex> lk(fake.mtx); fake.window_top += 6; }

            // (b) byte fidelity: gather >= 1.5 segments of tee'd bytes while
            // keeping the consumer draining (producer backpressure).
            std::vector<uint8_t> got;
            bool clean_disc = false;
            uint32_t t0 = port::tickMs();
            while ((long)(port::tickMs() - t0) < 20000 &&
                   got.size() < fake.seg_audio.size() * 3 / 2) {
                float pb[512 * 2];
                ss.readFrames(pb, 512);
                int32_t c = 0, d = 0;
                uint32_t n = ss.readEncoded(tb, sizeof tb, &c, &d);
                if (d) clean_disc = true;
                if (n) { got.insert(got.end(), tb, tb + n); CHECK(c == 2); }
                else port::sleepMs(5);
            }
            CHECK(!clean_disc);                           // no gap on a clean run
            CHECK(got.size() >= fake.seg_audio.size());
            {
                // Align the (mid-stream) start inside seg_audio, then every
                // byte must continue the cyclic repetition — proving the tee
                // carries the broadcast bytes verbatim, ID3 never leaks.
                const auto& seg = fake.seg_audio;
                size_t probe = got.size() < 256 ? got.size() : 256;
                size_t off = std::string::npos;
                for (size_t i = 0; i + probe <= seg.size() && off == std::string::npos; ++i)
                    if (std::memcmp(seg.data() + i, got.data(), probe) == 0) off = i;
                CHECK(off != std::string::npos);
                bool cyclic_ok = off != std::string::npos;
                for (size_t i = 0; cyclic_ok && i < got.size(); ++i)
                    cyclic_ok = got[i] == seg[(off + i) % seg.size()];
                CHECK(cyclic_ok);
            }

            // (c) discont across a die -> reconnect -> self-heal boundary.
            int gets0 = fake.segment_gets.load();
            fake.fail_segments.store(true);
            { std::lock_guard<std::mutex> lk(fake.mtx); fake.window_top += 2; }
            {
                // Keep the consumer draining so backpressure can't park the
                // producer before it reaches the (dying) segment fetch.
                uint32_t tw = port::tickMs();
                while ((long)(port::tickMs() - tw) < 15000 &&
                       fake.segment_gets.load() <= gets0) {
                    float pb[512 * 2];
                    ss.readFrames(pb, 512);
                }
                CHECK(fake.segment_gets.load() > gets0);
            }
            port::sleepMs(300);                           // let the failure land
            fake.fail_segments.store(false);
            { std::lock_guard<std::mutex> lk(fake.mtx); fake.window_top += 4; }
            bool saw_disc = false, post_bytes = false;
            t0 = port::tickMs();
            while ((long)(port::tickMs() - t0) < 20000 && !(saw_disc && post_bytes)) {
                float pb[512 * 2];
                ss.readFrames(pb, 512);
                int32_t c = 0, d = 0;
                uint32_t n = ss.readEncoded(tb, sizeof tb, &c, &d);
                if (d) saw_disc = true;
                if (n && saw_disc) post_bytes = true;     // stream continues past the gap
                if (!n) port::sleepMs(5);
            }
            CHECK(saw_disc);
            CHECK(post_bytes);

            // (d) disarm: reads stop, playback untouched.
            ss.setEncodedCapture(false);
            int32_t c2 = 0, d2 = 0;
            CHECK(ss.readEncoded(tb, sizeof tb, &c2, &d2) == 0);
        }

        // ── 6. prompt close mid-fetch: stop_ as the cancel token ──────────────
        {
            fake.block_segments.store(true);
            // Starve the ring so the producer goes back to the pump, then hand it
            // a new segment whose fetch blocks until cancelled.
            {
                uint32_t t0 = port::tickMs();
                float buf[512 * 2];
                while ((long)(port::tickMs() - t0) < 15000 && !ss.buffering())
                    ss.readFrames(buf, 512);
            }
            { std::lock_guard<std::mutex> lk(fake.mtx); fake.window_top += 2; }
            CHECK(waitFor(8000, [&]{ return fake.in_blocked.load(); }));

            uint32_t t0 = port::tickMs();
            ss.close();
            uint32_t dt = port::tickMs() - t0;
            std::printf("  close() during blocked segment fetch: %lu ms\n", (unsigned long)dt);
            CHECK(dt < 3000);                             // baseline gate: never the old 8s hang
            CHECK(fake.saw_cancel.load());                // the token actually reached the wire
            CHECK(!ss.isOpen());                          // producer joined
        }
    }
    core::setHttp(nullptr);

    if (g_fail == 0) { std::printf("hls_pipeline_test: ALL PASS\n"); return 0; }
    std::printf("hls_pipeline_test: %d FAILURE(S)\n", g_fail);
    return 1;
}
