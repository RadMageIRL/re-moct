// podcast_client_test - proves the PodcastClient fetch glue (slice 2) headless,
// with no network: a FakeHttp injected via core::setHttp stands in for the real
// transport. Asserts the request PodcastClient builds (feed-sized cap, loose
// timeout, friendly UA) and the fetched-vs-parsed distinction the honest-failure
// toasts rely on. The real HTTP impl is linked only to define core::http()/
// setHttp(); the fake overrides it at runtime. Pure, both matrix jobs.

#include "PodcastClient.h"
#include "core/IHttp.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// Fake transport: returns a preset response, captures the request it was handed.
// fetchToFile writes canned bytes to the destination and reports progress, so the
// download glue can be proven with no network.
struct FakeHttp : core::IHttp {
    core::HttpResponse next;
    core::HttpRequest  last;
    int calls = 0;
    // fetchToFile fakery
    std::string        file_body;              // bytes to "download"
    bool               file_ok   = true;       // simulate success/failure
    core::HttpRequest  last_dl;
    std::uint64_t      last_progress_recv = 0;  // last progress callback's received
    std::uint64_t      last_progress_total = 0;
    int                progress_calls = 0;

    core::HttpResponse fetch(const core::HttpRequest& req) override {
        last = req; ++calls;
        return next;
    }
    core::HttpResponse fetchToFile(const core::HttpRequest& req, const std::string& dest,
                                   const core::ProgressFn& progress) override {
        last_dl = req;
        core::HttpResponse res;
        if (!file_ok) { res.ok = false; return res; }         // no file written
        std::ofstream f(dest, std::ios::binary);
        f.write(file_body.data(), (std::streamsize)file_body.size());
        f.close();
        if (progress) {
            progress(0, file_body.size());
            progress(file_body.size(), file_body.size());
            ++progress_calls;
            last_progress_recv  = file_body.size();
            last_progress_total = file_body.size();
        }
        res.ok = true; res.status = 200;
        return res;
    }
};

static const char* kFeed = R"(<?xml version="1.0"?>
<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd">
  <channel>
    <title>Test Cast</title>
    <itunes:image href="http://x/art.jpg"/>
    <item>
      <title>Ep One</title>
      <enclosure url="http://x/1.mp3" type="audio/mpeg" length="10"/>
      <itunes:duration>1:02:30</itunes:duration>
      <pubDate>Wed, 15 Jan 2025 08:00:00 GMT</pubDate>
    </item>
    <item>
      <title>Ep Two</title>
      <enclosure url="http://x/2.mp3" type="audio/mpeg" length="20"/>
    </item>
  </channel>
</rss>)";

int main() {
    FakeHttp h;
    core::setHttp(&h);

    // ---- (1) a good fetch: request shape + parsed feed ----
    {
        h.next = core::HttpResponse{}; h.next.ok = true; h.next.status = 200; h.next.body = kFeed;
        PodcastClient::Result r = PodcastClient::fetch("http://x/feed.xml");
        CHECK(h.calls == 1);
        CHECK(h.last.url == "http://x/feed.xml");
        CHECK(h.last.max_body == 64u * 1024 * 1024);   // feed-sized cap (not radio's 4 MB)
        CHECK(h.last.timeout_ms == 30000);
        CHECK(h.last.user_agent == "RE-MOCT/1.4 (podcast client)");
        CHECK(r.fetched);
        CHECK(r.feed.ok);
        CHECK(r.feed.title == "Test Cast");
        CHECK(r.feed.image_url == "http://x/art.jpg");
        CHECK(r.feed.episodes.size() == 2);
        if (r.feed.episodes.size() == 2) {
            CHECK(r.feed.episodes[0].audio_url == "http://x/1.mp3");
            CHECK(r.feed.episodes[0].duration_sec == 3750);   // 1:02:30
        }
    }

    // ---- (2) network failure: ok=false -> not fetched, feed not ok ----
    {
        h.next = core::HttpResponse{}; h.next.ok = false; h.next.body = "";
        PodcastClient::Result r = PodcastClient::fetch("http://x/dead.xml");
        CHECK(!r.fetched);        // -> "Couldn't fetch feed"
        CHECK(!r.feed.ok);
    }

    // ---- (3) fetched but not a feed: fetched=true, feed !ok -> "Not a valid feed" ----
    {
        h.next = core::HttpResponse{}; h.next.ok = true; h.next.status = 200;
        h.next.body = "<html><body>not a podcast</body></html>";
        PodcastClient::Result r = PodcastClient::fetch("http://x/page.html");
        CHECK(r.fetched);
        CHECK(!r.feed.ok);
    }

    // ---- (4) empty URL: no HTTP call, no crash ----
    {
        int before = h.calls;
        PodcastClient::Result r = PodcastClient::fetch("");
        CHECK(h.calls == before);   // fetch() returns early, never hits the transport
        CHECK(!r.fetched);
        CHECK(!r.feed.ok);
    }

    // ---- (5) download() streams to a file + reports progress ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dest = fs::temp_directory_path(ec) / "remoct_podcast_dl_test.bin";
        std::remove(dest.string().c_str());

        h.file_ok = true;
        h.file_body = "AUDIO-BYTES-xxxxxxxxxx";
        h.progress_calls = 0;
        std::uint64_t seen_recv = 0, seen_total = 0;
        core::ProgressFn prog = [&](std::uint64_t r, std::uint64_t t){ seen_recv = r; seen_total = t; };
        bool ok = PodcastClient::download("http://x/ep.mp3", dest.string(), prog, nullptr);
        CHECK(ok);
        CHECK(h.last_dl.url == "http://x/ep.mp3");
        CHECK(h.last_dl.max_body == 0);                 // no cap: streams to disk
        CHECK(h.last_dl.user_agent == "RE-MOCT/1.4 (podcast client)");
        CHECK(h.progress_calls == 1);
        CHECK(seen_recv == h.file_body.size() && seen_total == h.file_body.size());
        // File actually written with the body.
        std::ifstream in(dest.string(), std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        CHECK(ss.str() == h.file_body);
        std::remove(dest.string().c_str());
    }

    // ---- (6) download() failure returns false ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dest = fs::temp_directory_path(ec) / "remoct_podcast_dl_fail.bin";
        h.file_ok = false;
        bool ok = PodcastClient::download("http://x/dead.mp3", dest.string(), {}, nullptr);
        CHECK(!ok);
    }

    // ---- (7) empty url/dest: early-out, transport never reached, returns false ----
    {
        h.last_dl.url = "SENTINEL";
        CHECK(!PodcastClient::download("", "/tmp/x", {}, nullptr));
        CHECK(!PodcastClient::download("http://x/y", "", {}, nullptr));
        CHECK(h.last_dl.url == "SENTINEL");   // fetchToFile never invoked
    }

    // ---- (8) fetchChapters(): kilobyte-scale caps + verbatim body ----
    {
        const char* kDoc = R"({"version":"1.1.0","chapters":[{"startTime":0,"title":"Intro"}]})";
        h.next = core::HttpResponse{}; h.next.ok = true; h.next.status = 200; h.next.body = kDoc;
        int before = h.calls;
        PodcastClient::ChaptersResult r = PodcastClient::fetchChapters("http://x/ch.json");
        CHECK(h.calls == before + 1);
        CHECK(h.last.url == "http://x/ch.json");
        CHECK(h.last.max_body == 1u * 1024 * 1024);       // 1 MB, NOT the feed's 64 MB
        CHECK(h.last.timeout_ms == 15000);                // short, NOT the feed's 30 s
        CHECK(h.last.user_agent == "RE-MOCT/1.4 (podcast client)");
        CHECK(r.fetched);
        CHECK(r.body == kDoc);                            // returned verbatim, unparsed
    }

    // ---- (9) fetchChapters(): network failure -> not fetched, empty body ----
    {
        h.next = core::HttpResponse{}; h.next.ok = false; h.next.body = "";
        PodcastClient::ChaptersResult r = PodcastClient::fetchChapters("http://x/dead.json");
        CHECK(!r.fetched);
        CHECK(r.body.empty());
    }

    // ---- (10) fetchChapters(): empty URL -> no HTTP call ----
    {
        int before = h.calls;
        PodcastClient::ChaptersResult r = PodcastClient::fetchChapters("");
        CHECK(h.calls == before);
        CHECK(!r.fetched);
    }

    core::setHttp(nullptr);   // restore

    if (g_fail == 0) std::printf("podcast_client_test: ALL PASS\n");
    else             std::printf("podcast_client_test: %d FAILURE(S)\n", g_fail);
    return g_fail != 0 ? 1 : 0;
}
