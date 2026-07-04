// lb_probe — headless verification of the ListenBrainz client, before any UI.
//
// Prompts for the user token at runtime (typing is hidden) so nothing lands on
// disk or in the binary. If LISTENBRAINZ_TOKEN is already set in the environment
// it is used without prompting, which is handy for scripted runs. Validates the
// token, sends one playing_now and one completed single listen, and prints the
// outcome plus the URL to eyeball the result in your listen history.
//
// Build (from the project root, in the MSYS2 ucrt64 shell):
//   g++ -std=c++20 -Iinclude -Ilib -Isrc \
//       src/lb_probe.cpp src/ListenBrainz.cpp src/Log.cpp \
//       -lwininet -o /e/code/digi/lb_probe.exe
//
// Run:  .\lb_probe.exe   (it will ask for the token)

#ifdef _WIN32

#include "ListenBrainz.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <iostream>

// Prompt for the token with console echo disabled; fall back to an env var.
static std::string getToken() {
    if (const char* env = std::getenv("LISTENBRAINZ_TOKEN"); env && *env)
        return env;

    std::printf("ListenBrainz user token: ");
    std::fflush(stdout);

    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  mode = 0;
    bool   restore = false;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);   // hide typing
        restore = true;
    }

    std::string token;
    std::getline(std::cin, token);

    if (restore) { SetConsoleMode(h, mode); std::printf("\n"); }

    while (!token.empty() &&
           (token.back() == '\r' || token.back() == '\n' ||
            token.back() == ' '  || token.back() == '\t'))
        token.pop_back();
    return token;
}

int main() {
    std::string token = getToken();
    if (token.empty()) {
        std::fprintf(stderr, "No token entered.\n");
        return 2;
    }

    std::string user;
    std::printf("validate-token ... ");
    if (ListenBrainz::validateToken(token, user)) {
        std::printf("OK  (user: %s)\n", user.c_str());
    } else {
        std::printf("FAILED  (bad token, or no network)\n");
        return 1;
    }

    std::printf("playing_now    ... ");
    bool pn = ListenBrainz::playingNow(token, "RE-MOCT", "Probe \xE2\x80\x94 Now Playing");
    std::printf("%s\n", pn ? "accepted" : "rejected");

    long now = (long)std::time(nullptr);
    std::printf("single listen  ... ");
    bool s = ListenBrainz::submitSingle(token, "RE-MOCT", "Probe \xE2\x80\x94 Single Listen",
                                        now, "RE-MOCT Tests");
    std::printf("%s  (listened_at=%ld)\n", s ? "accepted" : "rejected", now);

    std::printf("\nVerify at: https://listenbrainz.org/user/%s/\n", user.c_str());
    std::printf("(Full HTTP detail is in the [listenbrainz] lines of today's remoct log.)\n");
    return s ? 0 : 1;
}

#else
int main() { std::printf("Windows-only.\n"); return 0; }
#endif
