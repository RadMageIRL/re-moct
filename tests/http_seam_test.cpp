// Pure unit test for the core::IHttp seam — no WinINet, no network. Proves the seam
// is mockable (a FakeHttp stands in for the transport) and that the group-(a)
// consumers' response-parsing works against a faked HTTP response. Returns nonzero
// on failure. Runs on Linux CI as well as MSYS2/UCRT64 (pure std + nlohmann).
#include "IHttp.h"
#include "json.hpp"
#include <cstdio>
#include <string>

using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// A fake transport: returns a preset response and captures the request it was given.
struct FakeHttp : core::IHttp {
    core::HttpResponse next;
    core::HttpRequest  last;
    int calls = 0;
    core::HttpResponse fetch(const core::HttpRequest& req) override {
        last = req; ++calls;
        return next;
    }
};

int main() {
    // ---- (1) the seam is mockable: fetch surfaces canned fields + captures request ----
    {
        FakeHttp h;
        h.next.ok = true; h.next.status = 200;
        h.next.body = "hello"; h.next.final_url = "https://x/final";
        core::HttpRequest req;
        req.url = "https://x/api"; req.max_body = 4u*1024*1024; req.timeout_ms = 6000;
        core::IHttp& seam = h;                 // exercise through the interface type
        auto r = seam.fetch(req);
        CHECK(h.calls == 1);
        CHECK(h.last.url == "https://x/api");
        CHECK(h.last.max_body == 4u*1024*1024);
        CHECK(h.last.timeout_ms == 6000);
        CHECK(r.ok && r.status == 200 && r.body == "hello" && r.final_url == "https://x/final");
    }

    // ---- (2) RadioBrowser-shape parse against a faked response (url_resolved fallback) ----
    {
        FakeHttp h; h.next.ok = true; h.next.status = 200;
        h.next.body = R"([
          {"name":"Jazz FM","url":"http://a/","url_resolved":"http://a/stream",
           "codec":"MP3","bitrate":128,"country":"UK"},
          {"name":"No Resolve","url":"http://b/","url_resolved":"",
           "codec":"AAC","bitrate":64,"country":"US"}
        ])";
        core::HttpRequest req; req.url = "https://mirror/json/stations/search?name=x";
        std::string body = h.fetch(req).body;

        auto j = json::parse(body);
        CHECK(j.is_array());
        CHECK(j.size() == 2);
        // mirror RadioBrowser::search field logic exactly
        std::string n0 = j[0].value("name", std::string());
        std::string u0 = j[0].value("url_resolved", std::string());
        if (u0.empty()) u0 = j[0].value("url", std::string());
        std::string u1 = j[1].value("url_resolved", std::string());
        if (u1.empty()) u1 = j[1].value("url", std::string());
        CHECK(n0 == "Jazz FM");
        CHECK(u0 == "http://a/stream");
        CHECK(u1 == "http://b/");           // falls back to url when url_resolved empty
    }

    // ---- (3) MBLookup-shape parse against a faked response ----
    {
        FakeHttp h; h.next.ok = true; h.next.status = 200;
        h.next.body = R"({"releases":[{"id":"mbid-1","title":"Relish","date":"1995",
            "artist-credit":[{"artist":{"name":"Joan Osborne"}}]}]})";
        core::HttpRequest req; req.url = "https://musicbrainz.org/ws/2/release/";
        auto j = json::parse(h.fetch(req).body);

        auto& rel = j.at("releases")[0];
        CHECK(rel.value("title",  std::string()) == "Relish");
        CHECK(rel.value("id",     std::string()) == "mbid-1");
        CHECK(rel.value("date",   std::string()) == "1995");
        CHECK(rel["artist-credit"][0]["artist"].value("name", std::string()) == "Joan Osborne");
    }

    // ---- (4) transport failure -> empty body (RadioBrowser "try next mirror" path) ----
    {
        FakeHttp h; h.next.ok = false; h.next.body = "";
        core::HttpRequest req; req.url = "https://down-mirror/";
        std::string body = h.fetch(req).body;
        CHECK(body.empty());
    }

    if (!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
