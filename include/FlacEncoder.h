// FlacEncoder.h — IEncoder for FLAC rip output (rip-encoder-seam slice).
// Verbatim transliteration of the inline FLAC path from CDRipper::ripTrack:
// level 5, verify off, 2ch/16-bit/44.1k, total_samples_estimate from the TOC.
// Byte-identity vs the original inline code is pinned by rip_encoder_seam_test.
#pragma once

#include "IEncoder.h"

#include <FLAC/stream_encoder.h>

#include <vector>

class FlacEncoder : public IEncoder {
public:
    FlacEncoder() = default;
    ~FlacEncoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    void finalize(bool ok) override;

private:
    FLAC__StreamEncoder*     enc_ = nullptr;
    std::vector<FLAC__int32> stage_;   // int16 -> int32 widening buffer
};
