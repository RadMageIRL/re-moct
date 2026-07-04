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
