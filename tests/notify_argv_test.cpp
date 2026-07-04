// notify_argv_test.cpp — Linux-only unit test for the notify-send argv builder
// (Phase 3 slice 5). Proves the injection-safety surface HEADLESS (no daemon, no
// spawn): buildNotifyArgv() puts title/body into argv slots VERBATIM (no shell,
// no escaping needed) and always terminates option parsing with "--" so a
// dash-leading title can't be read as a notify-send flag. This is the argv-level
// analog of the Windows -EncodedCommand quote-injection case (notify_toast_test
// #7), tested here because on Linux we CAN assert it directly (no UTF-16LE/base64
// to decode). Content mapping is covered by notify_toast_test; the live toast
// render is the desktop gate. Returns nonzero on failure.
#include "NotifyArgv.h"
#include <cstdio>
#include <string>
#include <vector>

using platform::lnx::buildNotifyArgv;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

int main() {
    // ---- (1) the fixed prefix: program, app-name attribution, option terminator
    {
        auto a = buildNotifyArgv("Title", "Body");
        CHECK(a.size() == 6);
        CHECK(a[0] == "notify-send");
        CHECK(a[1] == "-a");
        CHECK(a[2] == "RE-MOCT");
        CHECK(a[3] == "--");        // option terminator BEFORE positionals
        CHECK(a[4] == "Title");     // summary
        CHECK(a[5] == "Body");      // body
    }

    // ---- (2) metadata is verbatim — quotes/apostrophes are DATA, not escaped
    //          (execvp hands them to notify-send as argv, never to a shell) ----
    {
        auto a = buildNotifyArgv("L'Artist - Don't \"Stop\"", "Album; rm -rf /");
        CHECK(a[4] == "L'Artist - Don't \"Stop\"");
        CHECK(a[5] == "Album; rm -rf /");   // shell metachars inert — no shell
    }

    // ---- (3) accented metadata (Björk-class) passes through byte-for-byte ----
    {
        auto a = buildNotifyArgv("Björk - Jóga", "Homogenic");
        CHECK(a[4] == "Björk - Jóga");
        CHECK(a[5] == "Homogenic");
    }

    // ---- (4) the option-injection guard: a dash-leading title stays positional.
    //          "--" precedes it, so notify-send reads "--version"/"-h" as summary,
    //          not as a flag. The argv-level twin of quote-injection. ----
    {
        auto a = buildNotifyArgv("--version", "-h");
        CHECK(a[3] == "--");
        CHECK(a[4] == "--version");   // summary, not a flag — the "--" guards it
        CHECK(a[5] == "-h");
    }

    // ---- (5) empty body (the "RE-MOCT" fallback happens upstream in Toast.h)
    //          still lands as a distinct positional slot ----
    {
        auto a = buildNotifyArgv("Just a title", "");
        CHECK(a.size() == 6);
        CHECK(a[5] == "");
    }

    if (g_fail) { std::printf("%d FAILURE(S)\n", g_fail); return 1; }
    std::printf("notify_argv_test: all checks passed\n");
    return 0;
}
