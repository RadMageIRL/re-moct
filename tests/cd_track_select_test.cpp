// cd_track_select_test - CD-S3: which marked row means which TOC track.
//
// The entire risk of the marking UI's engine handoff is "which number means
// what", and that is pure arithmetic over a TOC and a set of synthetic paths -
// so it is provable with no drive, no disc and no modal.
//
// THE THING THIS FILE EXISTS TO PROVE: a track's NUMBER is not its TOC INDEX.
// `number == index + 1` holds on an ordinary audio CD, so `num - 1` passes every
// test written against one. Block 2 is a disc where it does NOT hold, and it
// fails against any implementation that does the subtraction. That is the same
// class as CD-S1's finding (5) - two quantities that coincide on the common case
// and are silently different on the one that matters.
//
// Pure: links nothing.
#include "CdTrackSelect.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fail = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

using namespace cdsel;

// An ordinary 12-track audio CD: numbers 1..12, so number == index + 1.
static std::vector<int> relishNumbers() {
    std::vector<int> n;
    for (int i = 1; i <= 12; ++i) n.push_back(i);
    return n;
}

static std::string row(const char* drive, int n) {
    char b[64];
    std::snprintf(b, sizeof b, "%s:CD Track %02d", drive, n);
    return b;
}

// ── 1. the ordinary disc - the case that must keep working ───────────────────
static void test_ordinary_disc() {
    const auto toc = relishNumbers();
    auto m = toTocIndices({ row("G", 1), row("G", 5), row("G", 12) }, "G", toc);
    CHECK(m.unresolved == 0, "unresolved was %d, wanted 0", m.unresolved);
    CHECK(m.toc_indices.size() == 3, "got %zu indices, wanted 3", m.toc_indices.size());
    CHECK(m.toc_indices[0] == 0,  "track 1 -> index %d, wanted 0",  m.toc_indices[0]);
    CHECK(m.toc_indices[1] == 4,  "track 5 -> index %d, wanted 4",  m.toc_indices[1]);
    CHECK(m.toc_indices[2] == 11, "track 12 -> index %d, wanted 11", m.toc_indices[2]);
}

// ── 2. *** NUMBER IS NOT INDEX *** ───────────────────────────────────────────
// A disc whose audio numbering does not start at 1 and is not contiguous - what
// a data track or a mixed-mode pressing produces. `num - 1` gets every one of
// these wrong, and on the first entry it is not even in range.
static void test_number_is_not_index() {
    const std::vector<int> toc = { 3, 4, 7, 8, 11 };   // 5 TOC entries

    auto m = toTocIndices({ row("G", 3) }, "G", toc);
    CHECK(m.toc_indices.size() == 1 && m.toc_indices[0] == 0,
          "track 3 must be INDEX 0 on this disc, got %zu entries first=%d",
          m.toc_indices.size(), m.toc_indices.empty() ? -99 : m.toc_indices[0]);

    auto m2 = toTocIndices({ row("G", 7) }, "G", toc);
    CHECK(m2.toc_indices.size() == 1 && m2.toc_indices[0] == 2,
          "track 7 must be INDEX 2, got first=%d",
          m2.toc_indices.empty() ? -99 : m2.toc_indices[0]);

    auto m3 = toTocIndices({ row("G", 11) }, "G", toc);
    CHECK(m3.toc_indices.size() == 1 && m3.toc_indices[0] == 4,
          "track 11 must be INDEX 4, got first=%d",
          m3.toc_indices.empty() ? -99 : m3.toc_indices[0]);

    // A number that exists as an INDEX but not as a track number must NOT resolve.
    auto m4 = toTocIndices({ row("G", 1) }, "G", toc);
    CHECK(m4.toc_indices.empty(), "track 1 does not exist on this disc");
    CHECK(m4.unresolved == 1, "it must be COUNTED as unresolved, got %d", m4.unresolved);
}

// ── 3. drive is part of identity ─────────────────────────────────────────────
static void test_drive_is_identity() {
    const auto toc = relishNumbers();
    auto m = toTocIndices({ row("G", 5), row("F", 7) }, "G", toc);
    CHECK(m.toc_indices.size() == 1 && m.toc_indices[0] == 4,
          "only the G row belongs to this rip");
    CHECK(m.unresolved == 1, "the F row must be counted, got %d", m.unresolved);

    // Linux-style multi-char drive spec (sr0) must work the same way.
    auto m2 = toTocIndices({ row("sr0", 2) }, "sr0", toc);
    CHECK(m2.toc_indices.size() == 1 && m2.toc_indices[0] == 1, "sr0 spec resolves");
    CHECK(m2.unresolved == 0, "sr0 row is not unresolved");
}

// ── 4. the drop is counted, never silent (addition B / the HTOA slot) ────────
static void test_drop_is_counted() {
    const auto toc = relishNumbers();
    // A real file path, a stray string, and a track off the end of the disc.
    auto m = toTocIndices({ "D:\\Music\\Best CD Tracks\\01.flac", "", row("G", 99) },
                          "G", toc);
    CHECK(m.toc_indices.empty(), "none of those name a track on this disc");
    CHECK(m.unresolved == 3, "all three must be counted, got %d", m.unresolved);
}

// ── 5. TOC order and dedup, whatever order the user marked in ────────────────
static void test_order_and_dedup() {
    const auto toc = relishNumbers();
    auto m = toTocIndices({ row("G", 9), row("G", 3), row("G", 7), row("G", 3) },
                          "G", toc);
    CHECK(m.toc_indices.size() == 3, "duplicate track 3 collapses, got %zu",
          m.toc_indices.size());
    CHECK(m.toc_indices[0] == 2 && m.toc_indices[1] == 6 && m.toc_indices[2] == 8,
          "result must be in TOC order");
}

// ── 6. empty means ALL, and so does everything (addition A) ──────────────────
static void test_whole_disc_predicate() {
    const auto toc = relishNumbers();

    auto none = toTocIndices({}, "G", toc);
    CHECK(none.toc_indices.empty(), "no marks yields no indices");
    CHECK(isWholeDiscSelection(none, 12),
          "EMPTY MEANS ALL - unmarking the last track rips the whole disc, not nothing");

    std::vector<std::string> every;
    for (int i = 1; i <= 12; ++i) every.push_back(row("G", i));
    auto all = toTocIndices(every, "G", toc);
    CHECK(all.toc_indices.size() == 12, "every track resolves");
    CHECK(isWholeDiscSelection(all, 12),
          "marking all twelve is also the whole disc, so no partial summary line");

    auto some = toTocIndices({ row("G", 3), row("G", 7) }, "G", toc);
    CHECK(!isWholeDiscSelection(some, 12), "two of twelve is partial");
}

int main() {
    test_ordinary_disc();
    test_number_is_not_index();
    test_drive_is_identity();
    test_drop_is_counted();
    test_order_and_dedup();
    test_whole_disc_predicate();
    std::printf("cd_track_select_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
