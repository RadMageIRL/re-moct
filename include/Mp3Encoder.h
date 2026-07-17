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
    // Default MUST stay 0 (V0) — the pre-seam literal; pinned by the seam
    // oracle's argument-free construction. vbr_mtrh + quality 2 remain fixed
    // literals in open() by design. Caller clamps (0-9); this ctor trusts it.
    explicit Mp3Encoder(int vbr_q = 0) : vbr_q_(vbr_q) {}
    ~Mp3Encoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    void finalize(bool ok) override;

private:
    int                  vbr_q_ = 0;
    lame_global_flags*   lame_ = nullptr;
    FILE*                file_ = nullptr;
    std::vector<int16_t> left_, right_;   // deinterleave staging
    std::vector<uint8_t> out_;            // encoded-frame staging
};
