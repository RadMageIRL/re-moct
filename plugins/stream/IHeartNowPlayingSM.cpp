#include "IHeartNowPlayingSM.h"

void IHeartNowPlayingSM::reset() {
    state_   = pending_ = IHNow::Live;     // StreamSource.cpp:424
    stateDisp_.clear(); pendingDisp_.clear(); streak_ = 0;   // :425
}

IHeartDecision IHeartNowPlayingSM::tick(const IHeartTick& in) {
    IHeartDecision d;
    // ── pre-tick snapshot (deep log records pre-debounce state) ──
    d.stKind   = state_;   d.stDisp   = stateDisp_;
    d.pendKind = pending_; d.pendDisp = pendingDisp_;
    d.streak   = streak_;

    // ── trackHistory currency (StreamSource.cpp:621-630) ──
    const long CUR   = 60;
    const long thMax = (state_ == IHNow::Ad) ? 0 : CUR;   // Ad: only accept actively-playing
    const bool thCurrent = !in.thSong.empty() && in.thEnded <= thMax;
    d.thCurrent = thCurrent;

    // ── ctm-confirmed live song, digital only (StreamSource.cpp:639-644) ──
    std::string ctmSong;
    if (in.digitalActive && in.ctmOk && in.ctmStatus == 200 &&
        !in.ctmTitle.empty() && in.ctmEndedSecsAgo <= 0) {
        ctmSong = in.ctmArtist.empty() ? in.ctmTitle
                                       : (in.ctmArtist + " - " + in.ctmTitle);
    }

    // ── confidence-ordered target (StreamSource.cpp:646-658) ──
    const std::string& st = in.stationName;
    IHNow tgtKind; std::string tgtDisp;
    if (!in.mfSong.empty())               { tgtKind = IHNow::Song; tgtDisp = in.mfSong; }
    else if (!ctmSong.empty())            { tgtKind = IHNow::Song; tgtDisp = ctmSong; }
    else if (in.mfCls == IHeartMfCls::Ad) { tgtKind = IHNow::Ad;
        tgtDisp = st.empty() ? std::string("Commercial break")
                             : (st + " - Commercial break"); }
    else if (thCurrent)                   { tgtKind = IHNow::Song; tgtDisp = in.thSong; }
    else                                  { tgtKind = IHNow::Live;
        tgtDisp = st.empty() ? std::string("LIVE") : (st + " - LIVE"); }
    d.tgtKind = tgtKind; d.tgtDisp = tgtDisp;

    // ── LIVE-floor stall self-heal (StreamSource.cpp:746-760) ──
    // f6-repin-finalize: one uniform floor for every active mode; off (0) never fires.
    // The SM only REPORTS the expiry - the caller gates the fire on ad-evidence.
    if (in.digitalActive && state_ == IHNow::Live) {
        const uint32_t stallMs = REPIN_FLOOR_MS;
        if (liveSince_ == 0) {
            liveSince_ = in.nowMs;
        } else if (in.repinMode != 0 && in.repinArmed &&
                   (in.nowMs - liveSince_) >= stallMs) {
            d.liveStallFired     = true;
            d.liveStallElapsedMs = in.nowMs - liveSince_;
            liveSince_ = 0;                       // restart floor timer after acting
        }
    } else {
        liveSince_ = 0;                           // off the floor (or raw) -> reset timer
    }

    // ── asymmetric debounce: Song 1, Ad 3, Live 2 (StreamSource.cpp:768-775) ──
    if (tgtKind == state_ && tgtDisp == stateDisp_) { streak_ = 0; return d; } // already committed
    if (tgtKind == pending_ && tgtDisp == pendingDisp_) streak_++;
    else { pending_ = tgtKind; pendingDisp_ = tgtDisp; streak_ = 1; }
    const int need = (tgtKind == IHNow::Song) ? 1 : (tgtKind == IHNow::Ad ? 3 : 2);
    if (streak_ < need) return d;                                              // hold current display

    state_ = tgtKind; stateDisp_ = tgtDisp; streak_ = 0;                       // commit
    d.committed = true; d.newKind = tgtKind; d.newDisp = tgtDisp;              // RAW; caller sanitizes
    return d;
}
