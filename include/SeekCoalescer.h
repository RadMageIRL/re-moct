// SeekCoalescer.h - the seek-coalescing state, extracted from UIManager so both
// the TUI [ / ] keys and OS-media SetPosition/Seek route through ONE home, and
// so the absolute-lane math is unit-testable without a UIManager (osmedia-seam).
//
// Two lanes:
//   relative - [ / ] and MPRIS Seek accumulate a delta (bit-reservoir-friendly
//              coalescing: many taps -> few applied seeks).
//   absolute - an OS scrubber SetPosition sets a TARGET, last-write-wins. It does
//              NOT fold to a delta at receive time: a drag emits a stream of
//              SetPosition events inside one window, and receive-time folding
//              would accumulate them against a stale (unflushed) position
//              (30-then-60 would land at 80). The target is resolved to a delta
//              at FLUSH time against the LIVE position instead.
//
// Precedence: if both lanes are dirty in one window the ABSOLUTE target wins -
// it is the definitive "go here" and supersedes any accumulated relative delta.
// The resolved value is always a DELTA the caller applies via its one seekBy()
// home (never a raw seekTo), so the MP3 bit-reservoir fix always applies.
//
// A track stamp guards a buffered seek from leaking onto a new track (n/p or
// auto-advance landing inside the window): resolve() drops the seek if the
// current track no longer matches the stamp taken when the lane went dirty.
#pragma once
#include <optional>

class SeekCoalescer {
public:
    bool pending() const { return dirty_; }

    // [ / ] and MPRIS Seek. `track` = the playlist index the seek belongs to.
    void addRelative(double delta, int track) {
        if (!dirty_) stamp_ = track;
        rel_ += delta;
        dirty_ = true;
    }
    // OS SetPosition. Last-write-wins; supersedes the accumulated relative delta.
    void setAbsolute(double target, int track) {
        if (!dirty_) stamp_ = track;
        abs_ = target;
        has_abs_ = true;
        rel_ = 0.0;                    // an absolute "go here" clears pending deltas
        dirty_ = true;
    }

    // Resolve the pending seek to a DELTA to apply, or nullopt when nothing is
    // pending OR the track changed since the seek was requested (drop it).
    // `live_pos` is only consulted for the absolute lane (target - live_pos).
    std::optional<double> resolve(int current_track, double live_pos) {
        if (!dirty_) return std::nullopt;
        const bool same = (current_track == stamp_);
        const double d = has_abs_ ? (abs_ - live_pos) : rel_;
        clear();
        return same ? std::optional<double>(d) : std::nullopt;
    }

    void clear() { rel_ = 0.0; abs_ = 0.0; has_abs_ = false; dirty_ = false; }

private:
    double rel_ = 0.0;         // accumulated relative delta
    double abs_ = 0.0;         // absolute target (valid when has_abs_)
    bool   has_abs_ = false;   // absolute lane wins over relative when set
    bool   dirty_ = false;
    int    stamp_ = 0;         // track index the pending seek is tied to
};
