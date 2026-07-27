// rip_selection_test - CD-S1: which tracks to rip, and which track each one IS.
//
// The entire risk of CD-S1 is "which index means what", and that is pure
// arithmetic over a TOC size and a selection - so it is provable with no drive,
// no disc, no network, before Relish ever spins.
//
// TWO THINGS THIS FILE EXISTS TO PROVE:
//
//   1. THE DEFAULT PATH IS UNCHANGED. planAll(n) must reproduce exactly the
//      arguments the worker passes today - toc_index == i, is_first == (i==0),
//      is_last == (i==n-1). Substituting that into the call sites is the
//      byte-identity argument, and block 1 is that argument in executable form.
//
//   2. is_first / is_last FOLLOW THE DISC, NOT THE PLAN. They choose the
//      AccurateRip CRC skip window: AR_SKIP samples are excluded at the very
//      start of the disc's first track and the very end of its last. Computed
//      from loop position - as the shipped worker did - ripping track 5 alone
//      would set is_first and cut AR_SKIP samples out of track 5's checksum, a
//      number that can never match AccurateRip and that no log line explains.
//      Block 4 is that case, and it fails against any implementation that uses
//      plan position.
//
// Pure: links nothing.
#include "RipSelection.h"

#include <cstdio>
#include <vector>

static int g_checks = 0, g_fail = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

using namespace ripsel;

// ── 1. planAll reproduces TODAY'S arguments, for every disc size that matters ─
static void test_plan_all_is_todays_behaviour() {
    for (int n : { 1, 2, 3, 12, 99 }) {          // 12 = Relish; 99 = Red Book max
        const auto p = planAll(n);
        CHECK((int)p.size() == n, "planAll(%d) gave %zu items", n, p.size());
        for (int i = 0; i < (int)p.size(); ++i) {
            CHECK(p[(size_t)i].toc_index == i,
                  "planAll(%d)[%d].toc_index == %d, wanted %d",
                  n, i, p[(size_t)i].toc_index, i);
            CHECK(p[(size_t)i].is_first == (i == 0),
                  "planAll(%d)[%d].is_first must equal the old (i==0)", n, i);
            CHECK(p[(size_t)i].is_last == (i == n - 1),
                  "planAll(%d)[%d].is_last must equal the old (i==total-1)", n, i);
        }
        CHECK(isWholeDisc(n, p), "planAll(%d) is the whole disc", n);
    }
}

// ── 2. the default path IS the general path ──────────────────────────────────
static void test_full_selection_equals_plan_all() {
    for (int n : { 1, 2, 12, 99 }) {
        std::vector<int> all;
        for (int i = 0; i < n; ++i) all.push_back(i);
        CHECK(plan(n, all) == planAll(n),
              "plan(%d, {0..%d}) must equal planAll(%d)", n, n - 1, n);
    }
}

// ── 3. order independence - the rip_sel_ contract, one level over ────────────
static void test_order_independence() {
    const auto a = plan(12, { 9, 3, 7 });
    const auto b = plan(12, { 3, 7, 9 });
    const auto c = plan(12, { 7, 9, 3 });
    CHECK(a == b && b == c, "toggle order must not change the plan");
    CHECK(a.size() == 3, "three selected, three planned");
    CHECK(a[0].toc_index == 3 && a[1].toc_index == 7 && a[2].toc_index == 9,
          "the plan is in TOC order");
}

// ── 4. *** is_first / is_last FOLLOW THE DISC *** ────────────────────────────
//
// The block that catches finding (5). A middle track selected alone is neither
// the disc's first nor its last, however early it sits in the plan.
static void test_disc_edges_are_not_plan_edges() {
    const auto one = plan(12, { 5 });
    CHECK(one.size() == 1, "one track selected");
    CHECK(one[0].toc_index == 5, "and it is track index 5");
    CHECK(!one[0].is_first,
          "*** track 5 alone is NOT the disc's first track - is_first here cuts "
          "AR_SKIP samples out of its checksum ***");
    CHECK(!one[0].is_last,
          "*** track 5 alone is NOT the disc's last track ***");

    // The same at the head of a multi-item plan: item 0 of the plan is track 3.
    const auto mid = plan(12, { 3, 7, 9 });
    CHECK(!mid[0].is_first, "the plan's first item is not the disc's first track");
    CHECK(!mid[2].is_last,  "the plan's last item is not the disc's last track");
}

// ── 5. the real edges, when they ARE selected ────────────────────────────────
static void test_real_edges_when_selected() {
    const auto first = plan(12, { 0 });
    CHECK(first[0].is_first && !first[0].is_last, "track 1 alone is first, not last");

    const auto last = plan(12, { 11 });
    CHECK(!last[0].is_first && last[0].is_last, "track 12 alone is last, not first");

    const auto both = plan(1, { 0 });
    CHECK(both[0].is_first && both[0].is_last,
          "on a one-track disc the only track is both");

    // Ends selected, middle not: each keeps its own disc identity.
    const auto ends = plan(12, { 0, 11 });
    CHECK(ends.size() == 2, "two selected");
    CHECK(ends[0].is_first && !ends[0].is_last, "track 1 keeps is_first only");
    CHECK(!ends[1].is_first && ends[1].is_last, "track 12 keeps is_last only");
}

// ── 6. defensive - the caller is a UI in a later slice ───────────────────────
static void test_defensive() {
    const auto oob = plan(12, { -1, 3, 12, 400 });
    CHECK(oob.size() == 1 && oob[0].toc_index == 3,
          "out-of-range indices are dropped, not clamped");

    const auto dup = plan(12, { 4, 4, 4 });
    CHECK(dup.size() == 1, "duplicates collapse - a track cannot be ripped twice");

    CHECK(plan(12, {}).empty(), "an empty selection plans nothing");
    CHECK(plan(0, { 0 }).empty(), "a zero-track disc plans nothing");
    CHECK(plan(-1, { 0 }).empty(), "a negative disc size plans nothing");
    CHECK(planAll(0).empty(), "planAll(0) is empty");

    CHECK(!isWholeDisc(12, plan(12, { 1, 2 })), "a subset is not the whole disc");
    CHECK(!isWholeDisc(0, {}), "a zero-track disc is not a whole disc");
}

int main() {
    test_plan_all_is_todays_behaviour();
    test_full_selection_equals_plan_all();
    test_order_independence();
    test_disc_edges_are_not_plan_edges();
    test_real_edges_when_selected();
    test_defensive();

    std::printf("rip_selection_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
