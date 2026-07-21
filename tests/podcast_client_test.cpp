// podcast_client_test - proves the PodcastClient fetch glue (slice 2) headless,
// with no network: a FakeHttp injected via core::setHttp stands in for the real
// transport. Asserts the request PodcastClient builds (feed-sized cap, loose
// timeout, friendly UA) and the fetched-vs-parsed distinction the honest-failure
// toasts rely on. The real HTTP impl is linked only to define core::http()/
// setHttp(); the fake overrides it at runtime. Pure, both matrix jobs.

#include "PodcastClient.h"
#include "core/IHttp.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// Fake transport: returns a preset response, captures the request it was handed.
struct FakeHttp : core::IHttp {
    core::HttpResponse next;
    core::HttpRequest  last;
    int calls = 0;
    core::HttpResponse fetch(const core::HttpRequest& req) override {
        last = req; ++calls;
        return next;
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

    core::setHttp(nullptr);   // restore

    if (g_fail == 0) std::printf("podcast_client_test: ALL PASS\n");
    else             std::printf("podcast_client_test: %d FAILURE(S)\n", g_fail);
    return g_fail != 0 ? 1 : 0;
}
