#ifdef _WIN32

#include "IHeartRadio.h"
#include "json.hpp"

#include <windows.h>
#include <wininet.h>

#include <fstream>
#include <ctime>
#include <cstdlib>
#include <cstring>

using json = nlohmann::json;

// ── Construction ─────────────────────────────────────────────────────────────
IHeartRadio::IHeartRadio() = default;

IHeartRadio::~IHeartRadio() {
    if (hInet_) { InternetCloseHandle((HINTERNET)hInet_); hInet_ = nullptr; }
}

// ── Paths ────────────────────────────────────────────────────────────────────
std::string IHeartRadio::tempDir() {
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0 || n > MAX_PATH) return ".\\";
    return std::string(buf, n);
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

// ── WinINet ──────────────────────────────────────────────────────────────────
bool IHeartRadio::ensureSession() {
    if (hInet_) return true;
    HINTERNET h = InternetOpenA(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RE-MOCT/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!h) { logmsg("iheart: InternetOpenA failed err=" + std::to_string(GetLastError())); return false; }
    DWORD to = 5000;   // shorter than a stream connect: this polls on the audio
                       // producer thread, so a hung iHeart connection mustn't
                       // stall playback for long — skip and retry next cycle.
    InternetSetOptionA(h, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(h, INTERNET_OPTION_SEND_TIMEOUT,    &to, sizeof(to));
    hInet_ = h;
    return true;
}

bool IHeartRadio::httpGet(const std::string& url, std::string& body, long& status) {
    body.clear(); status = 0;
    if (!ensureSession()) return false;
    const char* headers = "Accept: application/json\r\n";
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION |
                  INTERNET_FLAG_SECURE;
    HINTERNET h = InternetOpenUrlA((HINTERNET)hInet_, url.c_str(), headers, (DWORD)-1L, flags, 0);
    if (!h) { logmsg("iheart: GET open failed err=" + std::to_string(GetLastError()) + " " + url); return false; }

    DWORD code = 0, clen = sizeof(code), idx = 0;
    if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, &idx))
        status = (long)code;

    char chunk[8192]; DWORD got = 0;
    while (InternetReadFile(h, chunk, sizeof(chunk), &got) && got > 0) {
        body.append(chunk, got);
        if (body.size() > 4u * 1024u * 1024u) break;
    }
    InternetCloseHandle(h);
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
        localtime_s(&tmv, &t);
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
    resolved_ = false;
    std::string zc; long num = 0;
    if (!extractZc(url, zc, num)) { logmsg("iheart: resolve - not an iHeart url"); return false; }
    zc_ = zc;
    if (readSidecar(zc)) { resolved_ = true; return true; }   // sidecar first
    return selfResolve(url);                                  // self-resolve on miss
}

// ── Public: poll now-playing (timestamp-gated) ───────────────────────────────
std::string IHeartRadio::pollNowPlaying() {
    if (!resolved_ || meta_url_.empty()) return {};
    std::string body; long st = 0;
    if (!httpGet(meta_url_, body, st) || st != 200) {
        logmsg("iheart: trackHistory HTTP " + std::to_string(st));
        return {};
    }
    json j;
    try { j = json::parse(body); } catch (...) { return {}; }
    if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) return {};

    const json& t0 = j["data"][0];   // newest-first: data[0] is most recent track
    long startTime = t0.value("startTime", 0L);
    long endTime   = t0.value("endTime",   0L);
    long dur       = t0.value("trackDuration", 0L);
    if (endTime == 0 && startTime > 0 && dur > 0) endTime = startTime + dur;

    // Stale gate. iHeart's trackHistory LAGS the live audio by ~75-110s observed
    // (logged across real song changes on zc4366): when a song ends, data[0] sits
    // "ended N seconds ago" for over a minute before the next song registers, even
    // though music is playing the whole time. A tight gate blanks the title on
    // every transition. So keep the last song up across that lag (150s grace) and
    // only blank for a genuinely long gap (real ad/talk break ran 195s+ stale).
    // Tunable: raise if transitions still flicker, lower if breaks show stale too long.
    const long GRACE = 150;
    long now = (long)std::time(nullptr);
    if (endTime > 0 && now > endTime + GRACE) {
        logmsg("iheart: data[0] stale (ended " + std::to_string(now - endTime) + "s ago) -> live stream");
        return {};
    }

    std::string artist = t0.value("artist", std::string());
    std::string title  = t0.value("title",  std::string());
    if (artist.empty() && title.empty()) return {};
    std::string combined = artist.empty() ? title
                         : (title.empty() ? artist : artist + " - " + title);
    return combined;
}

#endif // _WIN32
