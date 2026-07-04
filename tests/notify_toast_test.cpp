// Pure unit test for the core::INotify seam (slice 7) — no PowerShell, no process
// spawn, no windows.h. Proves the seam is mockable (a FakeNotify stands in for the
// transport) and that the consumer-side content adapter in Toast.h builds exactly
// the (title, body) pair the baseline Toast.cpp built — including its oddities:
// the artist PREPENDS the title, and an empty album falls back to "RE-MOCT".
// The transport itself (escaping/-EncodedCommand/CreateProcess, moved verbatim to
// NotifyWinToast.cpp) is proven at the live gate — including the quote-injection
// case, the documented past bug. Returns nonzero on failure. Runs on Linux CI as
// well as MSYS2/UCRT64 (pure std).
#include "core/INotify.h"
#include "Toast.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// A fake notifier: captures every (title, body) pair it is asked to show.
struct FakeNotify : core::INotify {
    std::vector<std::pair<std::string, std::string>> shown;
    void notify(const std::string& title, const std::string& body) override {
        shown.emplace_back(title, body);
    }
};

int main() {
    // ---- (1) the seam is mockable: notify captures through the interface type ----
    {
        FakeNotify n;
        core::INotify& seam = n;               // exercise through the interface type
        seam.notify("t", "b");
        CHECK(n.shown.size() == 1);
        CHECK(n.shown[0].first == "t" && n.shown[0].second == "b");
    }

    // ---- (2) full track shape: artist prepends title, album is the body ----
    {
        FakeNotify n;
        showTrackToast("Jóga", "Björk", "Homogenic", n);
        CHECK(n.shown.size() == 1);
        CHECK(n.shown[0].first  == "Björk - Jóga");
        CHECK(n.shown[0].second == "Homogenic");
    }

    // ---- (3) empty artist: title alone ----
    {
        FakeNotify n;
        showTrackToast("Jóga", "", "Homogenic", n);
        CHECK(n.shown[0].first  == "Jóga");
        CHECK(n.shown[0].second == "Homogenic");
    }

    // ---- (4) empty album: body falls back to "RE-MOCT" ----
    {
        FakeNotify n;
        showTrackToast("Jóga", "Björk", "", n);
        CHECK(n.shown[0].first  == "Björk - Jóga");
        CHECK(n.shown[0].second == "RE-MOCT");
    }

    // ---- (5) the dominant status shape, (msg, "", "") — ~28 of the 33 call sites ----
    {
        FakeNotify n;
        showTrackToast("Theme: Awesome", "", "", n);
        CHECK(n.shown[0].first  == "Theme: Awesome");
        CHECK(n.shown[0].second == "RE-MOCT");
    }

    // ---- (6) the ("Streaming", station, "") shape: baseline INVERSION preserved —
    //          the detail arg prepends, "station - Streaming", not the reverse ----
    {
        FakeNotify n;
        showTrackToast("Streaming", "KWIN 97.7", "", n);
        CHECK(n.shown[0].first  == "KWIN 97.7 - Streaming");
        CHECK(n.shown[0].second == "RE-MOCT");
    }

    // ---- (7) content adapter does NOT escape — quotes pass through untouched
    //          (escaping is the transport's job; the fake must see raw metadata) ----
    {
        FakeNotify n;
        showTrackToast("Don't \"Stop\"", "L'Artist", "", n);
        CHECK(n.shown[0].first == "L'Artist - Don't \"Stop\"");
    }

    // ---- (8) successive toasts arrive in order, one notify per call ----
    {
        FakeNotify n;
        showTrackToast("first", "", "", n);
        showTrackToast("second", "", "", n);
        CHECK(n.shown.size() == 2);
        CHECK(n.shown[0].first == "first" && n.shown[1].first == "second");
    }

    if (g_fail) { std::printf("%d FAILURE(S)\n", g_fail); return 1; }
    std::printf("notify_toast_test: all checks passed\n");
    return 0;
}
