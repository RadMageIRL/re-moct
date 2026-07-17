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
    Mp3Encoder() = default;
    ~Mp3Encoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    void finalize(bool ok) override;

private:
    lame_global_flags*   lame_ = nullptr;
    FILE*                file_ = nullptr;
    std::vector<int16_t> left_, right_;   // deinterleave staging
    std::vector<uint8_t> out_;            // encoded-frame staging
};
