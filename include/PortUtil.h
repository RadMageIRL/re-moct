// PortUtil.h — tiny cross-platform primitives (Phase 3 slice 1).
//
// The rule that makes this header safe to adopt: on Windows, every helper
// expands to the BASELINE CALL, verbatim — port::sleepMs is ::Sleep,
// port::tickMs is ::GetTickCount, port::fopenUtf8 is _wfopen(utf8_to_wide(p)) —
// so adopting a helper at a call site is a rename, not a behavior change
// (the slice-1 gate holds Windows at zero behavior change). Linux gets the
// direct twin with the same shape, including the baseline's own quirks
// (uint32 tick wrap arithmetic, ~15 ms sleep granularity noted in lessons.md).
//
// Include from .cpp files ONLY — on Windows this pulls in <windows.h>, and
// keeping platform headers out of remoct's own headers is the slice-8 detox
// property (audit: grep include/*.h for PortUtil).
#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include "StringUtils.h"     // utf8_to_wide (Windows-only helper)
#else
#  include <ctime>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace port {

// Sleep for ~ms. Windows: ::Sleep — the exact baseline call in the CDSource
// reader / StreamSource producer / CDRipper pacing loops (granularity ~15 ms,
// a load-bearing property those loops were tuned against). Linux: nanosleep.
inline void sleepMs(unsigned ms) {
#ifdef _WIN32
    ::Sleep(ms);
#else
    struct timespec ts { static_cast<time_t>(ms / 1000u),
                         static_cast<long>(ms % 1000u) * 1000000L };
    nanosleep(&ts, nullptr);
#endif
}

// Monotonic millisecond tick in the baseline's exact shape: a uint32 that
// wraps (GetTickCount wraps at 49.7 days; all baseline consumers do wrap-safe
// unsigned subtraction). Linux: CLOCK_MONOTONIC folded into the same uint32.
inline uint32_t tickMs() {
#ifdef _WIN32
    return ::GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000u
                                 + static_cast<uint64_t>(ts.tv_nsec) / 1000000u);
#endif
}

// fopen with a UTF-8 path. Windows: _wfopen(utf8_to_wide(path), wide(mode)) —
// the exact baseline shape at every CDRipper/Mp4Chapters call site (narrow
// fopen would break non-ASCII paths there). Linux: fopen, byte paths are
// UTF-8 natively.
inline FILE* fopenUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    wchar_t wmode[8] = {};
    int i = 0;
    for (; mode[i] && i < 7; ++i) wmode[i] = static_cast<wchar_t>(mode[i]);
    return _wfopen(utf8_to_wide(path).c_str(), wmode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// Scratch/temp directory WITH trailing separator. Windows: GetTempPathA — the
// baseline in Log/IHeartRadio (includes the trailing backslash). Linux:
// $TMPDIR or /tmp.
inline std::string tempDir() {
#ifdef _WIN32
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);   // includes trailing backslash
    return (n > 0 && n < MAX_PATH) ? std::string(tmp) : std::string();
#else
    const char* t = std::getenv("TMPDIR");
    std::string d = (t && *t) ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d;
#endif
}

// Where dated operational logs live, WITH trailing separator. Windows: %TEMP%
// (the Log.cpp / IHeartDeepLog.cpp baseline — dated files under %TEMP%,
// day-rolled and trimmed). Linux: XDG state — $XDG_STATE_HOME/re-moct or
// ~/.local/state/re-moct (created on first use); logs are state, not temp,
// under the XDG conventions decided for the port.
inline std::string logDir() {
#ifdef _WIN32
    return tempDir();
#else
    std::string base;
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h)
        base = std::string(h) + "/.local/state";
    else return tempDir();                    // last resort: scratch
    std::string dir = base + "/re-moct";
    ::mkdir(base.c_str(), 0755);              // best-effort parents
    ::mkdir(dir.c_str(), 0755);
    return dir + "/";
#endif
}

// Directory of the running executable, NO trailing separator (Phase 4 slice c:
// resolve the streaming plugin at <exeDir>/plugins/remoct_stream.{dll,so}).
// Windows: GetModuleFileNameW -> strip the file name -> UTF-8. Linux: readlink
// /proc/self/exe -> dirname. Returns "" if the OS query fails (caller falls back).
inline std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    while (n > 0 && buf[n - 1] != L'\\' && buf[n - 1] != L'/') --n;  // strip file name
    if (n > 0) --n;                                                  // drop the separator
    int len = ::WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    if (len) ::WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, out.data(), len, nullptr, nullptr);
    return out;
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string p(buf);
    size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
#endif
}

} // namespace port
