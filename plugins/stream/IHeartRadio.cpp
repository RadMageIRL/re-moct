#include "IHeartRadio.h"
#include "core/IHttp.h"
#include "json.hpp"

#include "PortUtil.h"     // tempDir (the GetTempPathA baseline on Windows) — no WinINet since slice 4
#include "StringUtils.h"  // localtimeSafe

#include <fstream>
#include <ctime>
#include <cstdlib>
#include <cstring>

using json = nlohmann::json;

// ── Construction ─────────────────────────────────────────────────────────────
IHeartRadio::IHeartRadio(core::IHttp& http) : http_(&http) {}
IHeartRadio::~IHeartRadio() = default;   // session_ closes itself

// ── Paths ────────────────────────────────────────────────────────────────────
std::string IHeartRadio::tempDir() {
    std::string d = port::tempDir();          // Windows: the GetTempPathA baseline
    return d.empty() ? std::string("./") : d;
}

std::string IHeartRadio::sidecarPath() {
    return tempDir() + "re-moct-iheart-stations.json";
}

// ── URL parsing ──────────────────────────────────────────────────────────────
bool IHeartRadio::extractZc(const std::string& url, std::string& zc, long& num) {
    auto p = url.find("/zc");
    if (p == std::string::npos) return false;
    p += 1;                                   // point at 'z'
    size_t e = p;
    while (e < url.size() && url[e] != '/') ++e;
    zc = url.substr(p, e - p);                // "zc4366"
    std::string digits;
    for (char c : zc) if (c >= '0' && c <= '9') digits += c;
    if (digits.empty()) return false;
    num = std::strtol(digits.c_str(), nullptr, 10);
    return true;
}

bool IHeartRadio::isIHeartUrl(const std::string& url) {
    if (url.find("revma.ihrhls.com") == std::string::npos) return false;
    std::string zc; long n = 0;
    return extractZc(url, zc, n);
}

// ── HTTP (via the core::IHttp seam, slice 4) ─────────────────────────────────
bool IHeartRadio::ensureSession() {
    if (session_) return true;
    core::HttpSessionConfig cfg;
    cfg.user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RE-MOCT/1.0";
    cfg.timeout_ms = 5000;   // shorter than a stream connect: this polls on the audio
                             // producer thread, so a hung iHeart connection mustn't
                             // stall playback for long — skip and retry next cycle.
    session_ = http_->openSession(cfg);   // slice c: injected host services (was core::http())
    if (!session_) { logmsg("iheart: openSession failed"); return false; }
    return true;
}

bool IHeartRadio::httpGet(const std::string& url, std::string& body, long& status) {
    body.clear(); status = 0;
    if (!ensureSession()) return false;
    core::HttpRequest req;
    req.url             = url;
    req.headers         = {{"Accept", "application/json"}};
    req.pragma_no_cache = true;                 // baseline INTERNET_FLAG_PRAGMA_NOCACHE
    req.max_body        = 4u * 1024u * 1024u;   // baseline 4 MB cap-and-keep
    // Deliberately NO cancel token: this poll's boundedness is the session's short
    // timeout, exactly as baseline. (Wiring stop_ in would be a behavior change —
    // parked as a separate decision.)
    core::HttpResponse res = session_->fetch(req);
    status = res.status;
    if (!res.ok) { logmsg("iheart: GET open failed " + url); return false; }
    body = std::move(res.body);   // partial body on a mid-read error is KEPT, as baseline
    return true;
}

// ── Sidecar identity cache ───────────────────────────────────────────────────
bool IHeartRadio::readSidecar(const std::string& zc) {
    std::ifstream in(sidecarPath());
    if (!in) return false;
    json root;
    try { in >> root; } catch (...) { return false; }
    if (!root.is_object() || !root.contains(zc)) return false;
    const json& e = root[zc];
    try {
        station_id_   = e.value("station_id", 0L);
        meta_url_     = e.value("meta_url", std::string());
        station_name_ = e.value("station_name", std::string());
    } catch (...) { return false; }
    if (station_id_ == 0 || meta_url_.empty()) return false;
    logmsg("iheart: sidecar hit " + zc + " id=" + std::to_string(station_id_) +
           " name=\"" + station_name_ + "\"");
    return true;
}

void IHeartRadio::writeSidecar() {
    std::string path = sidecarPath();
    json root = json::object();
    { std::ifstream in(path); if (in) { try { in >> root; } catch (...) { root = json::object(); } } }
    if (!root.is_object()) root = json::object();

    // Protect a user-supplied name: never overwrite name/name_source when the
    // existing entry was tagged "user". Otherwise store the iHeart-resolved name.
    std::string name_to_write   = station_name_;
    std::string source_to_write = "iheart";
    if (root.contains(zc_) && root[zc_].is_object()) {
        const json& prev = root[zc_];
        if (prev.value("name_source", std::string()) == "user") {
            name_to_write   = prev.value("station_name", station_name_);
            source_to_write = "user";
        }
    }

    json entry = json::object();
    entry["station_id"]  = station_id_;
    entry["meta_url"]    = meta_url_;
    if (!name_to_write.empty()) {
        entry["station_name"] = name_to_write;
        entry["name_source"]  = source_to_write;
    }
    {
        std::time_t t = std::time(nullptr); std::tm tmv{};
        localtimeSafe(t, tmv);
        char b[32]; std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tmv);
        entry["resolved_at"] = b;
    }
    root[zc_] = entry;

    std::ofstream out(path, std::ios::trunc);
    if (out) { out << root.dump(2) << "\n"; logmsg("iheart: sidecar written " + zc_); }
    else     logmsg("iheart: sidecar WRITE FAILED " + path);
}

// ── Self-resolve (sidecar miss): confirm id + name via liveStations ──────────
bool IHeartRadio::selfResolve(const std::string& url) {
    long num = 0;
    if (!extractZc(url, zc_, num)) { logmsg("iheart: no zc#### in url"); return false; }
    station_id_ = num;                                 // zc number IS the station id
    std::string id = std::to_string(station_id_);

    // liveStations confirms the id is real and hands us the station name.
    std::string body; long st = 0;
    std::string ls = "https://api.iheart.com/api/v2/content/liveStations/" + id;
    if (httpGet(ls, body, st) && st == 200) {
        try {
            json j = json::parse(body);
            if (j.contains("hits") && j["hits"].is_array() && !j["hits"].empty()) {
                const json& h0 = j["hits"][0];
                station_name_ = h0.value("name", std::string());
            }
        } catch (...) { /* name is optional; keep going */ }
    } else {
        logmsg("iheart: liveStations HTTP " + std::to_string(st) + " (continuing; id from zc)");
    }

    // trackHistory is the verified now-playing source (currentTrackMeta 410s here).
    meta_url_ = "https://api.iheart.com/api/v3/live-meta/stream/" + id + "/trackHistory";
    resolved_ = true;
    logmsg("iheart: self-resolved " + zc_ + " id=" + id + " name=\"" + station_name_ + "\"");
    writeSidecar();
    return true;
}

// ── Public: resolve ──────────────────────────────────────────────────────────
bool IHeartRadio::resolve(const std::string& url) {
    // Already resolved THIS url? Reuse it. (Idempotent + cheap to call every poll.)
    if (resolved_ && url == resolved_url_) return true;
    // Different url (station switch) or first call: re-resolve from scratch so we
    // never keep serving the previous station's meta_url/name.
    resolved_ = false;
    station_id_ = 0; meta_url_.clear(); station_name_.clear();
    accepted_max_start_ = 0;   // new station -> forget the previous schedule's max
    std::string zc; long num = 0;
    if (!extractZc(url, zc, num)) { logmsg("iheart: resolve - not an iHeart url"); return false; }
    zc_ = zc;
    bool ok = readSidecar(zc) ? (resolved_ = true) : selfResolve(url);   // sidecar first, else self-resolve
    if (ok) resolved_url_ = url;
    return ok;
}

// ── Public: poll now-playing (timestamp-gated) ───────────────────────────────
std::string IHeartRadio::pollNowPlaying(long* endedSecsAgo, PollDiag* diag) {
    if (endedSecsAgo) *endedSecsAgo = -1;
    if (diag) { *diag = {}; diag->acceptedMaxStart = accepted_max_start_; }  // ceiling at ENTRY
    if (!resolved_ || meta_url_.empty()) { if (diag) diag->heldBy = "unresolved"; return {}; }
    std::string body; long st = 0;
    if (!httpGet(meta_url_, body, st) || st != 200) {
        logmsg("iheart: trackHistory HTTP " + std::to_string(st));
        if (diag) diag->heldBy = "poll-fail";
        return {};
    }
    json j;
    try { j = json::parse(body); } catch (...) { if (diag) diag->heldBy = "parse"; return {}; }
    if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
        if (diag) { diag->heldBy = "no-data"; diag->entryCount = 0; }
        return {};
    }

    const json& data = j["data"];
    if (diag) diag->entryCount = (int)data.size();
    long now = (long)std::time(nullptr);
    const long FUTURE_GRACE = 60;   // tolerate small clock skew; reject genuinely-future scheduled entries

    // Pick the genuinely newest entry by startTime that has actually aired.
    // data[] is *nominally* newest-first, but a jumbled Pnp feed (e.g. Z100) can
    // resurface an old entry at [0] above newer ones — so scan, don't trust [0].
    int  best = -1;
    long bestStart = 0;
    for (int i = 0; i < (int)data.size(); ++i) {
        long s = data[i].value("startTime", 0L);
        if (diag && s > now + FUTURE_GRACE) ++diag->futureSkipped;  // count only genuinely-future (s>0 implied)
        if (s <= 0 || s > now + FUTURE_GRACE) continue;          // skip undated / not-yet-aired
        if (best < 0 || s > bestStart) { best = i; bestStart = s; }
    }
    if (best < 0) { if (diag) diag->heldBy = "none-aired"; return {}; }  // nothing has aired yet
    if (diag) { diag->chosenIdx = best; diag->newestAiredStart = bestStart; }  // accept path (best final)

    if (best != 0)
        logmsg("iheart: data[0] out-of-order (start=" + std::to_string(data[0].value("startTime", 0L))
             + ") -> using newest idx=" + std::to_string(best) + " start=" + std::to_string(bestStart));

    // Monotonic guard: if even the newest available entry is older than the highest
    // startTime we've already served, the feed has regressed (resurfacing / freeze) —
    // hold rather than rewind to a stale song. Only ever SUPPRESSES a known-bad entry;
    // never invents one. Provable no-op on a clean (monotonic) station like Breeze.
    if (bestStart < accepted_max_start_) {
        logmsg("iheart: trackHistory regressed (newest start=" + std::to_string(bestStart)
             + " < accepted=" + std::to_string(accepted_max_start_) + ") -> hold");
        if (diag) diag->heldBy = "monotonic";   // (acceptedMaxStart, newestAiredStart) = ceiling vs offered
        return {};
    }
    accepted_max_start_ = bestStart;

    const json& t0 = data[best];   // the genuine newest aired track
    long startTime = bestStart;
    long endTime   = t0.value("endTime",   0L);
    long dur       = t0.value("trackDuration", 0L);
    if (endTime == 0 && startTime > 0 && dur > 0) endTime = startTime + dur;

    long ended = (endTime > 0) ? (now - endTime) : -1;   // <=0 playing, >0 ended N ago, -1 unknown
    if (endedSecsAgo) *endedSecsAgo = ended;

    std::string artist = t0.value("artist", std::string());
    std::string title  = t0.value("title",  std::string());
    if (artist.empty() && title.empty()) { if (diag) diag->heldBy = "empty-meta"; return {}; }
    std::string combined = artist.empty() ? title
                         : (title.empty() ? artist : artist + " - " + title);

    // Caller wants the staleness (the reconciliation state machine) -> return the
    // raw song and let it decide freshness. Legacy/probe path (no out-param) keeps
    // the strict 30s gate so -M still reports "(empty)" during a break.
    if (!endedSecsAgo) {
        const long GRACE = 30;
        if (endTime > 0 && ended > GRACE) {
            logmsg("iheart: data[" + std::to_string(best) + "] stale (ended " + std::to_string(ended) + "s ago) -> live stream");
            return {};
        }
    }
    return combined;
}

// ── currentTrackMeta — the iHeart web player's own now-playing source ─────────
// GET us.api.iheart.com/api/v3/live-meta/stream/<id>/currentTrackMeta?defaultMetadata=true
// Returns the CURRENT track (structured: artist/title/album/art + start/end epoch
// in MILLISECONDS). Normalised to epoch seconds; endedSecsAgo lets the caller gate
// freshness exactly like trackHistory. Unauthenticated + station-id-keyed, so it
// generalises across stations. Probe-only for now (caller polls it only while the
// deep-analysis log is enabled), to confirm whether it survives a trackHistory freeze.
bool IHeartRadio::pollCurrentTrackMeta(CurrentTrack* out) {
    if (out) *out = CurrentTrack{};
    if (!resolved_ || station_id_ == 0) return false;

    std::string id  = std::to_string(station_id_);
    std::string url = "https://us.api.iheart.com/api/v3/live-meta/stream/" + id +
                      "/currentTrackMeta?defaultMetadata=true";
    std::string body; long st = 0;
    bool got = httpGet(url, body, st);
    if (out) out->httpStatus = st;
    if (!got || st != 200) {
        logmsg("iheart: currentTrackMeta HTTP " + std::to_string(st));
        return false;
    }

    json j;
    try { j = json::parse(body); } catch (...) { return false; }

    // epoch may be ms (currentTrackMeta) or s — normalise to seconds.
    auto toSec = [](long long v) -> long long {
        return (v > 100000000000LL) ? (v / 1000) : v;
    };

    CurrentTrack c;
    c.httpStatus   = st;
    c.artist       = j.value("artist", std::string());
    c.title        = j.value("title",  std::string());
    c.album        = j.value("album",  std::string());
    c.trackId      = j.value("trackId", 0LL);
    c.startSec     = toSec(j.value("startTime", 0LL));
    c.endSec       = toSec(j.value("endTime",   0LL));
    c.durationSec  = j.value("trackDuration", 0L);
    c.imagePath    = j.value("imagePath", std::string());
    c.dataSource   = j.value("dataSource", std::string());
    if (c.endSec == 0 && c.startSec > 0 && c.durationSec > 0)
        c.endSec = c.startSec + c.durationSec;

    long long now  = (long long)std::time(nullptr);
    c.endedSecsAgo = (c.endSec > 0) ? (now - c.endSec) : -1;
    c.ok           = !(c.artist.empty() && c.title.empty());

    if (out) *out = c;
    return c.ok;
}


