#include "Log.h"

// Portable since Phase 3 slice 1: the Win32 file-enumeration/time calls became
// std::filesystem + localtimeSafe with the behavior contract unchanged — dated
// files `remoct-YYYY-MM-DD.log` in port::logDir() (Windows: %TEMP%, the exact
// baseline; Linux: $XDG_STATE_HOME/re-moct), day-roll trim keeping the newest
// `keep_days` files, "YYYY-MM-DD HH:MM:SS.mmm" local-time stamps.
#include "PortUtil.h"
#include "StringUtils.h"   // localtimeSafe

#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

// ─── Tunables / state ────────────────────────────────────────────────────────
// Dated files are kPrefix + "YYYY-MM-DD" + kSuffix under port::logDir().
// (The old per-subsystem helpers wrote "remoct_stream.log"; the consolidated
// operational log is dated. Any stale remoct_stream.log from a prior build is
// left untouched — it is not a dated file — and can be deleted by hand.)
static const char* kPrefix = "remoct-";
static const char* kSuffix = ".log";

static std::atomic<bool> g_enabled{true};   // operational log: on by default
static std::atomic<int>  g_keep_days{5};    // dated files retained

// Single mutex serializing day-roll/trim + write across the streaming, scrobble
// and UI threads. Meyers singleton avoids static-init-order pitfalls.
static std::mutex& logMutex() { static std::mutex m; return m; }
// Last day we wrote/trimmed for ("YYYY-MM-DD"); only touched under logMutex().
static std::string g_last_day;

// ─── Helpers ─────────────────────────────────────────────────────────────────
static std::string todayStamp() {            // "YYYY-MM-DD" (local time)
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    if (!localtimeSafe(t, tmv)) return "1970-01-01";
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return b;
}

static std::string nowStamp() {              // "YYYY-MM-DD HH:MM:SS.mmm" (local)
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

static bool isDailyName(const std::string& n) {
    const size_t plen = std::string(kPrefix).size();
    const size_t slen = std::string(kSuffix).size();
    return n.size() > plen + slen
        && n.compare(0, plen, kPrefix) == 0
        && n.compare(n.size() - slen, slen, kSuffix) == 0;
}

// Keep the newest `keepDays` dated files, delete older. ISO dates sort lexically,
// so an ascending sort puts the oldest first. Non-dated files are ignored.
// Caller must already hold logMutex(). Non-throwing throughout (error_code
// overloads) — logging housekeeping must never take the app down.
static void trimOldDailyLocked(const std::string& dir, int keepDays) {
    if (keepDays < 1) keepDays = 1;
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string n = it->path().filename().string();
        if (isDailyName(n)) names.push_back(n);
    }

    if (static_cast<int>(names.size()) <= keepDays) return;
    std::sort(names.begin(), names.end());              // ascending: oldest first
    int toDelete = static_cast<int>(names.size()) - keepDays;
    for (int i = 0; i < toDelete; ++i)
        fs::remove(fs::path(dir + names[i]), ec);
}

// ─── Public API ──────────────────────────────────────────────────────────────
void Log::setEnabled(bool on) { g_enabled.store(on); }
bool Log::enabled()           { return g_enabled.load(); }

void Log::configure(int keep_days) {
    if (keep_days > 0) g_keep_days.store(keep_days);
}

std::string Log::path() {
    return port::logDir() + kPrefix + todayStamp() + kSuffix;
}

void Log::write(const char* component, const std::string& msg) {
    if (!g_enabled.load()) return;

    std::string line = nowStamp();
    line += " [";
    line += (component ? component : "");
    line += "] ";
    line += msg;
    line += '\n';

    std::lock_guard<std::mutex> lk(logMutex());
    std::string dir   = port::logDir();
    std::string today = todayStamp();

    // First write, or the clock has rolled into a new day: trim old dated files.
    if (today != g_last_day) {
        g_last_day = today;
        trimOldDailyLocked(dir, g_keep_days.load());
    }

    std::string p = dir + kPrefix + today + kSuffix;
    FILE* f = std::fopen(p.c_str(), "a");
    if (!f) return;
    std::fwrite(line.data(), 1, line.size(), f);
    std::fclose(f);
}

void Log::writef(const char* component, const char* fmt, ...) {
    if (!g_enabled.load()) return;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log::write(component, buf);
}
