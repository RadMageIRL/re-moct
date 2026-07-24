// podcast_parse_test - proves include/PodcastFeed.h in ISOLATION before any
// [Podcasts] UI / HTTP wiring lands (Podcasts slice 1, probe-first discipline:
// the one genuinely-new, genuinely-messy routine - XML-in-the-wild - is validated
// standalone against real feeds first).
//
// Two layers of proof:
//   1. Inline rule cases: hand-built RSS exercising each parser rule with a known
//      answer - the three itunes:duration forms, CDATA, HTML-in-description,
//      numeric entities, episode-level itunes:image override, enclosure attribute
//      order, entity-in-URL, enclosure-less-item skip+count, and the defensive
//      floor (empty / non-XML / truncated -> ok=false, never a crash).
//   2. Real fixtures: three current subscribed feeds captured verbatim (channel
//      head + first 15 items) across two host shapes - ART19 (Adam Carolla Show,
//      The Adam and Dr. Drew Show) and acast (WTF with Marc Maron). Proves the
//      parser survives the full real mess and counts sanely.
//
// Pure (no curses/audio/net), runs in both matrix jobs. Fixtures are found via
// PODCAST_FIXTURE_DIR (set by CMake).

#include "PodcastFeed.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) do {                                                 \
    ++g_checks;                                                               \
    if (!(cond)) {                                                            \
        ++g_fail;                                                             \
        std::printf("FAIL %s:%d  %s\n  ", __FILE__, __LINE__, #cond);         \
        std::printf(__VA_ARGS__);                                             \
        std::printf("\n");                                                    \
    }                                                                         \
} while (0)

static bool startsWith(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
}

// ─── 1. Inline rule cases ────────────────────────────────────────────────────

static void test_minimal_no_itunes() {
    const std::string xml = R"(<?xml version="1.0"?>
<rss version="2.0">
  <channel>
    <title>Bare Minimal Cast</title>
    <link>https://example.com/bare</link>
    <description>Nothing fancy.</description>
    <item>
      <title>Ep One</title>
      <enclosure url="https://example.com/1.mp3" type="audio/mpeg" length="1000"/>
      <guid>ep-1</guid>
      <pubDate>Wed, 15 Jan 2025 08:00:00 GMT</pubDate>
    </item>
    <item>
      <title>Ep Two</title>
      <enclosure url="https://example.com/2.mp3" type="audio/mpeg" length="2000"/>
      <guid>ep-2</guid>
    </item>
  </channel>
</rss>)";
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.ok, "minimal feed should parse ok");
    CHECK(f.title == "Bare Minimal Cast", "title=[%s]", f.title.c_str());
    CHECK(f.link == "https://example.com/bare", "link=[%s]", f.link.c_str());
    CHECK(f.image_url.empty(), "no feed image expected, got [%s]", f.image_url.c_str());
    CHECK(f.episodes.size() == 2, "episodes=%zu", f.episodes.size());
    CHECK(f.skipped_episodes == 0, "skipped=%d", f.skipped_episodes);
    if (f.episodes.size() == 2) {
        CHECK(f.episodes[0].title == "Ep One", "ep0 title=[%s]", f.episodes[0].title.c_str());
        CHECK(f.episodes[0].audio_url == "https://example.com/1.mp3", "ep0 url=[%s]", f.episodes[0].audio_url.c_str());
        CHECK(f.episodes[0].guid == "ep-1", "ep0 guid=[%s]", f.episodes[0].guid.c_str());
        CHECK(f.episodes[0].duration_sec == 0, "ep0 dur=%lld", (long long)f.episodes[0].duration_sec);
        CHECK(f.episodes[0].image_url.empty(), "ep0 image should be empty");
        CHECK(f.episodes[0].pub_date_unix > 1600000000 && f.episodes[0].pub_date_unix < 2000000000,
              "ep0 pub_date_unix=%lld (want ~2025)", (long long)f.episodes[0].pub_date_unix);
        CHECK(f.episodes[1].pub_date_unix == 0, "ep1 has no pubDate -> 0, got %lld",
              (long long)f.episodes[1].pub_date_unix);
    }
}

static void test_duration_forms() {
    const std::string xml = R"(<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd">
  <channel><title>Durations</title>
    <item><title>bare-seconds</title><enclosure url="http://x/a.mp3"/><itunes:duration>3600</itunes:duration></item>
    <item><title>mm-ss</title><enclosure url="http://x/b.mp3"/><itunes:duration>58:30</itunes:duration></item>
    <item><title>hh-mm-ss</title><enclosure url="http://x/c.mp3"/><itunes:duration>1:02:30</itunes:duration></item>
    <item><title>garbage-dur</title><enclosure url="http://x/d.mp3"/><itunes:duration>not-a-time</itunes:duration></item>
    <item><title>no-dur</title><enclosure url="http://x/e.mp3"/></item>
  </channel></rss>)";
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.episodes.size() == 5, "episodes=%zu", f.episodes.size());
    if (f.episodes.size() == 5) {
        CHECK(f.episodes[0].duration_sec == 3600, "bare seconds -> %lld", (long long)f.episodes[0].duration_sec);
        CHECK(f.episodes[1].duration_sec == 3510, "MM:SS 58:30 -> %lld", (long long)f.episodes[1].duration_sec);
        CHECK(f.episodes[2].duration_sec == 3750, "HH:MM:SS 1:02:30 -> %lld", (long long)f.episodes[2].duration_sec);
        CHECK(f.episodes[3].duration_sec == 0, "garbage -> %lld", (long long)f.episodes[3].duration_sec);
        CHECK(f.episodes[4].duration_sec == 0, "absent -> %lld", (long long)f.episodes[4].duration_sec);
    }
}

static void test_cdata_html_entities() {
    const std::string xml = R"(<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd">
  <channel><title>CDATA Cast</title>
    <item>
      <title><![CDATA[Bob & Ray "Live"]]></title>
      <description><![CDATA[<p>Hello <b>world</b> &amp; friends</p>]]></description>
      <enclosure url="http://x/c.mp3"/>
    </item>
    <item>
      <title>It&#39;s a &#8217;test&#8217; &amp; more</title>
      <enclosure url="http://x/d.mp3"/>
    </item>
  </channel></rss>)";
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.episodes.size() == 2, "episodes=%zu", f.episodes.size());
    if (f.episodes.size() == 2) {
        CHECK(f.episodes[0].title == "Bob & Ray \"Live\"", "cdata title=[%s]", f.episodes[0].title.c_str());
        CHECK(f.episodes[0].description == "Hello world & friends",
              "html-stripped desc=[%s]", f.episodes[0].description.c_str());
        // &#39; -> ' , &#8217; -> U+2019 (E2 80 99) , &amp; -> &
        CHECK(f.episodes[1].title == "It's a \xE2\x80\x99test\xE2\x80\x99 & more",
              "numeric-entity title=[%s]", f.episodes[1].title.c_str());
    }
}

static void test_episode_image_override() {
    const std::string xml = R"(<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd">
  <channel><title>Img Cast</title>
    <itunes:image href="http://x/feed.jpg"/>
    <item><title>with-image</title><enclosure url="http://x/1.mp3"/><itunes:image href="http://x/ep1.jpg"/></item>
    <item><title>no-image</title><enclosure url="http://x/2.mp3"/></item>
  </channel></rss>)";
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.image_url == "http://x/feed.jpg", "feed image=[%s]", f.image_url.c_str());
    CHECK(f.episodes.size() == 2, "episodes=%zu", f.episodes.size());
    if (f.episodes.size() == 2) {
        CHECK(f.episodes[0].image_url == "http://x/ep1.jpg", "ep0 image=[%s]", f.episodes[0].image_url.c_str());
        CHECK(f.episodes[1].image_url.empty(), "ep1 image should be empty -> inherit");
    }
}

// <podcast:chapters url type> extraction into PodcastEpisode::chapters_url, and
// the type gating that keeps a non-JSON document from ever being fetched.
static void test_chapters_url_extraction() {
    const std::string xml = R"(<rss version="2.0" xmlns:podcast="https://podcastindex.org/namespace/1.0">
  <channel><title>Chap Cast</title>
    <item><title>json-type</title><enclosure url="http://x/1.mp3"/>
      <podcast:chapters url="http://x/1.json" type="application/json"/></item>
    <item><title>no-type</title><enclosure url="http://x/2.mp3"/>
      <podcast:chapters url="http://x/2.json"/></item>
    <item><title>xml-type</title><enclosure url="http://x/3.mp3"/>
      <podcast:chapters url="http://x/3.xml" type="application/xml+psc"/></item>
    <item><title>none</title><enclosure url="http://x/4.mp3"/></item>
    <item><title>entity-url</title><enclosure url="http://x/5.mp3"/>
      <podcast:chapters url="http://x/5.json?a=1&amp;b=2" type="application/json; charset=utf-8"/></item>
  </channel></rss>)";
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.episodes.size() == 5, "episodes=%zu", f.episodes.size());
    if (f.episodes.size() == 5) {
        CHECK(f.episodes[0].chapters_url == "http://x/1.json",
              "explicit json type -> [%s]", f.episodes[0].chapters_url.c_str());
        CHECK(f.episodes[1].chapters_url == "http://x/2.json",
              "absent type is eligible -> [%s]", f.episodes[1].chapters_url.c_str());
        CHECK(f.episodes[2].chapters_url.empty(),
              "non-json type -> not fetched -> empty, got [%s]", f.episodes[2].chapters_url.c_str());
        CHECK(f.episodes[3].chapters_url.empty(),
              "no chapters element -> empty, got [%s]", f.episodes[3].chapters_url.c_str());
        CHECK(f.episodes[4].chapters_url == "http://x/5.json?a=1&b=2",
              "entity-unescaped url + json w/ params -> [%s]", f.episodes[4].chapters_url.c_str());
    }
}

static void test_malformed_item_skipped() {
    const std::string xml = R"(<rss version="2.0"><channel><title>Malformed Cast</title>
    <item><title>good-1</title><enclosure url="http://x/1.mp3"/></item>
    <item><title>no-enclosure should be skipped</title><guid>x</guid></item>
    <item><title>good-2</title><enclosure url="http://x/2.mp3"/></item>
  </channel></rss>)";
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.ok, "feed with 2 valid items should be ok");
    CHECK(f.episodes.size() == 2, "valid episodes=%zu (want 2)", f.episodes.size());
    CHECK(f.skipped_episodes == 1, "skipped=%d (want 1)", f.skipped_episodes);
    if (f.episodes.size() == 2) {
        CHECK(f.episodes[0].title == "good-1", "ep0=[%s]", f.episodes[0].title.c_str());
        CHECK(f.episodes[1].title == "good-2", "ep1=[%s]", f.episodes[1].title.c_str());
    }
}

static void test_enclosure_attr_order_and_entity_url() {
    const std::string xml = R"(<rss version="2.0"><channel><title>Enc Cast</title>
    <item><title>url-first</title><enclosure url="http://x/a.mp3?x=1&amp;y=2" type="audio/mpeg" length="10"/></item>
    <item><title>url-last</title><enclosure type="audio/mpeg" length="20" url="http://x/b.mp3"/></item>
  </channel></rss>)";
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.episodes.size() == 2, "episodes=%zu", f.episodes.size());
    if (f.episodes.size() == 2) {
        CHECK(f.episodes[0].audio_url == "http://x/a.mp3?x=1&y=2",
              "url-first (entity unescaped)=[%s]", f.episodes[0].audio_url.c_str());
        CHECK(f.episodes[1].audio_url == "http://x/b.mp3",
              "url-last=[%s]", f.episodes[1].audio_url.c_str());
    }
}

static void test_defensive_garbage() {
    // Empty
    PodcastFeed a = parsePodcastFeed("");
    CHECK(!a.ok && a.episodes.empty() && a.skipped_episodes == 0, "empty input -> ok=false, no crash");
    // Non-XML noise
    PodcastFeed b = parsePodcastFeed("not xml at all <<< >>> &&& & &# &#x ;;;");
    CHECK(!b.ok, "garbage input -> ok=false");
    // Valid XML, wrong document (no channel)
    PodcastFeed c = parsePodcastFeed("<html><body><p>hi &amp; bye</p></body></html>");
    CHECK(!c.ok, "non-feed XML -> ok=false");
    // Channel present but zero items
    PodcastFeed d = parsePodcastFeed("<rss><channel><title>Empty</title></channel></rss>");
    CHECK(!d.ok && d.episodes.empty(), "channel with no items -> ok=false");
    CHECK(d.title == "Empty", "channel title still parsed=[%s]", d.title.c_str());
    // Truncated mid-element (no closing '>' on enclosure, no </item>, no </channel>)
    PodcastFeed e = parsePodcastFeed(
        "<rss><channel><title>Trunc</title><item><title>x</title><enclosure url=\"http://x/1.mp3\"");
    CHECK(!e.ok, "truncated feed -> ok=false, no crash");
    // Atom (deferred) must not crash and yields ok=false (no <item>)
    PodcastFeed g = parsePodcastFeed(
        "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Atomic</title>"
        "<entry><title>a</title></entry></feed>");
    CHECK(!g.ok, "Atom feed (deferred) -> ok=false, no crash");
}

// ─── 2. Real fixtures ────────────────────────────────────────────────────────

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static void test_real_fixture(const std::string& file, const std::string& expect_title) {
    std::string path = std::string(PODCAST_FIXTURE_DIR) + "/" + file;
    std::string xml;
    if (!readFile(path, xml)) {
        ++g_fail; ++g_checks;
        std::printf("FAIL could not read fixture %s\n", path.c_str());
        return;
    }
    PodcastFeed f = parsePodcastFeed(xml);
    CHECK(f.ok, "%s: should parse ok", file.c_str());
    CHECK(f.title == expect_title, "%s: title=[%s] want [%s]",
          file.c_str(), f.title.c_str(), expect_title.c_str());
    CHECK(f.episodes.size() == 15, "%s: episodes=%zu (want 15)", file.c_str(), f.episodes.size());
    CHECK(f.skipped_episodes == 0, "%s: skipped=%d (want 0)", file.c_str(), f.skipped_episodes);
    CHECK(startsWith(f.image_url, "http"), "%s: feed image=[%s]", file.c_str(), f.image_url.c_str());

    int with_audio = 0, with_dur = 0, with_date = 0;
    for (const auto& ep : f.episodes) {
        if (startsWith(ep.audio_url, "http")) ++with_audio;
        if (ep.duration_sec > 0) ++with_dur;
        if (ep.pub_date_unix > 0) ++with_date;
    }
    CHECK(with_audio == (int)f.episodes.size(), "%s: %d/%zu episodes have http audio",
          file.c_str(), with_audio, f.episodes.size());
    CHECK(with_dur == (int)f.episodes.size(), "%s: %d/%zu episodes have a duration",
          file.c_str(), with_dur, f.episodes.size());
    CHECK(with_date >= 1, "%s: %d episodes have a parsed date (want >=1)", file.c_str(), with_date);

    // Sanity: first episode has a non-empty title.
    if (!f.episodes.empty())
        CHECK(!f.episodes.front().title.empty(), "%s: first episode has a title", file.c_str());
}

int main() {
    test_minimal_no_itunes();
    test_duration_forms();
    test_cdata_html_entities();
    test_episode_image_override();
    test_chapters_url_extraction();
    test_malformed_item_skipped();
    test_enclosure_attr_order_and_entity_url();
    test_defensive_garbage();

    test_real_fixture("adam-carolla-show.art19.xml",        "Adam Carolla Show");
    test_real_fixture("adam-and-dr-drew-show.art19.xml",    "The Adam and Dr. Drew Show");
    test_real_fixture("wtf-with-marc-maron.acast.xml",      "WTF with Marc Maron Podcast");

    if (g_fail == 0)
        std::printf("podcast_parse_test: ALL PASS (%d checks)\n", g_checks);
    else
        std::printf("podcast_parse_test: %d FAILURE(S) of %d checks\n", g_fail, g_checks);
    return g_fail != 0 ? 1 : 0;
}
