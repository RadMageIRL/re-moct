// Windows-only request test for group (c): drives the REAL CoverArt paths through an
// injected FakeHttp and asserts the request each builds — the interface fields (a)/(b)
// never exercised: per-call UA, 10 MB reject_truncated, and the FollowSameScheme
// redirect policy for CAA. Links CoverArt (now portable) + the WinINet impl; it is
// if(WIN32)-guarded in tests/CMakeLists.txt so Linux CI keeps only the pure suites.
//
// (CDRipper's AR/CTDB requests aren't driven here — that TU pulls the full rip
// dependency set. Their plain-HTTP no-SECURE decision is proven purely by
// urlIsSecureScheme in http_seam_test, and AR end-to-end by the Joan Osborne rip gate.)
#include "core/IHttp.h"
#include "CoverArt.h"
#include "Version.h"
#include "json.hpp"
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// Fake transport: captures the request, returns a preset response.
struct FakeHttp : core::IHttp {
    core::HttpRequest  last;
    core::HttpResponse next;
    int calls = 0;
    core::HttpResponse fetch(const core::HttpRequest& req) override {
        last = req; ++calls; return next;
    }
};
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    FakeHttp fake;
    core::setHttp(&fake);

    // ── CoverArt::bytesByMbid (CAA): default UA, 10 MB reject, FollowSameScheme ──
    fake.next = core::HttpResponse{}; fake.next.ok = true; fake.next.status = 200;
    fake.next.body = std::string({ (char)0x89, 'P', 'N', 'G', (char)0x0D, (char)0x0A,
                                   (char)0x1A, (char)0x0A, 'd', 'a', 't', 'a' });   // >=12, PNG magic
    std::vector<uint8_t> img = CoverArt::bytesByMbid("mbid-test-123");
    CHECK(!img.empty());                                       // looksLikeImage(PNG) -> returned
    CHECK(fake.last.method == "GET");
    CHECK(has(fake.last.url, "coverartarchive.org/release/mbid-test-123/front-500"));
    CHECK(fake.last.user_agent.empty());                      // seam default (full) UA
    CHECK(fake.last.max_body == 10u * 1024 * 1024);
    CHECK(fake.last.reject_truncated == true);
    CHECK(fake.last.redirect == core::RedirectPolicy::FollowSameScheme);

    // ── CoverArt generic httpGet (via urlBySong): short UA, FollowAll, 10 MB reject ──
    fake.next = core::HttpResponse{}; fake.next.ok = true; fake.next.status = 200;
    fake.next.body =
        R"({"results":[{"artistName":"Portishead","trackName":"Roads",)"
        R"("artworkUrl100":"https://is1.mzstatic.com/image/x/100x100bb.jpg"}]})";
    std::string url = CoverArt::urlBySong("Portishead", "Roads");
    CHECK(has(url, "600x600"));                                // 100x100 bumped -> a real hit
    CHECK(fake.last.method == "GET");
    CHECK(has(fake.last.url, "itunes.apple.com/search"));
    CHECK(fake.last.user_agent == "RE-MOCT/" REMOCT_VERSION); // short UA (non-default)
    CHECK(fake.last.max_body == 10u * 1024 * 1024);
    CHECK(fake.last.reject_truncated == true);
    CHECK(fake.last.redirect == core::RedirectPolicy::FollowAll);

    core::setHttp(nullptr);   // restore the default transport

    if (!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
