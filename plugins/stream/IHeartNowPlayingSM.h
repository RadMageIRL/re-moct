// IHeartNowPlayingSM.h — pure iHeart now-playing reconciliation state machine.
//
// Extracted verbatim from StreamSource::updateIHeartNowPlaying so the debounce /
// target-ladder / LIVE-floor-stall decisions are unit-testable off-network.
// PURE: no windows.h, no threads, no network, no audio, no I/O. The caller
// (StreamSource) owns everything impure — manifest classification, the throttled
// HTTP polls, the cross-thread now_playing_/np_pub_q_ publish, the re-pin
// armed/cooldown/pending atomics, album art, and the deep log. This unit only
// consumes already-parsed values and returns a decision.
#pragma once
#include <string>
#include <cstdint>

// Newest-manifest-segment class. classifyIHeartManifest stays in StreamSource.cpp
// and feeds its result in; this enum is shared from here (moved out of that .cpp).
enum class IHeartMfCls { None, Song, Ad };

// Committed now-playing state (moved out of StreamSource.h).
enum class IHNow { Live, Song, Ad };

inline const char* ihNowName(IHNow s) {
    return s == IHNow::Song ? "Song" : (s == IHNow::Ad ? "Ad" : "Live");
}

// One reconciliation tick's inputs, all produced by the caller from the manifest
// body + the (throttled) trackHistory / currentTrackMeta polls.
struct IHeartTick {
    uint32_t    nowMs           = 0;     // GetTickCount(), captured once for this tick
    IHeartMfCls mfCls           = IHeartMfCls::None;
    std::string mfSong;                  // "artist - title" when mfCls==Song, else ""
    std::string thSong;                  // trackHistory cached song ("" if none)
    long        thEnded         = -1;    // trackHistory staleness (now-endTime; -1 unknown)
    bool        digitalActive   = false;
    bool        ctmOk           = false; // currentTrackMeta poll succeeded
    long        ctmStatus       = 0;
    std::string ctmArtist;
    std::string ctmTitle;
    long        ctmEndedSecsAgo = -1;
    std::string stationName;             // iHeart 'name' ("" if unknown)
    bool        repinArmed      = false; // caller's hls_repin_armed_ (shared with disc path)
};

// One tick's decision. Carries the pre-tick snapshot + computed target so the
// caller's deep log stays byte-identical, plus the results the caller acts on.
struct IHeartDecision {
    // pre-tick committed/pending snapshot (deep log: stState/stDisp/pendKind/pendDisp/streak)
    IHNow       stKind   = IHNow::Live; std::string stDisp;
    IHNow       pendKind = IHNow::Live; std::string pendDisp;
    int         streak   = 0;
    // this tick's computed target (deep log: tgtKind/tgtDisp/thCurrent)
    IHNow       tgtKind  = IHNow::Live; std::string tgtDisp;
    bool        thCurrent = false;
    // results the caller acts on
    bool        committed = false;            // a new label committed this tick
    IHNow       newKind  = IHNow::Live; std::string newDisp;  // RAW (caller sanitizes)
    bool        liveStallFired    = false;    // caller fires the live-edge re-pin
    uint32_t    liveStallElapsedMs = 0;       // for the caller's slog only
};

class IHeartNowPlayingSM {
public:
    // The whole reconciliation tick. No side effects beyond advancing internal state.
    IHeartDecision tick(const IHeartTick& in);

    // Connect reset — exactly StreamSource.cpp:424-425 (deliberately does NOT touch
    // the floor timer, matching the original).
    void reset();

    // Committed state, for the caller's album-art gate (which reads the *previous*
    // commit, before this tick runs).
    IHNow              state()     const { return state_; }
    const std::string& stateDisp() const { return stateDisp_; }

private:
    // How long the reconciler may sit on the LIVE floor (digital) before we assume a
    // stall and force a live-edge re-pin. Healthy ad breaks clear in <=~30s; a real
    // stall ran 7 min; 35s sits just above the healthy ceiling. (== old StreamSource.cpp:21)
    static constexpr uint32_t LIVE_STALL_MS = 35000;

    IHNow       state_      = IHNow::Live;
    std::string stateDisp_;
    IHNow       pending_    = IHNow::Live;
    std::string pendingDisp_;
    int         streak_     = 0;
    uint32_t    liveSince_  = 0;            // was ih_live_since_
};
