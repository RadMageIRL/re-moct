// M4aEncoder.cpp — AAC-LC (FDK) + MP4 mux (Mp4Mux.h). See M4aEncoder.h.
//
// The encode loop is the aac-m4a-phase0-probe's encoder, transliterated: FDK in
// TT_MP4_RAW, fed whole 1024-sample frames, each emitted access unit collected;
// finalize flushes the tail and hands the AU list + ASC to Mp4Mux::muxM4a.
#include "M4aEncoder.h"

#include "Mp4Mux.h"
#include "PortUtil.h"   // port::fopenUtf8 — the wide-path open (house rule)

#include <fdk-aac/aacenc_lib.h>

#include <cstdio>

bool M4aEncoder::open(const std::string& path, uint64_t /*total_frames*/) {
    path_ = path;
    if (aacEncOpen(&enc_, 0, kChannels) != AACENC_OK) { enc_ = nullptr; return false; }

    auto set = [&](AACENC_PARAM p, UINT v) { return aacEncoder_SetParam(enc_, p, v) == AACENC_OK; };
    bool ok = true;
    ok &= set(AACENC_AOT, 2);                        // AAC-LC (no HE-AAC)
    ok &= set(AACENC_SAMPLERATE, kSampleRate);
    ok &= set(AACENC_CHANNELMODE, MODE_2);           // stereo (kChannels == 2)
    ok &= set(AACENC_TRANSMUX, TT_MP4_RAW);          // raw access units — WE mux
    if (vbr_) {
        // VBR quality ladder 1-5 (5 best). Caller clamps; clamp again defensively.
        int lvl = vbr_level_ < 1 ? 1 : (vbr_level_ > 5 ? 5 : vbr_level_);
        ok &= set(AACENC_BITRATEMODE, (UINT)lvl);
    } else {
        ok &= set(AACENC_BITRATEMODE, 0);            // CBR
        ok &= set(AACENC_BITRATE, (UINT)cbr_bitrate_bps_);
    }
    if (!ok) { aacEncClose(&enc_); enc_ = nullptr; return false; }

    if (aacEncEncode(enc_, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
        aacEncClose(&enc_); enc_ = nullptr; return false;
    }
    AACENC_InfoStruct info{};
    if (aacEncInfo(enc_, &info) != AACENC_OK) { aacEncClose(&enc_); enc_ = nullptr; return false; }
    frame_length_ = info.frameLength ? (uint32_t)info.frameLength : 1024;
    asc_.assign(info.confBuf, info.confBuf + info.confSize);

    opened_ = true;
    return !asc_.empty();
}

// One FDK encode call. numInSamples < 0 signals EOF (drain). Collects an AU when
// FDK emits bytes. Returns false on a real error (or a completed drain).
static bool encodeCall(AACENCODER* enc, INT numInSamples, void* inPtr,
                       std::vector<uint8_t>& obuf,
                       std::vector<std::vector<uint8_t>>& aus, uint64_t& total_bytes) {
    INT   inId = IN_AUDIO_DATA;
    INT   inSize = numInSamples > 0 ? numInSamples * (INT)sizeof(INT_PCM) : 0;
    INT   inEl = sizeof(INT_PCM);
    void* outPtr = obuf.data();
    INT   outId = OUT_BITSTREAM_DATA, outSize = (INT)obuf.size(), outEl = 1;

    AACENC_BufDesc inDesc{};  inDesc.numBufs = 1; inDesc.bufs = &inPtr;
    inDesc.bufferIdentifiers = &inId; inDesc.bufSizes = &inSize; inDesc.bufElSizes = &inEl;
    AACENC_BufDesc outDesc{}; outDesc.numBufs = 1; outDesc.bufs = &outPtr;
    outDesc.bufferIdentifiers = &outId; outDesc.bufSizes = &outSize; outDesc.bufElSizes = &outEl;
    AACENC_InArgs inArgs{};  inArgs.numInSamples = numInSamples;
    AACENC_OutArgs outArgs{};

    AACENC_ERROR r = aacEncEncode(enc, &inDesc, &outDesc, &inArgs, &outArgs);
    if (numInSamples < 0 && r == AACENC_ENCODE_EOF && outArgs.numOutBytes == 0) return false; // drained
    if (r != AACENC_OK && r != AACENC_ENCODE_EOF) return false;
    if (outArgs.numOutBytes > 0) {
        aus.emplace_back(obuf.data(), obuf.data() + outArgs.numOutBytes);
        total_bytes += (uint64_t)outArgs.numOutBytes;
    }
    return true;
}

bool M4aEncoder::feedFrame(const int16_t* interleaved, int frames) {
    static thread_local std::vector<uint8_t> obuf;
    if (obuf.size() < 16384) obuf.resize(16384);
    // INT_PCM is int16_t on this build (no HI_RES); the interleaved s16 buffer is
    // FDK's input format directly.
    return encodeCall(enc_, (INT)(frames * (int)kChannels),
                      (void*)interleaved, obuf, aus_, total_bytes_);
}

bool M4aEncoder::writeFrames(const int16_t* interleaved, size_t frames) {
    if (!opened_ || !enc_) return false;
    residual_.insert(residual_.end(), interleaved, interleaved + frames * kChannels);

    const size_t per = frame_length_;               // samples/channel per AU
    size_t have = residual_.size() / kChannels;
    size_t consumed = 0;
    while (have - consumed >= per) {
        if (!feedFrame(residual_.data() + consumed * kChannels, (int)per)) return false;
        consumed += per;
    }
    if (consumed)
        residual_.erase(residual_.begin(), residual_.begin() + consumed * kChannels);
    return true;
}

bool M4aEncoder::drainTail() {
    static thread_local std::vector<uint8_t> obuf;
    if (obuf.size() < 16384) obuf.resize(16384);
    // Feed any sub-frame remainder, then drain FDK's internal delay (bounded).
    if (!residual_.empty()) {
        int rem = (int)(residual_.size() / kChannels);
        if (rem > 0) encodeCall(enc_, (INT)(rem * (int)kChannels),
                                residual_.data(), obuf, aus_, total_bytes_);
        residual_.clear();
    }
    for (int guard = 0; guard < 64; ++guard)
        if (!encodeCall(enc_, -1, nullptr, obuf, aus_, total_bytes_)) break;
    return true;
}

bool M4aEncoder::finalize(bool ok) {
    bool wrote = false;
    if (ok && opened_ && enc_ && !asc_.empty()) {
        drainTail();

        mp4mux::Track t;
        t.asc         = asc_;
        t.sampleRate  = kSampleRate;
        t.channels    = kChannels;
        t.frameLength = frame_length_;
        t.aus         = std::move(aus_);
        double secs   = t.aus.empty() ? 0.0
                        : (double)t.aus.size() * frame_length_ / (double)kSampleRate;
        t.avgBitrate  = secs > 0 ? (uint32_t)(total_bytes_ * 8 / secs) : kAacDefaultBitrate;

        std::vector<uint8_t> bytes = mp4mux::muxM4a(t);
        if (!bytes.empty()) {
            if (FILE* f = port::fopenUtf8(path_, "wb")) {
                size_t w = std::fwrite(bytes.data(), 1, bytes.size(), f);
                // fflush before close so a full-disk short write is seen HERE, not
                // swallowed by a failing close (the ENOSPC-laundering lesson).
                bool flush_ok = (std::fflush(f) == 0);
                bool close_ok = (std::fclose(f) == 0);
                wrote = (w == bytes.size()) && flush_ok && close_ok;
            }
        }
    }
    if (enc_) { aacEncClose(&enc_); enc_ = nullptr; }
    aus_.clear(); aus_.shrink_to_fit();
    residual_.clear();
    opened_ = false;
    return wrote;
}
