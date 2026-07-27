// browser_sections_test - LIB-S17: the virtual-section flag set, proved headless.
//
// The bug this slice fixes was not in an algorithm. It was four hand-written
// copies of one list disagreeing, and the worst copy - the main loop's directory
// poll - testing one flag out of ten and so calling refreshDir() from inside a
// live section. So what is provable here is what the two operations over that set
// do, and in particular that NEITHER of them can miss a member.
//
// WHAT THIS FILE CANNOT PROVE, stated rather than implied: that UIManager's
// section_flags_ array actually lists all ten members. That array is the single
// enumeration and it lives beside the members it names; a test cannot reach it
// without linking UIManager, which drags curses. The structural guarantee is that
// there is now ONE place to forget instead of four - not that forgetting is
// impossible. The same is true of BrowserPins.h, for the same reason.
//
// Pure: no curses, no filesystem, no UIManager. Links nothing.
#include "BrowserSections.h"

#include <cstdio>
#include <cstddef>

static int g_checks = 0, g_fail = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

// The same arity UIManager uses, so the table below exercises a set of the real
// size rather than a convenient one.
static constexpr std::size_t N = 10;

struct Set {
    bool  f[N] {};
    bool* p[N] {};
    Set() { for (std::size_t i = 0; i < N; ++i) p[i] = &f[i]; }
};

// EVERY member counts. This is the block that would have caught the poll's guard:
// `!in_drive_list_` is anySet over a set of ONE, and nine flags could be set
// without it noticing.
static void test_every_flag_counts() {
    for (std::size_t i = 0; i < N; ++i) {
        Set s;
        CHECK(!browsersec::anySet(s.p, N), "a fresh set must read as no section");
        s.f[i] = true;
        CHECK(browsersec::anySet(s.p, N),
              "flag %zu alone must read as a section - a guard that misses it "
              "lets current_dir_ work run inside a section", i);
    }
}

static void test_none_set_is_false() {
    Set s;
    CHECK(!browsersec::anySet(s.p, N), "nothing set is not a section");
    // A sub-mode set alongside its parent is still one answer, not two.
    s.f[5] = true;   // podcasts
    s.f[6] = true;   // podcast_feed
    CHECK(browsersec::anySet(s.p, N), "a section and its sub-mode still read as a section");
}

// clearAll must clear EVERY member, which is the other half of the same list.
// The two operations pin each other: whatever anySet can see, clearAll must clear.
static void test_clear_all_clears_everything() {
    Set s;
    for (std::size_t i = 0; i < N; ++i) s.f[i] = true;
    CHECK(browsersec::anySet(s.p, N), "all set reads as a section");

    browsersec::clearAll(s.p, N);
    CHECK(!browsersec::anySet(s.p, N), "*** clearAll must leave NO flag set ***");
    for (std::size_t i = 0; i < N; ++i)
        CHECK(!s.f[i], "flag %zu survived clearAll - the reset trap leaks", i);
}

// The round trip the two reset sites actually perform: clear everything, then set
// one flag true. enterDriveList does exactly this, and it must not disturb the
// other nine.
static void test_clear_then_set_one() {
    Set s;
    for (std::size_t i = 0; i < N; ++i) s.f[i] = true;
    browsersec::clearAll(s.p, N);
    s.f[0] = true;                                  // in_drive_list_ = true
    CHECK(browsersec::anySet(s.p, N), "the drive list is a section");
    for (std::size_t i = 1; i < N; ++i)
        CHECK(!s.f[i], "entering the drive list left flag %zu set", i);
}

// Defensive: the helpers take raw pointers, and a null entry must not crash. Not
// reachable from UIManager - the array is filled in the constructor - but these
// are header-inline functions anything may call, and this file is their contract.
static void test_null_entries_are_survivable() {
    bool  a = false;
    bool* p[3] = { &a, nullptr, nullptr };
    CHECK(!browsersec::anySet(p, 3), "nulls do not read as set");
    a = true;
    CHECK(browsersec::anySet(p, 3), "a real flag past a null is still seen");
    browsersec::clearAll(p, 3);
    CHECK(!a, "clearAll skipped the nulls and cleared the real one");

    CHECK(!browsersec::anySet(p, 0), "an empty set is not a section");
    browsersec::clearAll(p, 0);                     // must not touch anything
}

int main() {
    test_every_flag_counts();
    test_none_set_is_false();
    test_clear_all_clears_everything();
    test_clear_then_set_one();
    test_null_entries_are_survivable();

    std::printf("browser_sections_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
