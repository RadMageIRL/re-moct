// browser_pins_test - the browser's pinned-row order, asserted rather than read.
//
// This exists because the order used to live in two hand-maintained lists that
// had to agree: refreshDir() pushed the rows, and the sort comparator pinned
// them. [Library] was added to the pushes and not to the comparator, fell through
// to the file/directory comparison, sorted as an ordinary file, and rendered at
// the BOTTOM of the pane. It failed a hardware gate, and the insert had been
// correct the whole time.
//
// So the order is one list now, and this test is what makes it stay one list: it
// pins the exact rendered sequence, proves the comparator is a strict weak
// ordering, and proves a full sort reproduces the sequence.
//
// Pure and device-free - no curses, no filesystem. Both matrix jobs.

#include "BrowserPins.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

using namespace browserpins;

// The rendered order, written out independently of kPins so that reordering the
// header without meaning to fails here rather than on Dos's screen.
static const char* kExpected[] = {
    "[Drives]", "[Recent]", "[FAVs]", "[Radio]", "[Podcasts]", "[Books]", "[Library]", ".."
};
static constexpr std::size_t kExpectedCount = sizeof(kExpected) / sizeof(kExpected[0]);

static void test_order_is_exactly_this() {
    CHECK(kCount == kExpectedCount, "pin count %zu, expected %zu", kCount, kExpectedCount);
    for (std::size_t i = 0; i < kExpectedCount && i < kCount; ++i)
        CHECK(std::string(kPins[i]) == kExpected[i],
              "row %zu is [%s], expected [%s]", i, kPins[i], kExpected[i]);

    // Every section that exists has a pin. A section pushed but not pinned is the
    // exact defect this replaces.
    const char* sections[] = {"[Drives]", "[Recent]", "[FAVs]", "[Radio]",
                              "[Podcasts]", "[Books]", "[Library]"};
    for (const char* s : sections) CHECK(isPin(s), "%s is pinned", s);
    CHECK(isPin(".."), ".. is pinned");
}

static void test_rank() {
    for (std::size_t i = 0; i < kCount; ++i)
        CHECK(rank(kPins[i]) == (int)i, "rank(%s)=%d", kPins[i], rank(kPins[i]));

    CHECK(rank("") == -1,                "empty is not a pin");
    CHECK(rank("Charli xcx") == -1,      "an ordinary directory is not a pin");
    CHECK(rank("[Bookmarks]") == -1,     "the bookmarks popup row is not a browser pin");
    CHECK(rank("[Back]") == -1,          "[Back] belongs to the sections, not the browser");
    CHECK(rank("[library]") == -1,       "matching is exact, not case-folded");
    CHECK(rank("[Library] ") == -1,      "no trailing-space tolerance");
    CHECK(rank("..") == (int)kCount - 1, ".. sorts last of the pins");
}

// ── The comparator contract ────────────────────────────────────────────────
static void test_before() {
    bool d = false;

    CHECK(before("[Drives]", "[Library]", d) && d, "earlier pin first");
    d = false;
    CHECK(!before("[Library]", "[Drives]", d) && d, "and not the other way");
    d = false;
    CHECK(before("[Library]", "..", d) && d, "sections precede ..");
    d = false;
    CHECK(before("[Books]", "Zebra Directory", d) && d, "a pin precedes an ordinary row");
    d = false;
    CHECK(!before("Zebra Directory", "[Books]", d) && d, "an ordinary row follows a pin");

    // Neither pinned: the comparator must DECLINE so the browser sort decides.
    d = true;
    CHECK(!before("Aardvark", "Zebra", d), "two ordinary rows: no pinned answer");
    CHECK(!d, "and it reports undecided so the sort mode takes over");
}

// ── Strict weak ordering: the latent UB this slice removes ─────────────────
static void test_strict_weak_ordering() {
    // comp(a, a) MUST be false. The old open-coded chain returned true here,
    // which is undefined behaviour in std::sort.
    for (std::size_t i = 0; i < kCount; ++i) {
        bool d = false;
        CHECK(!before(kPins[i], kPins[i], d), "comp(%s, %s) is false", kPins[i], kPins[i]);
    }
    // Irreflexive and asymmetric across every pair, pinned and not.
    std::vector<std::string> all(kPins, kPins + kCount);
    all.push_back("Ordinary A");
    all.push_back("Ordinary B");
    for (const auto& a : all) {
        for (const auto& b : all) {
            bool da = false, db = false;
            const bool ab = before(a, b, da);
            const bool ba = before(b, a, db);
            if (a == b) CHECK(!ab, "irreflexive for [%s]", a.c_str());
            else if (da && db) CHECK(!(ab && ba), "asymmetric for [%s] vs [%s]", a.c_str(), b.c_str());
        }
    }
}

// ── A real sort reproduces the rendered sequence ───────────────────────────
static void test_full_sort_reproduces_order() {
    // Deliberately scrambled, with ordinary rows mixed through - the shape a
    // directory listing actually arrives in before sorting.
    std::vector<std::string> rows = {
        "zzz last dir", "[Library]", "Charli xcx - BRAT (2024)", "..", "[Books]",
        "aaa first dir", "[Drives]", "[Podcasts]", "[Recent]", "[Radio]", "[FAVs]"
    };
    std::stable_sort(rows.begin(), rows.end(),
        [](const std::string& a, const std::string& b) {
            bool decided = false;
            const bool pinned = before(a, b, decided);
            if (decided) return pinned;
            return a < b;                       // stand-in for BrowserSort::Name
        });

    for (std::size_t i = 0; i < kExpectedCount; ++i)
        CHECK(rows[i] == kExpected[i], "sorted row %zu is [%s], expected [%s]",
              i, rows[i].c_str(), kExpected[i]);
    CHECK(rows[kExpectedCount + 0] == "Charli xcx - BRAT (2024)", "contents follow the pins");
    CHECK(rows[kExpectedCount + 1] == "aaa first dir", "and sort among themselves");
    CHECK(rows.back() == "zzz last dir", "last row");

    // At a filesystem root ".." is never pushed; the rest must still be in order.
    std::vector<std::string> at_root;
    for (std::size_t i = 0; i < kCount; ++i)
        if (std::string(kPins[i]) != "..") at_root.push_back(kPins[i]);
    std::vector<std::string> scrambled(at_root.rbegin(), at_root.rend());
    std::stable_sort(scrambled.begin(), scrambled.end(),
        [](const std::string& a, const std::string& b) {
            bool decided = false;
            const bool pinned = before(a, b, decided);
            return decided ? pinned : a < b;
        });
    CHECK(scrambled == at_root, "root listing keeps the order with .. absent");
}

// ── The [Library] toggle (slice 6) ───────────────────────────────────────────
// "Off" means the row is ABSENT, not present-and-inert, and that has to hold for
// EVERY reader or the section half-exists. Before slice 6 there were three lists of
// these names and enterDriveList's own read none of this header; it now does, so this
// one predicate is what the toggle turns.
static void test_library_toggle() {
    CHECK(shown("[Library]", true),  "toggle on -> [Library] is shown");
    CHECK(!shown("[Library]", false), "toggle off -> [Library] is absent");

    // Nothing else may be affected by it.
    for (std::size_t i = 0; i < kCount; ++i) {
        const std::string nm = kPins[i];
        if (nm == "[Library]") continue;
        CHECK(shown(nm, false), "toggle off leaves %s alone", nm.c_str());
    }
    // [Bookmarks] is not a pin at all - it exists only in the [Drives] list - so it is
    // unaffected either way and must not be ranked.
    CHECK(rank("[Bookmarks]") == -1, "[Bookmarks] is not a pinned row");
    CHECK(shown("[Bookmarks]", false), "and the toggle does not touch it");

    // A hidden section must rank as -1 so it sorts as ordinary content rather than
    // being pinned invisibly.
    CHECK(shownRank("[Library]", false) == -1, "hidden -> not pinned");
    CHECK(shownRank("[Library]", true) == rank("[Library]"), "shown -> its normal rank");

    // And the comparator honours it: with the toggle off, a real file must not lose to
    // a row that is not being drawn.
    bool decided = false;
    CHECK(!before("[Library]", "song.flac", decided, false) || !decided,
          "toggle off: [Library] does not outrank ordinary content");
    decided = false;
    CHECK(before("[Library]", "song.flac", decided, true) && decided,
          "toggle on: it does");

    // Sorting the full list with the toggle off reproduces the order minus that row.
    std::vector<std::string> want;
    for (std::size_t i = 0; i < kCount; ++i)
        if (std::string(kPins[i]) != "[Library]") want.push_back(kPins[i]);
    std::vector<std::string> got(want.rbegin(), want.rend());
    std::stable_sort(got.begin(), got.end(),
        [](const std::string& a, const std::string& b) {
            bool d = false;
            const bool pinned = before(a, b, d, false);
            return d ? pinned : a < b;
        });
    CHECK(got == want, "sort with the toggle off keeps the remaining order");
}

int main() {
    test_order_is_exactly_this();
    test_rank();
    test_before();
    test_strict_weak_ordering();
    test_full_sort_reproduces_order();
    test_library_toggle();

    std::printf("browser_pins_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
