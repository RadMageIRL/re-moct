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
#ifdef _WIN32

#define MINIAUDIO_IMPLEMENTATION            // StreamSource.cpp holds decls only
#include "miniaudio.h"

#include "StreamSource.h"
#include "core/IHttp.h"

#include <fdk-aac/aacenc_lib.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ─── Fixture: real ADTS audio (FDK-encoded stereo 44100 sine) ─────────────────

static std::vector<uint8_t> encodeAdtsSine(double secs, double freq, double amp) {
    std::vector<uint8_t> out;
    HANDLE_AACENCODER enc = nullptr;
    if (aacEncOpen(&enc, 0, 2) != AACENC_OK) return out;
    aacEncoder_SetParam(enc, AACENC_AOT, 2);              // AAC-LC
    aacEncoder_SetParam(enc, AACENC_SAMPLERATE, 44100);
    aacEncoder_SetParam(enc, AACENC_CHANNELMODE, MODE_2);
    aacEncoder_SetParam(enc, AACENC_BITRATE, 128000);
    aacEncoder_SetParam(enc, AACENC_TRANSMUX, TT_MP4_ADTS);
    if (aacEncEncode(enc, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
        aacEncClose(&enc); return out;
    }
    AACENC_InfoStruct info{};
    aacEncInfo(enc, &info);
    const int frame = (int)info.frameLength;              // 1024 for LC
    const int total = (int)(secs * 44100.0);

    std::vector<INT_PCM> pcm((size_t)frame * 2);
    std::vector<uint8_t> obuf(8192);
    int pos = 0;
    while (pos < total) {
        for (int i = 0; i < frame; ++i) {
            double t = (double)(pos + i) / 44100.0;
            INT_PCM s = (INT_PCM)(amp * 32767.0 * std::sin(2.0 * 3.14159265358979 * freq * t));
            pcm[(size_t)i * 2]     = s;
            pcm[(size_t)i * 2 + 1] = s;
        }
        void* inPtr  = pcm.data();
        INT   inId   = IN_AUDIO_DATA, inSize = (INT)(pcm.size() * sizeof(INT_PCM)), inEl = sizeof(INT_PCM);
        void* outPtr = obuf.data();
        INT   outId  = OUT_BITSTREAM_DATA, outSize = (INT)obuf.size(), outEl = 1;
        AACENC_BufDesc inDesc{};  inDesc.numBufs = 1;  inDesc.bufs = &inPtr;
        inDesc.bufferIdentifiers = &inId;   inDesc.bufSizes = &inSize;  inDesc.bufElSizes = &inEl;
        AACENC_BufDesc outDesc{}; outDesc.numBufs = 1; outDesc.bufs = &outPtr;
        outDesc.bufferIdentifiers = &outId; outDesc.bufSizes = &outSize; outDesc.bufElSizes = &outEl;
        AACENC_InArgs  inArgs{};  inArgs.numInSamples = frame * 2;
        AACENC_OutArgs outArgs{};
        if (aacEncEncode(enc, &inDesc, &outDesc, &inArgs, &outArgs) != AACENC_OK) break;
        out.insert(out.end(), obuf.data(), obuf.data() + outArgs.numOutBytes);
        pos += frame;
    }
    aacEncClose(&enc);
    return out;
}

// ID3v2.3 tag with latin1 TPE1 + TIT2 (the timed-ID3 shape hlsParseId3 consumes).
static std::vector<uint8_t> makeId3(const std::string& artist, const std::string& title) {
    auto frame = [](const char* id, const std::string& text) {
        std::vector<uint8_t> f = { (uint8_t)id[0], (uint8_t)id[1], (uint8_t)id[2], (uint8_t)id[3] };
        uint32_t sz = 1 + (uint32_t)text.size();          // v2.3 size: plain big-endian
        f.push_back((uint8_t)(sz >> 24)); f.push_back((uint8_t)(sz >> 16));
        f.push_back((uint8_t)(sz >> 8));  f.push_back((uint8_t)sz);
        f.push_back(0); f.push_back(0);                   // frame flags
        f.push_back(0x00);                                // ISO-8859-1
        f.insert(f.end(), text.begin(), text.end());
        return f;
    };
    std::vector<uint8_t> body = frame("TPE1", artist);
    auto t = frame("TIT2", title);
    body.insert(body.end(), t.begin(), t.end());
    uint32_t bs = (uint32_t)body.size();                  // tag size: syncsafe
    std::vector<uint8_t> tag = { 'I', 'D', '3', 0x03, 0x00, 0x00,
        (uint8_t)((bs >> 21) & 0x7f), (uint8_t)((bs >> 14) & 0x7f),
        (uint8_t)((bs >> 7) & 0x7f),  (uint8_t)(bs & 0x7f) };
    tag.insert(tag.end(), body.begin(), body.end());
    return tag;
}

// ─── FakeHttp: a synthetic HLS origin behind the seam ─────────────────────────
// Serves master -> variant (4-segment sliding window the TEST advances) ->
// segments. Thread-safe: fetch() is called from both the open path and the
// producer thread, exactly like production.

struct FakeHls final : core::IHttp {
    static constexpr const char* MASTER  = "http://fake.local/hls/master.m3u8";
    static constexpr const char* VARIANT = "http://fake.local/hls/variant.m3u8";

    std::mutex mtx;
    uint64_t window_top = 103;                 // newest seq; window = top-3 .. top
    uint64_t id3_seq    = 0;                   // segment that carries the ID3 prefix
    std::vector<uint8_t> seg_audio;            // canned ADTS payload (same every segment)
    std::vector<uint8_t> id3;

    std::atomic<bool> block_segments { false };// segment GETs block until cancelled
    std::atomic<bool> in_blocked     { false };
    std::atomic<bool> saw_cancel     { false };
    std::atomic<int>  segment_gets   { 0 };
    std::atomic<int>  variant_gets   { 0 };

    core::HttpResponse fetch(const core::HttpRequest& req) override {
        core::HttpResponse res;
        res.status    = 200;
        res.final_url = req.url;
        if (req.url == MASTER) {
            res.body = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000\n" +
                       std::string(VARIANT) + "\n";
            res.ok = true;
            return res;
        }
        if (req.url == VARIANT) {
            ++variant_gets;
            uint64_t top;
            { std::lock_guard<std::mutex> lk(mtx); top = window_top; }
            std::string m = "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:" +
                            std::to_string(top - 3) + "\n";
            for (uint64_t s = top - 3; s <= top; ++s)
                m += "#EXTINF:2.0,\nhttp://fake.local/hls/seg" + std::to_string(s) + ".aac\n";
            res.body = m;
            res.ok = true;
            return res;
        }
        size_t p = req.url.find("/seg");
        if (p != std::string::npos) {
            ++segment_gets;
            if (block_segments.load()) {
                // A wedged origin: sit on the request until the caller's cancel
                // token fires (bounded so a broken cancel can't hang the test).
                in_blocked.store(true);
                DWORD t0 = GetTickCount();
                while ((long)(GetTickCount() - t0) < 30000) {
                    if (req.cancel && req.cancel->load()) {
                        saw_cancel.store(true);
                        in_blocked.store(false);
                        core::finalizeCancelled(res);
                        return res;
                    }
                    Sleep(10);
                }
                in_blocked.store(false);
                res.ok = false;
                return res;
            }
            uint64_t seq = std::strtoull(req.url.c_str() + p + 4, nullptr, 10);
            std::lock_guard<std::mutex> lk(mtx);
            std::string body;
            if (seq == id3_seq)
                body.assign(id3.begin(), id3.end());
            body.append(seg_audio.begin(), seg_audio.end());
            res.body = std::move(body);
            res.ok = true;
            return res;
        }
        res.ok = false; res.status = 404;
        return res;
    }
};

// ─── Consumer helpers ──────────────────────────────────────────────────────────

static double rmsOf(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double acc = 0.0;
    for (float s : v) acc += (double)s * s;
    return std::sqrt(acc / (double)v.size());
}

// Paced drain of n frames through readFrames (the audio-callback contract).
static std::vector<float> drainFrames(StreamSource& ss, int frames) {
    std::vector<float> all;
    all.reserve((size_t)frames * 2);
    float buf[512 * 2];
    int left = frames;
    while (left > 0) {
        int n = left < 512 ? left : 512;
        ss.readFrames(buf, (uint32_t)n);
        all.insert(all.end(), buf, buf + (size_t)n * 2);
        left -= n;
        Sleep(1);
    }
    return all;
}

template <typename Pred>
static bool waitFor(int timeout_ms, Pred pred) {
    DWORD t0 = GetTickCount();
    while ((long)(GetTickCount() - t0) < timeout_ms) {
        if (pred()) return true;
        Sleep(10);
    }
    return false;
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
        StreamSource ss;

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
            DWORD t0 = GetTickCount();
            bool underrun = false;
            float buf[512 * 2];
            while ((long)(GetTickCount() - t0) < 15000) { // drain fast, no pacing
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
                DWORD t0 = GetTickCount();
                float buf[512 * 2];
                while ((long)(GetTickCount() - t0) < 15000 && !ss.buffering())
                    ss.readFrames(buf, 512);
            }
            { std::lock_guard<std::mutex> lk(fake.mtx); fake.window_top += 2; }
            CHECK(waitFor(8000, [&]{ return fake.in_blocked.load(); }));

            DWORD t0 = GetTickCount();
            ss.close();
            DWORD dt = GetTickCount() - t0;
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
#else
int main() { return 0; }
#endif
