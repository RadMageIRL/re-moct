#pragma once
#ifdef _WIN32

#include <string>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// IHeartRadio — isolated iHeart now-playing service.
//
// Resolves a revma zc#### stream URL to an iHeart station id + trackHistory
// endpoint (sidecar-cached, self-resolving on a miss), then polls for the
// current track with timestamp gating (blank during ad/talk breaks).
//
// Self-contained: owns its WinINet session, depends only on json.hpp + WinINet
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
    IHeartRadio();
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

    bool        resolved()    const { return resolved_; }
    long        stationId()   const { return station_id_; }
    std::string stationName() const { return station_name_; }   // iHeart 'name', "" if unknown

    static std::string sidecarPath();   // %TEMP%\re-moct-iheart-stations.json

private:
    void*       hInet_      = nullptr;  // HINTERNET (own session)
    bool        resolved_   = false;
    long        station_id_ = 0;
    std::string zc_;                    // "zc4366"
    std::string resolved_url_;          // url that resolved_ currently reflects (station-switch guard)
    std::string meta_url_;              // trackHistory endpoint
    std::string station_name_;          // iHeart 'name' (e.g. "92.5 The Breeze")

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

#endif // _WIN32
