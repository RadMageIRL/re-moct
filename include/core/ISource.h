// ISource.h — the internal Source interface (Phase 2, slice A).
//
// `core::ISource` is the compile-time playback-source contract: the frame-
// feeding surface the audio callback consumes, plus the generic transport
// queries the UI needs. Statically linked, NOT a plugin ABI — hardening this
// into a loadable C-ABI boundary is Phase 4, and Phase-4 shapes must not leak
// in here. It models the REAL sources this codebase has (CD and stream in
// slice A; local file joins in slice B), not a hypothetical future plugin.
//
// What made a uniform frame contract honest rather than aspirational: every
// playback path already converged on fixed 44100 Hz / stereo / f32 output, and
// StreamSource::readFrames was deliberately written to CDSource's exact shape.
//
// Deliberate EXCLUSIONS (decided 2026-07-02; rationale in roadmap.md's
// Decisions log — if honoring these ever requires pretending, that is a
// stop-and-redesign signal, not a push-through):
//   - open() is NOT here. The three real open shapes are irreconcilable:
//     file(path) is synchronous; stream(url) is slow and owned by
//     AudioManager's async-connect machinery; CD is a two-level container
//     (open(drive) + playTrack(n)). Construction/open stays concrete —
//     an ISource is what you hold AFTER a source is producing.
//   - Metadata is NOT here. It is the least-uniform facet (file: full TagLib
//     upfront; CD: TOC + external lookup; stream: time-varying now-playing).
//     Grow it later if a real consumer needs it (the RedirectPolicy precedent).
//   - pause is NOT here: device-level for the pull path, a source flag for the
//     push sources — different mechanisms, honest split.
// CD's extended surface (TOC, drive offset/model, stopReader, media checks —
// consumed by CDRipper and UIManager) stays concrete on CDSource beside this
// interface.
//
// This header must NOT include <windows.h> or any platform header — the same
// "does core stay portable" test as the Phase 1 seams.
#pragma once
#include <cstdint>

namespace core {

// What a source actually supports — declared, never faked. A consumer branches
// on these instead of assuming one source's shape fits another.
struct SourceCaps {
    bool seekable = false;   // seekTo() is honored
    bool finite   = false;   // has an end; durationSec() is meaningful
    bool live     = false;   // real-time: a starved ring means buffering (the
                             // source self-heals), never end-of-source
};

class ISource {
public:
    virtual ~ISource() = default;

    // THE audio-callback contract. Fill dst with frame_count stereo float
    // frames at 44100 Hz and return the number of frames written. Ring-backed
    // sources (CD, stream) ALWAYS return frame_count, padding silence while
    // paused/buffering/underrun/stopped — their end is observable via their own
    // state, never via a short read. A pull-decoded source (local file,
    // slice B) returns the frames actually produced; a short return means the
    // source is permanently exhausted and the caller pads the remainder.
    // Called on the audio thread: implementations must be lock-free and must
    // not block on this path.
    virtual uint32_t readFrames(float* dst, uint32_t frame_count) = 0;

    virtual SourceCaps caps() const = 0;

    // Transport queries/commands — UI/main-thread surface.
    virtual double positionSec() const = 0;    // live sources: wall-clock since audio first flowed
    virtual double durationSec() const = 0;    // 0.0 when unknown (live sources)
    virtual bool   seekTo(double seconds) = 0; // false when !caps().seekable or nothing is playing
    virtual void   close() = 0;                // idempotent; stops/joins the source's machinery
};

} // namespace core
