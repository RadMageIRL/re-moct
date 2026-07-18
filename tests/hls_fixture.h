// hls_fixture.h — shared synthetic HLS origin for the HLS pipeline tests.
//
// A FakeHls (core::IHttp) serves master -> variant (a 4-segment sliding window) ->
// segments carrying REAL FDK-encoded ADTS sine, so the whole decode/buffer path
// runs exactly as production, off-network. Factored out of hls_pipeline_test so
// plugin_hls_parity_test (Phase 4 slice d — the identical-to-compiled-in byte
// gate) reuses the identical fixture through the loaded plugin boundary.
//
// Transport-only: no StreamSource / miniaudio here (the includer owns those).
#pragma once
#include "core/IHttp.h"
#include "PortUtil.h"   // sleepMs/tickMs (baseline Sleep/GetTickCount on Windows)

#include <fdk-aac/aacenc_lib.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ─── Fixture: real ADTS audio (FDK-encoded stereo 44100 sine) ─────────────────
// 44100 in == 44100 out, so StreamSource's resampler is BYPASSED — the whole
// capture path stays fixed-point/integer (FDK decode -> int16 ring -> exact
// int16->float), which is what makes the slice-d byte-identity hold across two
// separately-compiled targets (compiled-in vs the .so), independent of float flags.
inline std::vector<uint8_t> encodeAdtsSine(double secs, double freq, double amp) {
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
inline std::vector<uint8_t> makeId3(const std::string& artist, const std::string& title) {
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

inline double rmsOf(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double acc = 0.0;
    for (float s : v) acc += (double)s * s;
    return std::sqrt(acc / (double)v.size());
}

template <typename Pred>
inline bool waitFor(int timeout_ms, Pred pred) {
    uint32_t t0 = port::tickMs();
    while ((long)(port::tickMs() - t0) < timeout_ms) {
        if (pred()) return true;
        port::sleepMs(10);
    }
    return false;
}

// ─── FakeHls: a synthetic HLS origin behind the seam ──────────────────────────
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
    std::atomic<bool> fail_segments  { false };// segment GETs fail fast (reconnect path)
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
            if (fail_segments.load()) {
                // A dying origin: immediate failure — drives the producer's
                // reconnect path (slice B uses it to pin the tee's discont).
                res.ok = false; res.status = 503;
                return res;
            }
            if (block_segments.load()) {
                // A wedged origin: sit on the request until the caller's cancel
                // token fires (bounded so a broken cancel can't hang the test).
                in_blocked.store(true);
                uint32_t t0 = port::tickMs();
                while ((long)(port::tickMs() - t0) < 30000) {
                    if (core::httpCancelRequested(req.cancel)) {
                        saw_cancel.store(true);
                        in_blocked.store(false);
                        core::finalizeCancelled(res);
                        return res;
                    }
                    port::sleepMs(10);
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
