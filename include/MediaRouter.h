// MediaRouter.h - the OS-media command marshal + split sink (osmedia-seam).
//
// OS transport commands arrive OFF the UI thread (SMTC on a WinRT threadpool
// thread; MPRIS during the bus pump). They MUST NOT touch playback directly.
// post() is the thread-safe enqueue the IMediaControl command handler calls;
// drain() runs on the UI thread once per main-loop tick and dispatches each
// queued command to an injected sink - the SAME functions the keyboard calls
// (no parallel transport layer). Standalone + injectable so the routing and the
// marshal are unit-testable without a UIManager (the recorder-engine pattern).
#pragma once
#include "core/IMediaControl.h"

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

class MediaRouter {
public:
    // The split sink: each wired to an existing home by the consumer.
    // seekAbs/seekRel go to the coalescer (never a raw seekTo).
    struct Sinks {
        std::function<void()>       play, pause, togglePause, stop, next, prev;
        std::function<void(double)> seekAbs;   // absolute target seconds
        std::function<void(double)> seekRel;   // relative offset seconds
    };
    void setSinks(Sinks s) { sinks_ = std::move(s); }

    // Thread-safe enqueue (any thread). This is what the IMediaControl handler calls.
    void post(core::MediaEvent e) {
        std::lock_guard<std::mutex> lk(mtx_);
        q_.push_back(e);
    }

    // UI thread: apply every queued command via the sinks. Drains under the lock
    // into a local first, then dispatches unlocked (a sink may re-enter nothing,
    // but keeping dispatch outside the lock is the safe discipline).
    void drain() {
        std::vector<core::MediaEvent> batch;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (q_.empty()) return;
            batch.assign(q_.begin(), q_.end());
            q_.clear();
        }
        for (const core::MediaEvent& e : batch) dispatch(e);
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.empty();
    }

    // Drop all queued commands without applying (the consumer gates transport in
    // states where a background action would break an invariant, e.g. an active
    // CD rip owning the drive). Discard, not defer: a key pressed during a long
    // rip should not fire when the rip ends.
    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        q_.clear();
    }

private:
    void dispatch(const core::MediaEvent& e) {
        using C = core::MediaCommand;
        switch (e.cmd) {
            case C::Play:        if (sinks_.play)        sinks_.play();        break;
            case C::Pause:       if (sinks_.pause)       sinks_.pause();       break;
            case C::TogglePause: if (sinks_.togglePause) sinks_.togglePause(); break;
            case C::Stop:        if (sinks_.stop)        sinks_.stop();        break;
            case C::Next:        if (sinks_.next)        sinks_.next();        break;
            case C::Previous:    if (sinks_.prev)        sinks_.prev();        break;
            case C::SeekToAbs:   if (sinks_.seekAbs)     sinks_.seekAbs(e.seconds); break;
            case C::SeekByRel:   if (sinks_.seekRel)     sinks_.seekRel(e.seconds); break;
        }
    }

    mutable std::mutex           mtx_;
    std::deque<core::MediaEvent> q_;
    Sinks                        sinks_;
};
