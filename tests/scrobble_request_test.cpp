// Windows-only end-to-end request test for the scrobble POST sites (group b).
//
// Injects a FakeHttp via core::setHttp() and drives the REAL public LastFm /
// ListenBrainz API — so the actual signing (api_sig) and JSON body-building run — then
// asserts the request that reaches the seam: verb, content-type, Authorization header,
// URL, timeout, and body shape. Includes a non-ASCII (UTF-8) artist/track so the fake
// confirms the body is carried as raw bytes up to the seam. (True wire fidelity is the
// live round-trip's job — a fake can't prove UTF-8 survives the socket.)
//
// Not Linux-CI-pure (it links the Windows consumers), so it is if(WIN32)-guarded in
// tests/CMakeLists.txt: Linux CI runs the 3 pure suites, Windows runs 4.
#include "IHttp.h"
#include "LastFm.h"
#include "ListenBrainz.h"
#include "json.hpp"
#include <cstdio>
#include <string>

using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// Fake transport: captures the request and returns a preset response.
struct FakeHttp : core::IHttp {
    core::HttpRequest  last;
    core::HttpResponse next;
    int calls = 0;
    core::HttpResponse fetch(const core::HttpRequest& req) override {
        last = req; ++calls; return next;
    }
};

static std::string header(const core::HttpRequest& r, const std::string& key) {
    for (const auto& kv : r.headers) if (kv.first == key) return kv.second;
    return {};
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // Non-ASCII artist/track — exercises the raw-UTF-8 body path.
    const std::string kArtist = "Björk";
    const std::string kTrack  = "Jóga";
    const std::string kAlbum  = "Homogenic";

    FakeHttp fake;
    core::setHttp(&fake);

    // ── LastFm scrobble: POST, form-encoded, api_sig present, no auth header ──
    fake.next = core::HttpResponse{}; fake.next.ok = true; fake.next.status = 200;
    fake.next.body = "{}";   // no "error" field -> postWriteCall treats as success
    LastFm::scrobble("apikey", "secret", "sessionkey",
                     kArtist, kTrack, 1700000000, true, kAlbum);
    CHECK(fake.calls >= 1);
    CHECK(fake.last.method == "POST");
    CHECK(fake.last.url == "https://ws.audioscrobbler.com/2.0/");
    CHECK(fake.last.content_type == "application/x-www-form-urlencoded");
    CHECK(fake.last.timeout_ms == 8000);
    CHECK(has(fake.last.body, "method=track.scrobble"));
    CHECK(has(fake.last.body, "api_sig="));                 // signing ran, consumer-side
    CHECK(header(fake.last, "Authorization").empty());      // LastFm uses no auth header

    // ── LastFm now-playing: POST, correct method ──
    LastFm::updateNowPlaying("apikey", "secret", "sessionkey", kArtist, kTrack, kAlbum);
    CHECK(fake.last.method == "POST");
    CHECK(has(fake.last.body, "method=track.updateNowPlaying"));

    // ── ListenBrainz single: POST, JSON, Authorization: Token, raw UTF-8 body ──
    fake.next = core::HttpResponse{}; fake.next.ok = true; fake.next.status = 200;
    ListenBrainz::submitSingle("mytoken", kArtist, kTrack, 1700000000, kAlbum);
    CHECK(fake.last.method == "POST");
    CHECK(fake.last.url == "https://api.listenbrainz.org/1/submit-listens");
    CHECK(fake.last.content_type == "application/json");
    CHECK(header(fake.last, "Authorization") == "Token mytoken");
    CHECK(fake.last.timeout_ms == 8000);
    CHECK(has(fake.last.body, kArtist));   // raw UTF-8 bytes reached the seam un-widened
    {
        auto j = json::parse(fake.last.body);
        CHECK(j.value("listen_type", std::string()) == "single");
        const auto& tm = j["payload"][0]["track_metadata"];
        CHECK(tm.value("artist_name", std::string()) == kArtist);
        CHECK(tm.value("track_name",  std::string()) == kTrack);
    }

    // ── ListenBrainz playing_now: POST, correct listen_type ──
    ListenBrainz::playingNow("mytoken", kArtist, kTrack, kAlbum);
    {
        auto j = json::parse(fake.last.body);
        CHECK(j.value("listen_type", std::string()) == "playing_now");
    }

    // ── ListenBrainz validate-token: GET carries the auth header; status-gated ──
    fake.next = core::HttpResponse{}; fake.next.ok = true; fake.next.status = 200;
    fake.next.body = R"({"valid":true,"user_name":"tester"})";
    std::string user;
    bool valid = ListenBrainz::validateToken("mytoken", user);
    CHECK(fake.last.method == "GET");
    CHECK(fake.last.url == "https://api.listenbrainz.org/1/validate-token");
    CHECK(header(fake.last, "Authorization") == "Token mytoken");
    CHECK(valid);
    CHECK(user == "tester");

    core::setHttp(nullptr);   // restore the default transport

    if (!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
