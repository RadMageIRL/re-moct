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
// Verified endpoints (SniffIHeartRadio against zc4366, 2026-06-24):
//   identity:    GET api.iheart.com/api/v2/content/liveStations/<id>  -> hits[0].name
//   now-playing: GET api.iheart.com/api/v3/live-meta/stream/<id>/trackHistory -> data[0]
//   (the zc#### number IS the iHeart station id; currentTrackMeta returns 410 here.)
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
    std::string pollNowPlaying();

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
