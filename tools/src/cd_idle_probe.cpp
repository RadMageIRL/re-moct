// cd_idle_probe.cpp - standalone CD idle-latency probe (Slice 1, Probe B2).
//
// Measures how long core::ICdDevice::mediaPresent() takes on a drive that has
// been left IDLE long enough to spin down - the exact production call Slice 3
// would run every 2s while the app sits stopped (Windows IOCTL_CDROM_CHECK_VERIFY
// / Linux SG_IO TEST UNIT READY). It opens the drive through the real platform
// factory (core::cdio()) so the syscall path is identical to the app's; it does
// NOT link the app - just the one platform CD-io TU.
//
// Build (from repo root):
//   Windows (MSYS2 UCRT64):
//     g++ -std=c++20 -Iinclude tools/src/cd_idle_probe.cpp \
//         src/platform/win/CdIoWin.cpp -o cd_idle_probe.exe
//   Linux:
//     g++ -std=c++20 -Iinclude tools/src/cd_idle_probe.cpp \
//         src/platform/linux/CdIoSgIo.cpp -o cd_idle_probe
//
// Run:  cd_idle_probe[.exe] [drive] [idle_sec] [iters] [interval_sec]
//   drive        Windows "D"; Linux "sr0" or "/dev/sr0"   (default: D / sr0)
//   idle_sec     seconds to sleep before the first poll    (default: 90)
//   iters        number of polls                           (default: 30)
//   interval_sec seconds between polls                     (default: 2)
//
// IMPORTANT: listen to the drive during the loop. A spin-up is audible and may
// not show in the timing if the call returns before the motor reaches speed.
#include "core/ICdIo.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using clock_t_ = std::chrono::steady_clock;

static long long usSince(clock_t_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               clock_t_::now() - t0).count();
}

int main(int argc, char** argv) {
#ifdef _WIN32
    const char* def_drive = "D";
#else
    const char* def_drive = "sr0";
#endif
    const std::string drive = (argc > 1) ? argv[1] : def_drive;
    const int idle_sec  = (argc > 2) ? std::atoi(argv[2]) : 90;
    const int iters     = (argc > 3) ? std::atoi(argv[3]) : 30;
    const int interval  = (argc > 4) ? std::atoi(argv[4]) : 2;

    std::printf("cd_idle_probe: drive=%s idle=%ds iters=%d interval=%ds\n",
                drive.c_str(), idle_sec, iters, interval);

    auto dev = core::cdio().open(drive);
    if (!dev) {
        std::fprintf(stderr, "ERROR: could not open drive '%s'\n", drive.c_str());
        return 1;
    }
    std::printf("opened. idling %ds so the drive spins down (DO NOT touch it)...\n",
                idle_sec);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(idle_sec));

    std::printf("polling mediaPresent() x%d, one every %ds. Listen for spin-up.\n",
                iters, interval);
    std::printf("%-5s  %-8s  %s\n", "iter", "present", "dt_us");

    std::vector<long long> dts;
    dts.reserve((size_t)iters);
    long long first_dt = -1;
    for (int i = 1; i <= iters; ++i) {
        const auto t0 = clock_t_::now();
        const bool ok = dev->mediaPresent();
        const long long dt = usSince(t0);
        if (i == 1) first_dt = dt;
        dts.push_back(dt);
        std::printf("%-5d  %-8s  %lld\n", i, ok ? "yes" : "no", dt);
        std::fflush(stdout);
        if (i < iters) std::this_thread::sleep_for(std::chrono::seconds(interval));
    }

    std::vector<long long> sorted = dts;
    std::sort(sorted.begin(), sorted.end());
    const long long med = sorted.empty() ? 0
        : sorted[sorted.size() / 2];
    const long long mx  = sorted.empty() ? 0 : sorted.back();
    std::printf("\nsummary: median=%lld us  max=%lld us  call#1=%lld us\n",
                med, mx, first_dt);
    std::printf("verdict guide: PASS if median<10000 && max<250000 && call#1 not an "
                "outlier && no audible spin-up.\n");
    return 0;
}
