// htoa_span_test - is there a hidden track, and how far does it run.
//
// Detection is TOC-only, so the entire question is arithmetic over one number:
// track 1's read address. That makes it provable before a drive ever spins, and
// it makes the one thing that must never happen cheap to state.
//
// THE FORBIDDEN CHANGE: Relish must never grow a hidden track. Its track 1
// starts at LBA 32 - an ordinary mechanical pregap of 0.427 s - and it is the
// regression disc every CD-path slice is measured against. If a threshold edit
// ever lets 32 qualify, block 2 fails, and "a disc without HTOA behaves exactly
// as it did" stops being provable. That is why the boundary is tested from both
// sides at the exact sector rather than sampled loosely.
//
// Pure: links nothing.
#include "HtoaSpan.h"

#include <cstdio>

static int g_checks = 0, g_fail = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

using namespace htoa;

// ── 1. The two real discs ────────────────────────────────────────────────────
static void test_the_discs_we_have() {
    // Factory Showroom: track 1 at LBA 4575, measured off the drive. 61.000 s.
    CHECK(present(4575), "Factory Showroom should offer HTOA");
    const auto fs = span(4575);
    CHECK(fs.has_value(), "Factory Showroom span should exist");
    if (fs) {
        CHECK(fs->start_lba == 0, "span must start at 0, got %u", fs->start_lba);
        CHECK(fs->sectors == 4575, "span should be 4575 sectors, got %u", fs->sectors);
        CHECK(fs->lastLba() == 4574, "last sector should be 4574, got %u", fs->lastLba());
        CHECK(fs->seconds() > 60.99 && fs->seconds() < 61.01,
              "span should be 61.000 s, got %.3f", fs->seconds());
    }

    // Relish: track 1 at LBA 32. The regression disc. Must never qualify.
    CHECK(!present(32), "RELISH MUST NOT OFFER HTOA");
    CHECK(!span(32).has_value(), "RELISH MUST NOT PRODUCE A SPAN");
}

// ── 2. The threshold, from both sides of the exact sector ────────────────────
static void test_threshold_is_strictly_greater() {
    CHECK(kMinPregapSectors == 375, "threshold should be 375, got %u", kMinPregapSectors);

    CHECK(!present(374), "374 is below the threshold");
    CHECK(!present(375), "375 must NOT qualify - the comparison is strictly greater");
    CHECK( present(376), "376 is above the threshold");

    CHECK(!span(375).has_value(), "375 must produce no span");
    CHECK( span(376).has_value(), "376 must produce a span");

    // A disc with track 1 at LBA 0 has no pregap at all. The Factory Showroom
    // XE pressing is exactly this: same album, same track count, no hidden
    // track, track 1 at frame 150 = LBA 0.
    CHECK(!present(0), "a disc with no pregap must not offer HTOA");
    CHECK(!span(0).has_value(), "LBA 0 must produce no span");
}

// ── 3. The span is the whole pregap, for any qualifying length ───────────────
static void test_span_covers_the_whole_pregap() {
    for (uint32_t lba : { 376u, 1000u, 4575u, 20000u }) {
        const auto s = span(lba);
        CHECK(s.has_value(), "lba %u should produce a span", lba);
        if (!s) continue;
        CHECK(s->start_lba == 0, "span must start at 0 for lba %u", lba);
        CHECK(s->sectors == lba, "span for lba %u should be %u sectors, got %u",
              lba, lba, s->sectors);
        // The span stops one sector short of track 1 - it must never include
        // track 1's own first sector, which belongs to track 1.
        CHECK(s->lastLba() == lba - 1,
              "span for lba %u must end at %u, got %u", lba, lba - 1, s->lastLba());
    }
}

// ── 4. present() and span() cannot disagree ──────────────────────────────────
// Two entry points answering one question is the shape that drifts. Anywhere
// they disagree, one caller offers a row the other refuses to extract.
static void test_present_and_span_agree() {
    for (uint32_t lba = 0; lba < 800; ++lba)
        CHECK(present(lba) == span(lba).has_value(),
              "present(%u) and span(%u) disagree", lba, lba);
}

int main() {
    test_the_discs_we_have();
    test_threshold_is_strictly_greater();
    test_span_covers_the_whole_pregap();
    test_present_and_span_agree();

    std::printf("htoa_span_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
