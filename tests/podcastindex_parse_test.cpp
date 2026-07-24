// podcastindex_parse_test - proves the Podcast Index client's pure pieces in
// ISOLATION before any [Podcasts] UI wiring depends on them (slice 6):
//   1. The reused SHA-1 (MBLookup::sha1) against the canonical "abc" test vector,
//      then PodcastIndex::authToken determinism + 40-char lowercase-hex format +
//      time-sensitivity (a different X-Auth-Date must yield a different token).
//   2. PodcastIndex::parse over a captured byterm fixture (title/url/author/artwork
//      extraction, whitespace trim, feed-less-of-url skip) and the defensive floor
//      (empty / malformed / no-"feeds" -> empty vector, never a throw).
//
// Pure (no live network - search() is not called here). Fixture via
// PODCASTINDEX_FIXTURE_DIR (set by CMake). Both matrix jobs.

#include "PodcastIndex.h"
#include "MBLookup.h"

#include <cstdint>
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

static std::string sha1hex(const std::string& s) {
    std::uint8_t d[20];
    MBLookup::sha1(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), d);
    static const char* h = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 20; ++i) { out += h[d[i] >> 4]; out += h[d[i] & 0x0F]; }
    return out;
}

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    // ── SHA-1 canonical vector: proves the reused hash is byte-correct ──────────
    CHECK(sha1hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d",
          "sha1(\"abc\") wrong: %s", sha1hex("abc").c_str());
    CHECK(sha1hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709",
          "sha1(\"\") wrong: %s", sha1hex("").c_str());

    // ── authToken: deterministic, 40 lowercase-hex, time-sensitive ──────────────
    std::string t1 = PodcastIndex::authToken("mykey", "mysecret", 1700000000);
    std::string t2 = PodcastIndex::authToken("mykey", "mysecret", 1700000000);
    std::string t3 = PodcastIndex::authToken("mykey", "mysecret", 1700000001);
    CHECK(t1 == t2, "authToken not deterministic");
    CHECK(t1 != t3, "authToken not time-sensitive (X-Auth-Date window)");
    CHECK(t1.size() == 40, "authToken not 40 chars: %zu", t1.size());
    bool lowhex = true;
    for (char c : t1) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) lowhex = false;
    CHECK(lowhex, "authToken not lowercase hex: %s", t1.c_str());
    // Exact-value check against the concatenation the API specifies.
    CHECK(t1 == sha1hex("mykeymysecret1700000000"), "authToken != sha1(key+secret+time)");

    // ── Defensive floor: never throw on junk ────────────────────────────────────
    CHECK(PodcastIndex::parse("").empty(),            "empty body should parse to no feeds");
    CHECK(PodcastIndex::parse("{not json").empty(),   "malformed body should parse to no feeds");
    CHECK(PodcastIndex::parse("{\"count\":0}").empty(), "no feeds[] should parse to no feeds");
    CHECK(PodcastIndex::parse("{\"feeds\":\"x\"}").empty(), "non-array feeds should be ignored");

    // ── Real-shape fixture ──────────────────────────────────────────────────────
    std::string body = slurp(std::string(PODCASTINDEX_FIXTURE_DIR) + "/byterm.json");
    CHECK(!body.empty(), "fixture byterm.json not found / empty");
    auto feeds = PodcastIndex::parse(body);
    // Fixture has 3 feed objects; one has an empty url and must be skipped.
    CHECK(feeds.size() == 2, "expected 2 usable feeds, got %zu", feeds.size());
    if (feeds.size() >= 1) {
        CHECK(feeds[0].title  == "Batman University", "title[0]=%s",  feeds[0].title.c_str());
        CHECK(feeds[0].url    == "https://feeds.example.com/batman.xml", "url[0]=%s", feeds[0].url.c_str());
        CHECK(feeds[0].author == "Steve Webb", "author[0]=%s", feeds[0].author.c_str());
        CHECK(!feeds[0].artwork.empty(), "artwork[0] empty");
    }
    if (feeds.size() >= 2) {
        // Second usable feed exercises the "image" fallback + a non-ASCII title.
        CHECK(feeds[1].title == "Café del Podcast", "title[1]=%s", feeds[1].title.c_str());
        CHECK(feeds[1].artwork == "https://img.example.com/cafe.png", "artwork[1] fallback=%s",
              feeds[1].artwork.c_str());
    }

    std::printf("%s: %d checks, %d failures\n", (g_fail ? "FAIL" : "PASS"), g_checks, g_fail);
    return g_fail ? 1 : 0;
}
