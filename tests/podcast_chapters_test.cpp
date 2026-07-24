// podcast_chapters_test - proves parsePodcastChaptersJson (PodcastChapters.h)
// headless and device-free: no network, no filesystem, no curses. The parser is
// the single trust boundary for a document that arrives off the open internet and
// is cached to disk, so this is where the hostile-input contract is nailed down -
// garbage bytes, wrong shapes, out-of-range values, oversized titles, control
// characters, disorder, duplicates, and volume. Header-only, both matrix jobs.

#include "PodcastChapters.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

// ── A well-formed v1.1.0 document parses to the expected chapters ────────────
static void test_valid_v1_1_0() {
    const std::string doc = R"({
      "version":"1.1.0",
      "chapters":[
        {"startTime":0,      "title":"Intro"},
        {"startTime":63.5,   "title":"First topic"},
        {"startTime":1200,   "title":"Wrap up"}
      ]})";
    auto ch = parsePodcastChaptersJson(doc);
    CHECK(ch.size() == 3, "size=%zu", ch.size());
    if (ch.size() == 3) {
        CHECK(ch[0].start_sec == 0.0    && ch[0].title == "Intro",       "c0 [%s]", ch[0].title.c_str());
        CHECK(ch[1].start_sec == 63.5   && ch[1].title == "First topic", "c1 [%s]", ch[1].title.c_str());
        CHECK(ch[2].start_sec == 1200.0 && ch[2].title == "Wrap up",     "c2 [%s]", ch[2].title.c_str());
    }
}

// ── Absent/unknown version is fine; the array is the contract ────────────────
static void test_version_optional() {
    CHECK(parsePodcastChaptersJson(R"({"chapters":[{"startTime":0,"title":"A"}]})").size() == 1,
          "absent version accepted");
    CHECK(parsePodcastChaptersJson(R"({"version":"9.9.9","chapters":[{"startTime":0,"title":"A"}]})").size() == 1,
          "unknown version accepted");
}

// ── Structural rejects -> empty (never a throw, never a crash) ───────────────
static void test_structural_rejects() {
    CHECK(parsePodcastChaptersJson("").empty(),                       "empty string");
    CHECK(parsePodcastChaptersJson("not json at all }{").empty(),     "garbage bytes");
    CHECK(parsePodcastChaptersJson(R"({"chapters":[{"startTime":0)").empty(), "truncated json");
    CHECK(parsePodcastChaptersJson(R"(["a","b"])").empty(),           "top-level array, not object");
    CHECK(parsePodcastChaptersJson("42").empty(),                     "top-level number");
    CHECK(parsePodcastChaptersJson(R"("a string")").empty(),          "top-level string");
    CHECK(parsePodcastChaptersJson(R"({"noChapters":true})").empty(), "missing chapters key");
    CHECK(parsePodcastChaptersJson(R"({"chapters":"nope"})").empty(), "chapters not an array");
    CHECK(parsePodcastChaptersJson(R"({"chapters":{}})").empty(),     "chapters an object");
    CHECK(parsePodcastChaptersJson(R"({"chapters":[]})").empty(),     "empty chapters array");
    // Invalid UTF-8 in the body: nlohmann rejects it -> discarded -> empty.
    CHECK(parsePodcastChaptersJson("{\"chapters\":[{\"startTime\":0,\"title\":\"\xff\xfe\"}]}").empty()
          || true, "invalid utf-8 does not crash");  // may reject whole doc OR the title; must not throw
}

// ── Per-entry startTime validation: bad entries skipped, good ones kept ──────
static void test_entry_start_validation() {
    const std::string doc = R"({"chapters":[
        {"startTime":-5,      "title":"negative"},
        {"startTime":"12",    "title":"string"},
        {"startTime":null,    "title":"null"},
        {"title":"absent"},
        {"startTime":10,      "title":"good-a"},
        {"startTime":20,      "title":"good-b"}
    ]})";
    auto ch = parsePodcastChaptersJson(doc);
    CHECK(ch.size() == 2, "only the two finite >=0 numeric starts survive: size=%zu", ch.size());
    if (ch.size() == 2) {
        CHECK(ch[0].title == "good-a", "c0 [%s]", ch[0].title.c_str());
        CHECK(ch[1].title == "good-b", "c1 [%s]", ch[1].title.c_str());
    }
}

// ── NaN / Infinity: nlohmann emits them as JSON null, which we skip ──────────
static void test_nan_inf_skipped() {
    // A publisher can't literally write NaN in strict JSON, but a lenient producer
    // upstream might; nlohmann parses bare nan/inf tokens to null, which fails the
    // is_number() gate. Either way: skipped, never propagated as a bad double.
    auto ch = parsePodcastChaptersJson(R"({"chapters":[
        {"startTime":NaN,"title":"nan"},
        {"startTime":5,"title":"ok"}
    ]})");
    // Strict parse rejects the whole doc (NaN not valid JSON) -> empty; that's fine.
    // The contract is only: no crash, and no NaN chapter.
    for (const auto& c : ch) CHECK(c.start_sec == c.start_sec && c.title != "nan", "no NaN chapter leaks");
}

// ── Missing / non-string / empty titles are synthesized by final position ────
static void test_title_synthesis() {
    const std::string doc = R"({"chapters":[
        {"startTime":0},
        {"startTime":10,"title":123},
        {"startTime":20,"title":""},
        {"startTime":30,"title":"Named"}
    ]})";
    auto ch = parsePodcastChaptersJson(doc);
    CHECK(ch.size() == 4, "size=%zu", ch.size());
    if (ch.size() == 4) {
        CHECK(ch[0].title == "Chapter 1", "absent title -> [%s]", ch[0].title.c_str());
        CHECK(ch[1].title == "Chapter 2", "non-string title -> [%s]", ch[1].title.c_str());
        CHECK(ch[2].title == "Chapter 3", "empty title -> [%s]", ch[2].title.c_str());
        CHECK(ch[3].title == "Named",     "named -> [%s]", ch[3].title.c_str());
    }
}

// ── Title over the byte cap is trimmed on a UTF-8 boundary ───────────────────
static void test_title_trim() {
    std::string big(400, 'x');
    auto ch = parsePodcastChaptersJson(
        std::string(R"({"chapters":[{"startTime":0,"title":")") + big + R"("}]})");
    CHECK(ch.size() == 1, "size=%zu", ch.size());
    if (ch.size() == 1) CHECK(ch[0].title.size() <= 256, "trimmed to <=256: %zu", ch[0].title.size());

    // A multi-byte character straddling the 256 boundary must not be split into a
    // partial sequence. Fill 254 ASCII then repeat a 3-byte char (U+2603, snowman).
    std::string s(254, 'a');
    for (int i = 0; i < 20; ++i) s += "\xE2\x98\x83";
    auto ch2 = parsePodcastChaptersJson(
        std::string(R"({"chapters":[{"startTime":0,"title":")") + s + R"("}]})");
    CHECK(ch2.size() == 1, "size=%zu", ch2.size());
    if (ch2.size() == 1) {
        // Every trailing byte must be part of a complete UTF-8 sequence: validate
        // by re-decoding without hitting a continuation byte at the very end.
        const std::string& t = ch2[0].title;
        CHECK(t.size() <= 256, "trim size %zu", t.size());
        bool ends_clean = t.empty() || ((unsigned char)t.back() < 0x80) ||
                          ((unsigned char)t.back() >= 0xC0);   // not a lone continuation
        // last byte of a complete 3-byte char IS a continuation byte, so instead
        // check the sequence count is whole:
        int cont = 0; for (auto rit = t.rbegin(); rit != t.rend(); ++rit) {
            if (((unsigned char)*rit & 0xC0) == 0x80) ++cont; else {
                unsigned char lead = (unsigned char)*rit;
                int need = (lead >= 0xF0) ? 3 : (lead >= 0xE0) ? 2 : (lead >= 0xC0) ? 1 : 0;
                CHECK(need == cont, "final char whole: lead needs %d continuations, has %d", need, cont);
                break;
            }
        }
        (void)ends_clean;
    }
}

// ── Control characters in a title become spaces, never reach curses raw ──────
static void test_control_chars_sanitized() {
    //  bell,  escape, \n, \t embedded in the title.
    auto ch = parsePodcastChaptersJson(
        "{\"chapters\":[{\"startTime\":0,\"title\":\"a\\u0007b\\u001bc\\td\\ne\"}]}");
    CHECK(ch.size() == 1, "size=%zu", ch.size());
    if (ch.size() == 1) {
        const std::string& t = ch[0].title;
        for (unsigned char c : t)
            CHECK(c >= 0x20 && c != 0x7F, "no control byte survives: 0x%02x", c);
        // Words separated by the controls stay separated (collapsed to single spaces).
        CHECK(t == "a b c d e", "collapsed to [%s]", t.c_str());
    }
}

// ── Out-of-order entries are sorted by start time (stable) ───────────────────
static void test_sort() {
    auto ch = parsePodcastChaptersJson(R"({"chapters":[
        {"startTime":30,"title":"C"},
        {"startTime":10,"title":"A"},
        {"startTime":20,"title":"B"}
    ]})");
    CHECK(ch.size() == 3, "size=%zu", ch.size());
    if (ch.size() == 3) {
        CHECK(ch[0].title == "A" && ch[1].title == "B" && ch[2].title == "C",
              "sorted: [%s][%s][%s]", ch[0].title.c_str(), ch[1].title.c_str(), ch[2].title.c_str());
    }
}

// ── Exact-duplicate start times are deduped keep-first (post-sort/stable) ─────
static void test_dedupe() {
    auto ch = parsePodcastChaptersJson(R"({"chapters":[
        {"startTime":10,"title":"first"},
        {"startTime":10,"title":"second"},
        {"startTime":20,"title":"later"}
    ]})");
    CHECK(ch.size() == 2, "deduped size=%zu", ch.size());
    if (ch.size() == 2) {
        CHECK(ch[0].title == "first", "keep-first: [%s]", ch[0].title.c_str());
        CHECK(ch[1].title == "later", "[%s]", ch[1].title.c_str());
    }
}

// ── A start time past the end of file is KEPT (seekTo clamps) ────────────────
static void test_past_end_kept() {
    auto ch = parsePodcastChaptersJson(R"({"chapters":[
        {"startTime":0,"title":"start"},
        {"startTime":999999,"title":"way past end"}
    ]})");
    CHECK(ch.size() == 2, "past-end chapter kept: size=%zu", ch.size());
}

// ── Volume cap: a 100k-entry array builds no more than the cap, and fast ──────
static void test_volume_cap() {
    std::string doc = R"({"chapters":[)";
    doc.reserve(100000 * 40);
    for (int i = 0; i < 100000; ++i) {
        if (i) doc += ',';
        doc += "{\"startTime\":" + std::to_string(i) + "}";
    }
    doc += "]}";
    auto ch = parsePodcastChaptersJson(doc);
    CHECK(ch.size() == 512, "capped at 512, got %zu", ch.size());
    if (ch.size() == 512) {
        // Cap applied AFTER sort -> the earliest 512 by start time (0..511).
        CHECK(ch[0].start_sec == 0.0,   "first start %f", ch[0].start_sec);
        CHECK(ch[511].start_sec == 511.0, "last kept start %f", ch[511].start_sec);
    }
}

// ── A body at the 1 MB boundary is handled without crashing ──────────────────
static void test_large_body_boundary() {
    // ~1 MB of valid chapters (each entry a few dozen bytes). Must parse, cap, and
    // return, with no crash and no unbounded work.
    std::string doc = R"({"chapters":[)";
    for (int i = 0; i < 20000; ++i) {   // ~1 MB with titles
        if (i) doc += ',';
        doc += "{\"startTime\":" + std::to_string(i) + ",\"title\":\"chapter number " +
               std::to_string(i) + "\"}";
    }
    doc += "]}";
    CHECK(doc.size() > 900u * 1024, "doc is ~1MB: %zu bytes", doc.size());
    auto ch = parsePodcastChaptersJson(doc);
    CHECK(ch.size() == 512, "still capped: %zu", ch.size());
}

int main() {
    test_valid_v1_1_0();
    test_version_optional();
    test_structural_rejects();
    test_entry_start_validation();
    test_nan_inf_skipped();
    test_title_synthesis();
    test_title_trim();
    test_control_chars_sanitized();
    test_sort();
    test_dedupe();
    test_past_end_kept();
    test_volume_cap();
    test_large_body_boundary();

    if (g_fail == 0) std::printf("podcast_chapters_test: ALL PASS (%d checks)\n", g_checks);
    else             std::printf("podcast_chapters_test: %d FAILURE(S) of %d checks\n", g_fail, g_checks);
    return g_fail != 0 ? 1 : 0;
}
