#pragma once
// Queue-aware preload (XF C3): ONE authority on "what plays after the current
// track", plus the per-tick reconciliation that keeps AudioManager's armed next
// decoder converged on it.
//
// Before this, fifteen call sites armed the next track ad hoc and four more
// cleared it, each encoding its own copy of the precedence rules - and the queue
// was excluded entirely, so every queue jump was a hard cut. Now the precedence
// lives here once, and a poll (UIManager::reconcileNextArm, each run-loop tick
// beside pollEvents) converges the armed state onto it after ANY queue or
// playlist mutation. Nothing to remember at the mutation sites; worst-case
// staleness is one ~80ms tick against a 2s fade.
//
// Header-inline per the repo norm (StringUtils.h): the logic links into tests
// without dragging UIManager's curses stack. UI THREAD ONLY.
#include <string>
#include "AudioManager.h"
#include "PlaylistManager.h"
#include "StringUtils.h"

// The authoritative next path, or "" when nothing should be armed:
//   1. repeat-one       -> nothing (the loop replays the current track; arming
//                          would either bleed it in or waste a decoder - see C2)
//   2. queue non-empty  -> the queue HEAD (FIFO: only the head can play next,
//                          so stacking more queue entries never invalidates it)
//   3. otherwise        -> peekNext() (shuffle/repeat-all aware)
//   4. CD and stream paths -> nothing (neither can be preloaded as a decoder;
//                          those transitions stay hard cuts by design)
inline std::string resolveNextPath(const PlaylistManager& pl) {
    if (pl.repeatMode() == RepeatMode::One) return {};
    if (!pl.queueEmpty()) {
        const std::string& q = pl.queueAt(0).path;
        if (isCDTrackPath(q) || isStreamPath(q)) return {};
        return q;
    }
    if (auto peek = pl.peekNext(); peek.has_value())
        if (!isCDTrackPath(*peek) && !isStreamPath(*peek))
            return *peek;
    return {};
}

// One reconciliation step: converge the armed decoder on resolveNextPath(pl).
//
// armed_next / failed_next are caller-owned UI-thread mirrors: armed_next is the
// last path handed to preloadNext (NEVER read back from AudioManager - its
// next_path_ is audio-thread-mutated, a cross-thread string read); failed_next
// latches a path whose preload failed so an unopenable file is not re-opened and
// TagLib-parsed every tick - it retries only once the resolver's answer changes.
//
// The isCrossfading() guard: a committed fade stays committed. Re-arming through
// clearNext() mid-fade takes the device stop/start quiesce (an audible click) -
// so a mutation that changes the answer mid-fade waits out the fade; the next
// tick after completion converges. Same philosophy as the q-mid-fade hard cut.
inline void reconcileNextArm(AudioManager& audio, const PlaylistManager& pl,
                             std::string& armed_next, std::string& failed_next) {
    if (audio.isCrossfading()) return;
    const std::string desired = resolveNextPath(pl);
    if (desired.empty()) {
        if (audio.hasNextArmed()) audio.clearNext();
        armed_next.clear(); failed_next.clear();
        return;
    }
    if (desired == failed_next) return;
    if (audio.hasNextArmed() && armed_next == desired) return;
    // preloadNext runs teardownNext first, so a wrong armed track is replaced in
    // the same call - no separate clearNext round-trip.
    if (audio.preloadNext(desired)) { armed_next = desired; failed_next.clear(); }
    else                            { failed_next = desired; armed_next.clear(); }
}
