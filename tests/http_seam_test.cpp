// Pure unit test for the core::IHttp seam — no WinINet, no network. Proves the seam
// is mockable (a FakeHttp stands in for the transport) and that the group-(a)
// consumers' response-parsing works against a faked HTTP response. Returns nonzero
// on failure. Runs on Linux CI as well as MSYS2/UCRT64 (pure std + nlohmann).
#include "core/IHttp.h"
#include "json.hpp"
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>

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

    // ═══ group (c): the fields (a)/(b) only declared ════════════════════════

    // ---- (5) byte-body passthrough: std::string carries arbitrary bytes incl NUL ----
    {
        FakeHttp h; h.next.ok = true; h.next.status = 200;
        const std::string png = { (char)0x89, 'P', 'N', 'G', (char)0x0D, (char)0x0A,
                                  (char)0x00, (char)0xFF, 'x' };   // has an embedded NUL + 0xFF
        h.next.body = png;
        core::HttpRequest req; req.url = "https://coverartarchive.org/release/x/front-500";
        auto r = h.fetch(req);
        CHECK(r.body.size() == png.size());
        CHECK(r.body == png);                                  // byte-identical, NUL-safe
        std::vector<uint8_t> bytes(r.body.begin(), r.body.end());   // the byte-consumer copy
        CHECK(bytes.size() == png.size());
        CHECK(bytes[0] == 0x89 && bytes[6] == 0x00 && bytes[7] == 0xFF);
    }

    // ---- (6) final_url surfaced (hlsHttpGet token propagation depends on this) ----
    {
        FakeHttp h; h.next.ok = true; h.next.status = 200;
        h.next.final_url = "https://cdn/redirected?rj-tok=abc";
        core::HttpRequest req; req.url = "https://stream/manifest";
        CHECK(h.fetch(req).final_url == "https://cdn/redirected?rj-tok=abc");
    }

    // ---- (7) finalizeBody: reject_truncated CLEARS the body at cap (the group-c fork) ----
    {   // reject on + truncated -> body cleared, response fails (CoverArt semantics)
        core::HttpRequest rq; rq.reject_truncated = true;
        core::HttpResponse rs; rs.truncated = true; rs.body = "\x89PNG-partial-cover";
        core::finalizeBody(rs, rq);
        CHECK(rs.body.empty());
        CHECK(rs.ok == false);
    }
    {   // reject on, not truncated -> body kept, ok
        core::HttpRequest rq; rq.reject_truncated = true;
        core::HttpResponse rs; rs.truncated = false; rs.body = "full-image";
        core::finalizeBody(rs, rq);
        CHECK(rs.body == "full-image"); CHECK(rs.ok);
    }
    {   // reject OFF (groups a/b) -> a truncated body is KEPT (cap-and-keep, e.g. MBLookup)
        core::HttpRequest rq; rq.reject_truncated = false;
        core::HttpResponse rs; rs.truncated = true; rs.body = "kept-partial";
        core::finalizeBody(rs, rq);
        CHECK(rs.body == "kept-partial"); CHECK(rs.ok);
    }

    // ---- (8) scheme predicate: plain-HTTP (CDRipper AR/CTDB) gets NO secure flag ----
    CHECK(core::urlIsSecureScheme("http://www.accuraterip.com/accuraterip/x.bin") == false);
    CHECK(core::urlIsSecureScheme("http://db.cuetools.net/lookup2.php")           == false);
    CHECK(core::urlIsSecureScheme("https://coverartarchive.org/release/x/front")  == true);
    CHECK(core::urlIsSecureScheme("https://itunes.apple.com/search")              == true);
    CHECK(core::urlIsSecureScheme("")                                             == false);

    // ---- (9) RedirectPolicy default is FollowAll (byte-identical to old follow=true) ----
    {
        core::HttpRequest rq;
        CHECK(rq.redirect == core::RedirectPolicy::FollowAll);
    }

    // ═══ slice 4: cancel token + persistent sessions ═════════════════════════

    // ---- (10) new fields default inert — the six shipped one-shot sites unchanged ----
    {
        core::HttpRequest rq;
        CHECK(rq.cancel == nullptr);            // not cancellable unless asked
        CHECK(rq.pragma_no_cache == false);     // no new wire header for shipped sites
        core::HttpResponse rs;
        CHECK(rs.cancelled == false);
        CHECK(rs.read_error == false);
    }

    // ---- (11) finalizeCancelled: fails + marks cancelled + CLEARS the partial body
    //           (a half-downloaded segment must never reach the decoder path) ----
    {
        core::HttpResponse rs; rs.ok = true; rs.body = "half-a-segment";
        core::finalizeCancelled(rs);
        CHECK(rs.cancelled);
        CHECK(rs.ok == false);
        CHECK(rs.body.empty());
    }

    // ---- (12) default openSession forwarder: session config folded into requests,
    //           delegates to fetch() — so FakeHttp-backed tests see session traffic ----
    {
        FakeHttp h; h.next.ok = true; h.next.status = 200; h.next.body = "ok";
        core::HttpSessionConfig cfg; cfg.user_agent = "UA-X/1.0"; cfg.timeout_ms = 5000;
        auto s = static_cast<core::IHttp&>(h).openSession(cfg);
        CHECK(s != nullptr);
        core::HttpRequest rq; rq.url = "https://api/x";
        auto r = s->fetch(rq);
        CHECK(h.calls == 1);                    // routed through fetch()
        CHECK(h.last.url == "https://api/x");
        CHECK(h.last.user_agent == "UA-X/1.0"); // folded from session config
        CHECK(h.last.timeout_ms == 5000);
        CHECK(r.ok && r.body == "ok");
        // explicit per-request values are not clobbered by the forwarder
        core::HttpRequest rq2; rq2.url = "https://api/y";
        rq2.user_agent = "UA-Y/2.0"; rq2.timeout_ms = 250;
        s->fetch(rq2);
        CHECK(h.last.user_agent == "UA-Y/2.0");
        CHECK(h.last.timeout_ms == 250);
    }

    // ---- (13) the cancel token rides the request through the seam, and the
    //           transport-side contract (finalizeCancelled on a set token) holds ----
    {
        FakeHttp h; h.next.ok = true;
        std::atomic<bool> stop{false};
        core::HttpRequest rq; rq.url = "https://seg/1.aac"; rq.cancel = &stop;
        h.fetch(rq);
        CHECK(h.last.cancel == &stop);          // pointer passthrough, no copy games
        stop.store(true);                       // model the transport's per-chunk poll
        core::HttpResponse rs; rs.body = "partial";
        if (h.last.cancel && h.last.cancel->load()) core::finalizeCancelled(rs);
        CHECK(rs.cancelled && !rs.ok && rs.body.empty());
    }

    if (!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
