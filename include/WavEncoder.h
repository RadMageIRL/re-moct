// WavEncoder.h — IEncoder for WAV rip output (rip-wav-encoder slice).
// Canonical RIFF/WAVE header + raw interleaved s16 PCM. No library — the one
// encoder that is a near-verbatim dump of the CD read buffer. Untagged by
// format: the tag/R128 pass skips it via the taggable row property.
//
// writeFrames is STRICT: a short fwrite returns false (the IEncoder bool
// contract FLAC/LAME already follow). This is load-bearing, not defensive —
// a lenient write plus finalize's size back-patch would turn disk-full
// (WAV's most likely failure at ~10x FLAC's bytes) into a valid-looking,
// silently truncated file marked success. Strict write -> track aborts ->
// the caller's cleanup removes the partial. The back-patch can then only
// fire for a legitimate non-exact caller, its intended purpose (never taken
// on the CD path, whose frame count is TOC-exact).
#pragma once

#include "IEncoder.h"

#include <cstdio>

class WavEncoder : public IEncoder {
public:
    WavEncoder() = default;   // no quality knob: PCM is always 16-bit/44.1/stereo
    ~WavEncoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    bool finalize(bool ok) override;

private:
    FILE*    f_ = nullptr;
    uint64_t declared_frames_ = 0;
    uint64_t written_frames_  = 0;
};
