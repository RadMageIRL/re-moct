// FlacEncoder.cpp — see FlacEncoder.h. The bodies below are the inline FLAC
// blocks from CDRipper::ripTrack moved verbatim (setup / process_interleaved /
// finish); only the buffer names changed. Do not "improve" values or ordering
// here — byte-identity with the pre-seam rip output is the contract, and
// rip_encoder_seam_test diffs this class against a frozen copy of the
// original inline sequence.
#include "FlacEncoder.h"

#include "PortUtil.h"   // port::fopenUtf8 — same open call the inline code used

#include <cstdio>

namespace {
// The rip format constants, exactly as in CDRipper.cpp. The compression
// level moved to the ctor (config-fed, default 5 = the old literal).
constexpr int SAMPLE_RATE = 44100;
constexpr int CHANNELS    = 2;
constexpr int BIT_DEPTH   = 16;
} // namespace

bool FlacEncoder::open(const std::string& path, uint64_t total_frames) {
    enc_ = FLAC__stream_encoder_new();
    if (!enc_) return false;
    FLAC__stream_encoder_set_verify(enc_, false);
    FLAC__stream_encoder_set_compression_level(enc_, level_);
    FLAC__stream_encoder_set_channels(enc_, CHANNELS);
    FLAC__stream_encoder_set_bits_per_sample(enc_, BIT_DEPTH);
    FLAC__stream_encoder_set_sample_rate(enc_, SAMPLE_RATE);
    FLAC__stream_encoder_set_total_samples_estimate(enc_, (FLAC__uint64)total_frames);
    FILE* f = port::fopenUtf8(path, "wb");
    if (!f) {
        FLAC__stream_encoder_delete(enc_); enc_ = nullptr;
        return false;
    }
    // init_FILE takes ownership of f on success; on failure we still own it.
    if (FLAC__stream_encoder_init_FILE(enc_, f, nullptr, nullptr)
            != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        fclose(f);
        FLAC__stream_encoder_delete(enc_); enc_ = nullptr;
        return false;
    }
    return true;
}

bool FlacEncoder::writeFrames(const int16_t* interleaved, size_t frames) {
    if (!enc_) return false;
    stage_.resize(frames * CHANNELS);
    for (size_t i = 0; i < frames; ++i) {          // the ripTrack widening, verbatim
        stage_[i*2]     = (FLAC__int32)interleaved[i*2];
        stage_[i*2 + 1] = (FLAC__int32)interleaved[i*2 + 1];
    }
    return FLAC__stream_encoder_process_interleaved(enc_, stage_.data(),
                                                    (unsigned)frames) != 0;
}

bool FlacEncoder::finalize(bool ok) {
    (void)ok;   // FLAC finish ran unconditionally in the inline code
    if (!enc_) return true;
    FLAC__stream_encoder_finish(enc_);
    FLAC__stream_encoder_delete(enc_);
    enc_ = nullptr;
    return true;   // best-effort, exactly the pre-seam inline behavior
}
