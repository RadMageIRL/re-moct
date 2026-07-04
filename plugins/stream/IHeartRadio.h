#pragma once

#include <string>
#include <functional>
#include <memory>

#include "core/IHttp.h"   // persistent HTTP session via the platform seam (slice 4)

// ─────────────────────────────────────────────────────────────────────────────
// IHeartRadio — isolated iHeart now-playing service.
//
// Resolves a revma zc#### stream URL to an iHeart station id + trackHistory
// endpoint (sidecar-cached, self-resolving on a miss), then polls for the
// current track with timestamp gating (blank during ad/talk breaks).
//
// Self-contained: owns its HTTP session (a keep-alive core::IHttpSession via the
// platform seam — WinINet-free since slice 4), depends only on json.hpp + the seam
// + the standard library. Used by StreamSource (production) and the standalone
// SniffIHeartRadio probe (diagnostic). All iHeart-specific quirks live HERE, so
// when iHeart changes their API only this one file needs to change.
//
// Verified endpoints (SniffIHeartRadio against zc4366 / zc1469 / zc4257, 2026-06-24):
//   identity:    GET api.iheart.com/api/v2/content/liveStations/<id>  -> hits[0].name
//   now-playing: GET api.iheart.com/api/v3/live-meta/stream/<id>/trackHistory -> data[0]
//   The zc#### number IS the iHeart station id (confirmed across 3 stations:
//   4366, 1469, 4257 -- each liveStations.hits[0].id matched the zc number).
//   Dead ends, do not re-probe:
//     - currentTrackMeta -> HTTP 410 ("No meta data for <id>") on BOTH
//       api.iheart.com AND us.api.iheart.com. Platform-wide, not per-station.
//     - track-history (hyphenated) -> HTTP 404. Only camelCase trackHistory resolves.
//
// Failure posture: any network/parse failure or stale window -> empty result, so
// the caller simply shows "(live stream)". Never throws out of its public API.
// ─────────────────────────────────────────────────────────────────────────────
class IHeartRadio {
public:
    // HTTP transport injected (Phase 4 slice c): plugin::HostServiceHttp over the
    // host services in the plugin; core::http() for in-tree callers. Was the
    // core::http() global inside ensureSession — unreachable from a loaded .so.
    explicit IHeartRadio(core::IHttp& http);
    ~IHeartRadio();
    IHeartRadio(const IHeartRadio&)            = delete;
    IHeartRadio& operator=(const IHeartRadio&) = delete;

    // Optional diagnostic logger (probe -> its log; RE-MOCT -> slog). No-op if unset.
    void setLogger(std::function<void(const std::string&)> fn) { log_ = std::move(fn); }

    // Is this a revma/iHeart HLS URL we can resolve? (host revma.ihrhls.com + /zc)
    static bool isIHeartUrl(const std::string& url);

    // Resolve identity: sidecar-first, self-resolve on miss (then cache). Returns
    // true if a usable station id + meta endpoint is available. Call once per open.
    bool resolve(const std::string& url);

    // Poll now-playing. Returns "Artist - Title" if a track is genuinely playing
    // now (timestamp-gated), else "" (ad/talk break or error). Caller throttles.
    // Poll now-playing. Returns "Artist - Title" for data[0] (the most recent track),
    // or "" on error/no data. If endedSecsAgo is given, it's set to (now - endTime):
    // <=0 currently playing, >0 ended that many seconds ago, -1 unknown — and the
    // raw song is returned regardless of staleness so the caller can judge freshness.
    // With no out-param (probe path) a strict 30s gate applies and stale -> "".
    std::string pollNowPlaying(long* endedSecsAgo = nullptr);

    // Structured live now-playing from the currentTrackMeta endpoint — the source
    // the iHeart WEB PLAYER itself uses. Distinct from trackHistory: it reports the
    // CURRENT track (not a history log) with start/end epoch times + album art, and
    // (pending verification) stays fresh through windows where trackHistory freezes.
    // NOTE: the bare currentTrackMeta path returns HTTP 410 ("No meta data"); adding
    // ?defaultMetadata=true flips it to 200 — which is why the earlier "dead end"
    // verdict was a false negative. Epochs arrive in MILLISECONDS here (trackHistory
    // uses seconds), normalised to epoch seconds below. ok=false on HTTP/parse/empty.
    struct CurrentTrack {
        bool        ok            = false;
        long        httpStatus    = 0;
        std::string artist, title, album;
        long long   trackId       = 0;
        long long   startSec      = 0;   // normalised epoch seconds
        long long   endSec        = 0;
        long        durationSec   = 0;
        long long   endedSecsAgo  = -1;  // now - endSec (<=0 playing, >0 ended N ago, -1 unknown)
        std::string imagePath;
        std::string dataSource;
    };

    // Poll currentTrackMeta?defaultMetadata=true for the resolved station. Returns
    // true and fills *out on a 200 with a usable (artist/title) payload. Uses the
    // same WinINet session; never throws out of its public API.
    bool pollCurrentTrackMeta(CurrentTrack* out);

    bool        resolved()    const { return resolved_; }
    long        stationId()   const { return station_id_; }
    std::string stationName() const { return station_name_; }   // iHeart 'name', "" if unknown

    static std::string sidecarPath();   // %TEMP%\re-moct-iheart-stations.json

private:
    // Injected HTTP transport (slice c). Non-owning; outlives us. Declared before
    // session_ so the ctor init list stays in declaration order.
    core::IHttp* http_ = nullptr;
    // Persistent keep-alive session (own UA + deliberate 5s timeouts; see ensureSession).
    std::unique_ptr<core::IHttpSession> session_;
    bool        resolved_   = false;
    long        station_id_ = 0;
    std::string zc_;                    // "zc4366"
    std::string resolved_url_;          // url that resolved_ currently reflects (station-switch guard)
    std::string meta_url_;              // trackHistory endpoint
    std::string station_name_;          // iHeart 'name' (e.g. "92.5 The Breeze")
    long        accepted_max_start_ = 0; // monotonic guard: highest trackHistory startTime accepted; reset on station switch

    std::function<void(const std::string&)> log_;
    void logmsg(const std::string& s) const { if (log_) log_(s); }

    bool ensureSession();
    bool httpGet(const std::string& url, std::string& body, long& status);
    bool selfResolve(const std::string& url);   // liveStations + build meta_url
    bool readSidecar(const std::string& zc);    // load cached identity
    void writeSidecar();                        // persist identity (protects user names)

    static std::string tempDir();
    static bool extractZc(const std::string& url, std::string& zc, long& num);
};

