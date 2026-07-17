// IEncoder.h — the rip-output encoder seam (rip-encoder-seam slice).
//
// One rip-output format. Pure PCM-to-file: open a target, accept interleaved
// s16 stereo 44.1k blocks (exactly what the CD read loop produces), finalize.
// Tagging, cover art, and ReplayGain happen AFTER encoding, per format, in the
// existing CDRipper::tagFile pass — album gain only exists once every track is
// done (ebur128_loudness_global_multiple over all tracks' states) — so they
// are deliberately NOT part of this interface.
//
// The implementations (FlacEncoder, Mp3Encoder) are verbatim transliterations
// of the inline encode code that lived in CDRipper::ripTrack; their output is
// byte-identical by construction and pinned by rip_encoder_seam_test, which
// diffs them against a frozen copy of the original inline sequence on a fixed
// PCM fixture. Settings are compile-time literals, exactly as they were inline
// (config-driven quality arrives with the format-selection slice).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct IEncoder {
    virtual ~IEncoder() = default;

    // total_frames: FLAC's total_samples_estimate hint (STREAMINFO); 0 = unknown.
    // On failure the encoder has released its own resources; any partial file
    // it created is left on disk (the caller's failure path removes files —
    // ripTrack's contract, unchanged).
    virtual bool open(const std::string& path, uint64_t total_frames) = 0;

    // Interleaved s16 stereo 44.1 kHz. Read-only view of the caller's block,
    // valid only for the duration of the call — implementations stage into
    // their own buffers.
    virtual bool writeFrames(const int16_t* interleaved, size_t frames) = 0;

    // ok=false skips quality-tail work (e.g. LAME's flush + Xing rewrite) but
    // still closes handles/state — mirroring the original inline behavior.
    // Best-effort: finish/flush results are not checked, as before.
    virtual void finalize(bool ok) = 0;
};
