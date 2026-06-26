#ifdef _WIN32

#include "IHeartDeepLog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "json.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <cstdio>
#include <cctype>
#include <cstring>

using json = nlohmann::json;

namespace {

// ─── Tunables ────────────────────────────────────────────────────────────────
constexpr unsigned long long kMaxBytes   = 2ull * 1024 * 1024;  // size roll at ~2 MB
constexpr DWORD              kHeartbeatMs = 30000;               // forced record cadence
constexpr int               kKeepDays    = 5;                    // retention (days)
constexpr int               kSchema      = 1;
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
DWORD              g_last_write_tick = 0;
bool               g_file_started    = false;  // _meta written for g_cur_path?

// ─── Helpers ─────────────────────────────────────────────────────────────────
std::string logsDir() {                  // %APPDATA%\RE-MOCT\logs\  (created if absent)
    char buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    if (!(len > 0 && len < MAX_PATH)) return std::string();
    std::string base = std::string(buf) + "\\RE-MOCT";
    CreateDirectoryA(base.c_str(), nullptr);             // ok if it already exists
    std::string logs = base + "\\logs";
    CreateDirectoryA(logs.c_str(), nullptr);
    return logs + "\\";
}

std::string stampCompact() {             // "YYYYMMDD-HHMMSS" (local)
    SYSTEMTIME st; GetLocalTime(&st);
    char b[24];
    std::snprintf(b, sizeof(b), "%04d%02d%02d-%02d%02d%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::string nowLocal() {                 // "YYYY-MM-DD HH:MM:SS.mmm" (local)
    SYSTEMTIME st; GetLocalTime(&st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

std::string nowUtcIso() {                // "YYYY-MM-DDTHH:MM:SS.mmmZ" (UTC)
    SYSTEMTIME st; GetSystemTime(&st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

unsigned long long fileSize(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d))
        return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return 0;
}

// Delete capture files whose last-write time is older than kKeepDays. The active
// file is never deleted. Caller holds g_mtx.
void trimOld(const std::string& dir) {
    FILETIME ftNow; GetSystemTimeAsFileTime(&ftNow);
    const ULONGLONG now  = ((ULONGLONG)ftNow.dwHighDateTime << 32) | ftNow.dwLowDateTime;
    const ULONGLONG span = (ULONGLONG)kKeepDays * 24ull * 3600ull * 10000000ull;  // 100-ns ticks/day
    const ULONGLONG cutoff = (now > span) ? (now - span) : 0;

    std::string pattern = dir + kPrefix + "*" + kSuffix;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULONGLONG wt = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32)
                     | fd.ftLastWriteTime.dwLowDateTime;
        if (wt >= cutoff) continue;
        std::string full = dir + fd.cFileName;
        if (full == g_cur_path) continue;
        DeleteFileA(full.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
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

    const DWORD tick = GetTickCount();
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

    std::string line = j.dump();
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), f);
    std::fclose(f);

    g_last_sig        = s;
    g_last_write_tick = tick;
}

} // namespace IHeartDeepLog

#endif // _WIN32
