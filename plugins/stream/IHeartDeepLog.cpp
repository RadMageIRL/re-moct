#include "IHeartDeepLog.h"

// Portable since Phase 3 slice 1: Win32 file/time calls became std::filesystem /
// chrono / localtimeSafe with the behavior contract unchanged — JSONL capture
// files `remoct-deep-analysis-YYYYMMDD-HHMMSS.log` under the per-platform app
// logs dir (Windows: %APPDATA%\RE-MOCT\logs\, the exact baseline; Linux:
// $XDG_STATE_HOME/re-moct/logs/), 5 MB size roll, kKeepDays retention by file
// mtime, 30 s heartbeat + semantic-dedup writes, uint32 tick stamps
// (port::tickMs == the baseline GetTickCount on Windows).
#include "PortUtil.h"
#include "StringUtils.h"   // localtimeSafe
#include "json.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ─── Tunables ────────────────────────────────────────────────────────────────
constexpr unsigned long long kMaxBytes   = 5ull * 1024 * 1024;  // size roll at ~5 MB
constexpr uint32_t           kHeartbeatMs = 30000;               // forced record cadence
constexpr int               kKeepDays    = 5;                    // retention (days)
constexpr int               kSchema      = 2;   // v2: + identity probe fields (idVariant/idProfileTail/idMintOk)
const char* const           kPrefix      = "remoct-deep-analysis-";
const char* const           kSuffix      = ".log";

// ─── State ───────────────────────────────────────────────────────────────────
// Only the producer thread calls emit(); the UI thread only flips g_enabled via
// toggle(). All filesystem + dedup state below is touched under g_mtx.
std::atomic<bool>  g_enabled{ false };
std::mutex         g_mtx;
std::string        g_cur_path;          // active capture file ("" => start fresh on next emit)
unsigned long long g_seq            = 0;
std::string        g_last_sig;          // dedup signature of last written record
uint32_t           g_last_write_tick = 0;
bool               g_file_started    = false;  // _meta written for g_cur_path?

// ─── Helpers ─────────────────────────────────────────────────────────────────
// Windows: %APPDATA%\RE-MOCT\logs\ — the baseline location (deep captures live
// with the app's data, not in %TEMP% like the operational log). Linux:
// $XDG_STATE_HOME/re-moct/logs/ (fallback ~/.local/state/re-moct/logs/).
std::string logsDir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) return std::string();
    std::string base = std::string(appdata) + "\\RE-MOCT";
    std::error_code ec;
    fs::create_directories(fs::path(base + "\\logs"), ec);   // ok if it already exists
    if (ec) return std::string();
    return base + "\\logs\\";
#else
    std::string base;
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h)
        base = std::string(h) + "/.local/state";
    else return std::string();
    std::string logs = base + "/re-moct/logs";
    std::error_code ec;
    fs::create_directories(fs::path(logs), ec);
    if (ec) return std::string();
    return logs + "/";
#endif
}

std::string stampCompact() {             // "YYYYMMDD-HHMMSS" (local)
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    if (!localtimeSafe(t, tmv)) return "19700101-000000";
    char b[40];
    std::snprintf(b, sizeof(b), "%04d%02d%02d-%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

std::string nowLocal() {                 // "YYYY-MM-DD HH:MM:SS.mmm" (local)
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()).count() % 1000);
    std::tm tmv{};
    if (!localtimeSafe(t, tmv)) return "1970-01-01 00:00:00.000";
    char b[48];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return b;
}

std::string nowUtcIso() {                // "YYYY-MM-DDTHH:MM:SS.mmmZ" (UTC)
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()).count() % 1000);
    std::tm tmv{};
#ifdef _WIN32
    if (gmtime_s(&tmv, &t) != 0) return "1970-01-01T00:00:00.000Z";
#else
    if (!gmtime_r(&t, &tmv)) return "1970-01-01T00:00:00.000Z";
#endif
    char b[48];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return b;
}

unsigned long long fileSize(const std::string& p) {
    std::error_code ec;
    auto sz = fs::file_size(fs::path(p), ec);
    return ec ? 0ull : (unsigned long long)sz;
}

// Delete capture files whose last-write time is older than kKeepDays. The active
// file is never deleted. Caller holds g_mtx. Non-throwing (error_code overloads).
void trimOld(const std::string& dir) {
    const auto cutoff = fs::file_time_type::clock::now()
                      - std::chrono::hours(24 * kKeepDays);
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string n = it->path().filename().string();
        const size_t plen = std::strlen(kPrefix), slen = std::strlen(kSuffix);
        if (n.size() <= plen + slen
            || n.compare(0, plen, kPrefix) != 0
            || n.compare(n.size() - slen, slen, kSuffix) != 0) continue;
        std::error_code tec;
        auto wt = fs::last_write_time(it->path(), tec);
        if (tec || wt >= cutoff) continue;
        std::string full = it->path().string();
        if (full == g_cur_path) continue;
        fs::remove(it->path(), tec);
    }
}

void startNewFile(const std::string& dir) {
    g_cur_path     = dir + kPrefix + stampCompact() + kSuffix;
    g_file_started = false;
}

// Dedup signature: SEMANTIC fields only. Excludes thEnded / audioSec / posSec /
// mfSeq / timestamps — all of which advance every tick during a freeze and would
// otherwise defeat the heartbeat collapse. A freeze == every field below constant.
std::string sigOf(const IHeartDeepLog::Record& r) {
    std::string s; s.reserve(256);
    const char US = '\x1f';
    s += r.mfCls;    s += US;
    s += r.mfSong;   s += US;
    s += r.th;       s += (r.thCurrent ? '1' : '0'); s += US;
    s += r.tgtKind;  s += US;
    s += r.tgtDisp;  s += US;
    s += r.stState;  s += US;
    s += r.stDisp;   s += US;
    s += r.pendKind; s += US;
    s += r.pendDisp; s += US;
    s += (r.ctmOk ? '1' : '0'); s += US;
    s += r.ctmArtist; s += US;
    s += r.ctmTitle;  s += US;
    s += r.streamMode; s += US;
    s += r.idVariant;  s += US;   // arm flip (anon<->minted) is a semantic input -> write immediately
    s += (r.spotPaid ? '1' : '0'); s += US;
    s += r.cartcutId; s += US;   // distinct ads -> per-ad writes; a stuck loop holds one id -> heartbeat only
    s += std::to_string(r.streak);
    return s;
}

} // namespace

namespace IHeartDeepLog {

bool toggle() {
    std::lock_guard<std::mutex> lk(g_mtx);
    bool now = !g_enabled.load();
    g_enabled.store(now);
    if (now) {
        // Start a fresh capture file on the next emit; reset dedup so the first
        // tick of the session writes immediately.
        g_cur_path.clear();
        g_last_sig.clear();
        g_last_write_tick = 0;
    }
    return now;
}

void setEnabled(bool on) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (on == g_enabled.load()) return;      // no transition
    g_enabled.store(on);
    if (on) {                                // 0->1: fresh capture file on next emit
        g_cur_path.clear();
        g_last_sig.clear();
        g_last_write_tick = 0;
    }
}

bool enabled() { return g_enabled.load(); }

std::string path() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_enabled.load() ? g_cur_path : std::string();
}

long extractMediaSeq(const std::string& body) {
    static const char* key = "#EXT-X-MEDIA-SEQUENCE:";
    size_t p = body.find(key);
    if (p == std::string::npos) return -1;
    p += std::strlen(key);
    long v = 0; bool any = false;
    while (p < body.size() && std::isdigit((unsigned char)body[p])) {
        v = v * 10 + (body[p] - '0'); ++p; any = true;
    }
    return any ? v : -1;
}

void emit(const Record& r) {
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_enabled.load()) return;       // re-check under lock (toggle race)

    const uint32_t tick = port::tickMs();
    std::string s = sigOf(r);
    const bool first     = (g_last_write_tick == 0);
    const bool changed   = first || (s != g_last_sig);
    const bool heartbeat = first || (tick - g_last_write_tick >= kHeartbeatMs);
    if (!changed && !heartbeat) return;

    std::string dir = logsDir();
    if (dir.empty()) return;

    if (g_cur_path.empty())                     { trimOld(dir); startNewFile(dir); }
    else if (fileSize(g_cur_path) >= kMaxBytes) { trimOld(dir); startNewFile(dir); }

    FILE* f = std::fopen(g_cur_path.c_str(), "a");
    if (!f) return;

    if (!g_file_started) {
        json m;
        m["_meta"]       = true;
        m["schema"]      = kSchema;
        m["app"]         = "RE-MOCT";
        m["sampleRate"]  = 44100;
        m["channels"]    = 2;
        m["heartbeatMs"] = (unsigned)kHeartbeatMs;
        m["maxBytes"]    = (unsigned long long)kMaxBytes;
        m["started"]     = nowLocal();
        m["startedUtc"]  = nowUtcIso();
        std::string ml = m.dump(); ml += '\n';
        std::fwrite(ml.data(), 1, ml.size(), f);
        g_file_started = true;
    }

    json j;
    j["ts"]          = nowLocal();
    j["utc"]         = nowUtcIso();
    j["tick"]        = (unsigned long long)tick;
    j["seq"]         = ++g_seq;
    j["writeReason"] = changed ? "change" : "heartbeat";

    j["stationId"]   = r.stationId;
    j["station"]     = r.station;
    j["audioSec"]    = r.audioSec;
    j["posSec"]      = r.posSec;

    j["mfCls"]       = r.mfCls;
    j["mfArtist"]    = r.mfArtist;
    j["mfTitle"]     = r.mfTitle;
    j["mfSong"]      = r.mfSong;
    j["mfSeq"]       = r.mfSeq;
    j["mfBodyLen"]   = (unsigned long long)r.mfBodyLen;
    j["pdt"]         = r.pdt;
    j["spotPaid"]    = r.spotPaid;
    j["spotInstanceId"] = r.spotInstanceId;
    j["cartcutId"]   = r.cartcutId;

    j["th"]          = r.th;
    j["thEnded"]     = r.thEnded;
    j["thCurrent"]   = r.thCurrent;

    j["tgtKind"]     = r.tgtKind;
    j["tgtDisp"]     = r.tgtDisp;

    j["stState"]     = r.stState;
    j["stDisp"]      = r.stDisp;
    j["pendKind"]    = r.pendKind;
    j["pendDisp"]    = r.pendDisp;
    j["streak"]      = r.streak;

    j["ctmOk"]          = r.ctmOk;
    j["ctmStatus"]      = r.ctmStatus;
    j["ctmArtist"]      = r.ctmArtist;
    j["ctmTitle"]       = r.ctmTitle;
    j["ctmAlbum"]       = r.ctmAlbum;
    j["ctmTrackId"]     = r.ctmTrackId;
    j["ctmStartSec"]    = r.ctmStartSec;
    j["ctmEndSec"]      = r.ctmEndSec;
    j["ctmDurationSec"] = r.ctmDurationSec;
    j["ctmEndedSecsAgo"]= r.ctmEndedSecsAgo;
    j["ctmImage"]       = r.ctmImage;
    j["ctmDataSource"]  = r.ctmDataSource;

    j["streamMode"]       = r.streamMode;
    j["digitalRequested"] = r.digitalRequested;
    j["digitalActive"]    = r.digitalActive;
    j["connectSeq"]       = r.connectSeq;

    j["idVariant"]        = r.idVariant;
    j["idProfileTail"]    = r.idProfileTail;
    j["idMintOk"]         = r.idMintOk;

    std::string line = j.dump();
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), f);
    std::fclose(f);

    g_last_sig        = s;
    g_last_write_tick = tick;
}

} // namespace IHeartDeepLog
