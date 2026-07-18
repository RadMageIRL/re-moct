// Mp3Encoder.h — IEncoder for MP3 rip output (rip-encoder-seam slice).
// Verbatim transliteration of the inline LAME path from CDRipper::ripTrack:
// VBR mtrh V0 quality-2 STEREO, manual Xing tag rewrite at finalize. Byte-
// identity vs the original inline code is pinned by rip_encoder_seam_test.
#pragma once

#include "IEncoder.h"

#include <lame/lame.h>

#include <cstdio>
#include <vector>

class Mp3Encoder : public IEncoder {
public:
    // vbr_q FIRST, defaulting to the pre-seam VBR-V0 literal: the arg-free and
    // Mp3Encoder(0) constructions the seam oracle uses stay byte-identical VBR.
    // The CBR path (encoder-bitrate-mode) is strictly ADDITIVE - cbr defaults
    // false, so nothing about the VBR sequence changes. vbr_mtrh + quality 2
    // remain fixed literals in open(). Caller clamps vbr_q (0-9) + bitrate.
    explicit Mp3Encoder(int vbr_q = 0, bool cbr = false, int cbr_bitrate_bps = 256000)
        : vbr_q_(vbr_q), cbr_(cbr), cbr_bitrate_bps_(cbr_bitrate_bps) {}
    ~Mp3Encoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    bool finalize(bool ok) override;

private:
    int                  vbr_q_ = 0;
    bool                 cbr_ = false;             // true = CBR (drops the Xing path)
    int                  cbr_bitrate_bps_ = 256000;
    lame_global_flags*   lame_ = nullptr;
    FILE*                file_ = nullptr;
    std::vector<int16_t> left_, right_;   // deinterleave staging
    std::vector<uint8_t> out_;            // encoded-frame staging
};
