// iheart_request_test — Windows-only end-to-end request test for HTTP site 8
// (slice 4): links the REAL IHeartRadio consumer + the WinINet impl, injects a
// FakeHttp via core::setHttp, and asserts the requests the consumer builds
// through its persistent session:
//   - session config folded in by the default openSession forwarder:
//     UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RE-MOCT/1.0" + 5000 ms timeout
//   - per-request: Accept: application/json, Pragma no-cache, 4 MB cap-and-keep,
//     GET, FollowAll, and NO cancel token (parity: boundedness = the 5 s timeout)
//   - the three verified endpoints hit in order: liveStations (self-resolve),
//     trackHistory (pollNowPlaying), currentTrackMeta (pollCurrentTrackMeta)
// and that the parse paths still produce the right now-playing from canned bodies.
#include "core/IHttp.h"
#include "IHeartRadio.h"
#include "json.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// Test station: an id no real station uses, so a stale sidecar can't alias it.
static const char* kZc  = "zc999999991";
static const long long kId = 999999991;

// The sidecar is a real cache file in %TEMP% shared with the app. Scrub ONLY our
// test key so resolve() takes the self-resolve path deterministically — never
// delete the file (it can hold user-named stations).
static void scrubSidecarKey() {
    const std::string path = IHeartRadio::sidecarPath();
    nlohmann::json root;
    { std::ifstream in(path); if (!in) return;
      try { in >> root; } catch (...) { return; } }
    if (!root.is_object() || !root.contains(kZc)) return;
    root.erase(kZc);
    std::ofstream out(path, std::ios::trunc);
    if (out) out << root.dump(2) << "\n";
}

struct FakeHttp : core::IHttp {
    std::vector<core::HttpRequest> reqs;
    core::HttpResponse fetch(const core::HttpRequest& req) override {
        reqs.push_back(req);
        core::HttpResponse res; res.ok = true; res.status = 200;
        long long now = (long long)std::time(nullptr);
        if (req.url.find("liveStations") != std::string::npos) {
            res.body = R"({"hits":[{"id":999999991,"name":"Test FM"}]})";
        } else if (req.url.find("trackHistory") != std::string::npos) {
            // started 60s ago, 240s long -> currently playing
            res.body = "{\"data\":[{\"artist\":\"Bjork\",\"title\":\"Joga\","
                       "\"startTime\":" + std::to_string(now - 60) +
                       ",\"trackDuration\":240}]}";
        } else if (req.url.find("currentTrackMeta") != std::string::npos) {
            // epochs in MILLISECONDS here (the endpoint's quirk the consumer normalises)
            res.body = "{\"artist\":\"Bjork\",\"title\":\"Joga\",\"album\":\"Homogenic\","
                       "\"trackId\":7,\"startTime\":" + std::to_string((now - 60) * 1000) +
                       ",\"endTime\":" + std::to_string((now + 180) * 1000) +
                       ",\"trackDuration\":240,\"imagePath\":\"http://img/x.jpg\","
                       "\"dataSource\":\"test\"}";
        } else {
            res.ok = false; res.status = 404;
        }
        return res;
    }
};

// Every IHeart request must carry the same session + per-request shape.
static void checkRequestShape(const core::HttpRequest& r) {
    CHECK(r.method.empty() || r.method == "GET");
    CHECK(r.user_agent == "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RE-MOCT/1.0");
    CHECK(r.timeout_ms == 5000);                    // folded from the session config
    CHECK(r.pragma_no_cache == true);               // baseline INTERNET_FLAG_PRAGMA_NOCACHE
    CHECK(r.max_body == 4u * 1024u * 1024u);        // baseline 4 MB cap-and-keep
    CHECK(r.reject_truncated == false);             // cap-and-KEEP
    CHECK(r.redirect == core::RedirectPolicy::FollowAll);
    CHECK(r.cancel == nullptr);                     // parity: bounded by timeout, not stop_
    bool accept_json = false;
    for (const auto& kv : r.headers)
        if (kv.first == "Accept" && kv.second == "application/json") accept_json = true;
    CHECK(accept_json);
}

int main() {
    scrubSidecarKey();

    FakeHttp fake;
    core::setHttp(&fake);

    const std::string url = std::string("https://n1a.revma.ihrhls.com/") + kZc + "/hls.m3u8";
    CHECK(IHeartRadio::isIHeartUrl(url));

    IHeartRadio ih(core::http());   // slice c: HTTP injected; setHttp(&fake) above backs core::http()
    ih.setLogger([](const std::string& s){ std::printf("  [log] %s\n", s.c_str()); });

    // ── resolve: sidecar miss -> self-resolve via liveStations ──
    CHECK(ih.resolve(url));
    CHECK(ih.resolved());
    CHECK(ih.stationId() == kId);
    CHECK(ih.stationName() == "Test FM");
    CHECK(fake.reqs.size() == 1);
    CHECK(fake.reqs[0].url ==
          "https://api.iheart.com/api/v2/content/liveStations/999999991");

    // ── pollNowPlaying -> trackHistory ──
    long ended = -1;
    std::string np = ih.pollNowPlaying(&ended);
    CHECK(np == "Bjork - Joga");
    CHECK(ended <= 0);                              // currently playing
    CHECK(fake.reqs.size() == 2);
    CHECK(fake.reqs[1].url ==
          "https://api.iheart.com/api/v3/live-meta/stream/999999991/trackHistory");

    // ── pollCurrentTrackMeta -> currentTrackMeta (ms epochs normalised) ──
    IHeartRadio::CurrentTrack ct;
    CHECK(ih.pollCurrentTrackMeta(&ct));
    CHECK(ct.ok && ct.httpStatus == 200);
    CHECK(ct.artist == "Bjork" && ct.title == "Joga" && ct.album == "Homogenic");
    CHECK(ct.endedSecsAgo <= 0);                    // playing (endTime in the future)
    CHECK(ct.imagePath == "http://img/x.jpg");
    CHECK(fake.reqs.size() == 3);
    CHECK(fake.reqs[2].url ==
          "https://us.api.iheart.com/api/v3/live-meta/stream/999999991/currentTrackMeta?defaultMetadata=true");

    // ── the request shape holds for every call through the session ──
    for (const auto& r : fake.reqs) checkRequestShape(r);

    core::setHttp(nullptr);
    scrubSidecarKey();   // leave no test residue in the shared cache

    if (!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
