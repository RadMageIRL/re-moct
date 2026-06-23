#ifdef _WIN32

#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <string>

// ─── Tunables / state ────────────────────────────────────────────────────────
// Dated files are kPrefix + "YYYY-MM-DD" + kSuffix under %TEMP%.
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
static std::string tempDir() {
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);   // includes trailing backslash
    return (n > 0 && n < MAX_PATH) ? std::string(tmp) : std::string();
}

static std::string todayStamp() {            // "YYYY-MM-DD" (local time)
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

static std::string nowStamp() {              // "YYYY-MM-DD HH:MM:SS.mmm" (local)
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
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
// Caller must already hold logMutex().
static void trimOldDailyLocked(const std::string& dir, int keepDays) {
    if (keepDays < 1) keepDays = 1;
    std::string pattern = dir + kPrefix + "*" + kSuffix;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::string> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string n = fd.cFileName;
            if (isDailyName(n)) names.push_back(n);     // guard against 8.3 quirks
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (static_cast<int>(names.size()) <= keepDays) return;
    std::sort(names.begin(), names.end());              // ascending: oldest first
    int toDelete = static_cast<int>(names.size()) - keepDays;
    for (int i = 0; i < toDelete; ++i)
        DeleteFileA((dir + names[i]).c_str());
}

// ─── Public API ──────────────────────────────────────────────────────────────
void Log::setEnabled(bool on) { g_enabled.store(on); }
bool Log::enabled()           { return g_enabled.load(); }

void Log::configure(int keep_days) {
    if (keep_days > 0) g_keep_days.store(keep_days);
}

std::string Log::path() {
    return tempDir() + kPrefix + todayStamp() + kSuffix;
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
    std::string dir   = tempDir();
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

#endif // _WIN32
