// pane_scroll_test - the cursor/scroll rule both list panes now share.
//
// This exists because the rule used to live in two places that were free to
// disagree, and did. The playlist pane reconciled cursor and scroll once at the
// top of its draw; the browser pane had a partial copy - the minimal-scroll step
// and nothing else - called from four handlers and never from the draw. Every
// path that moved dir_cursor_ without remembering to call it left the cursor
// off-pane, and the [Library] populate functions restored the cursor BY NAME and
// then pinned the view to row 0, so returning to a section showed a pane that was
// not displaying the row the cursor was on.
//
// The rule is one pure function now, and this test is what keeps it one function
// AND keeps its policy from drifting: minimal movement, idempotent, three clamps.
//
// Pure and device-free - no curses, no filesystem. Both matrix jobs.

#include "PaneScroll.h"

#include <cstdio>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

using panescroll::ensureVisible;

// Is `cur` inside the window [scr, scr+vis)? The property the whole function
// exists to establish, written once so every test can assert it directly.
static bool onScreen(int cur, int scr, int vis, int n) {
    if (n <= 0) return cur == 0 && scr == 0;
    if (vis <= 0) return true;              // pane not built: nothing to be inside of
    return cur >= scr && cur < scr + vis;
}

// ── The reported defect, as an assertion ─────────────────────────────────────
//
// A 500-row library level, a 40-row pane, the cursor restored by name onto row
// 300, and the scroll sitting at 0 because the populate function had just reset
// it. Before slice 9 this drew rows 0-39 and the cursor was nowhere on screen.
static void test_the_hardware_bug() {
    int cur = 300, scr = 0;
    ensureVisible(cur, scr, 500, 40);
    CHECK(cur == 300, "the cursor must not move: got %d", cur);
    CHECK(scr == 261, "scroll to 261 (300 - 40 + 1): got %d", scr);
    CHECK(onScreen(cur, scr, 40, 500), "cursor %d not inside [%d,%d)", cur, scr, scr + 40);

    // The same shape ascending: cursor above the window rather than below it.
    cur = 12; scr = 261;
    ensureVisible(cur, scr, 500, 40);
    CHECK(scr == 12, "scrolling up lands the cursor on the FIRST row: got %d", scr);
    CHECK(onScreen(cur, scr, 40, 500), "cursor %d not inside [%d,%d)", cur, scr, scr + 40);
}

// ── Minimal movement: the landing policy, pinned ─────────────────────────────
//
// This is the test that fails if anyone later "improves" the helper to centre the
// cursor. Centring was considered and rejected in the slice 9 design note: it
// would change scroll behaviour on every j/k the user already has muscle memory
// for, and the playlist has never done it.
static void test_minimal_movement() {
    // Already visible: the view must not move AT ALL.
    for (int c = 100; c < 140; ++c) {
        int cur = c, scr = 100;
        ensureVisible(cur, scr, 500, 40);
        CHECK(scr == 100, "visible cursor %d moved the scroll to %d", c, scr);
        CHECK(cur == c, "visible cursor %d was altered to %d", c, cur);
    }
    // Exactly one row below the window: scroll by exactly one, not a page, not a
    // re-centre.
    { int cur = 140, scr = 100; ensureVisible(cur, scr, 500, 40);
      CHECK(scr == 101, "one row below should scroll by one: got %d", scr); }
    // Exactly one row above: same, upward.
    { int cur = 99, scr = 100; ensureVisible(cur, scr, 500, 40);
      CHECK(scr == 99, "one row above should scroll by one: got %d", scr); }
    // A cursor at the very top and bottom of the list.
    { int cur = 0, scr = 300; ensureVisible(cur, scr, 500, 40);
      CHECK(scr == 0, "cursor 0 must land at scroll 0: got %d", scr); }
    { int cur = 499, scr = 0; ensureVisible(cur, scr, 500, 40);
      CHECK(scr == 460, "last row: scroll 460: got %d", scr); }
}

// ── Idempotence ──────────────────────────────────────────────────────────────
//
// What lets the draw-time call coexist with the four per-handler calls that
// predate it: calling it twice changes nothing the first call did not.
static void test_idempotent() {
    const int ns[]   = { 0, 1, 2, 5, 40, 41, 500 };
    const int viss[] = { -3, 0, 1, 2, 39, 40, 41, 600 };
    const int curs[] = { -7, -1, 0, 1, 20, 260, 499, 500, 900 };
    const int scrs[] = { -5, 0, 1, 39, 260, 461, 499, 900 };
    for (int n : ns) for (int vis : viss) for (int c0 : curs) for (int s0 : scrs) {
        int c1 = c0, s1 = s0;
        ensureVisible(c1, s1, n, vis);
        int c2 = c1, s2 = s1;
        ensureVisible(c2, s2, n, vis);
        CHECK(c1 == c2 && s1 == s2,
              "not idempotent at n=%d vis=%d cur=%d scr=%d: (%d,%d) then (%d,%d)",
              n, vis, c0, s0, c1, s1, c2, s2);
    }
}

// ── The property, over the whole grid ────────────────────────────────────────
//
// After one call, for any starting state: the cursor names a real row, the scroll
// is a legal offset, and the cursor is on screen.
static void test_invariant_holds_everywhere() {
    const int ns[]   = { 1, 2, 5, 40, 41, 500 };
    const int viss[] = { 1, 2, 39, 40, 41, 600 };
    const int curs[] = { -7, -1, 0, 1, 20, 260, 499, 500, 900 };
    const int scrs[] = { -5, 0, 1, 39, 260, 461, 499, 900 };
    for (int n : ns) for (int vis : viss) for (int c0 : curs) for (int s0 : scrs) {
        int c = c0, s = s0;
        ensureVisible(c, s, n, vis);
        CHECK(c >= 0 && c < n, "cursor %d out of [0,%d) from cur=%d n=%d", c, n, c0, n);
        CHECK(s >= 0, "negative scroll %d from scr=%d n=%d vis=%d", s, s0, n, vis);
        CHECK(s == 0 || s <= n - vis,
              "scroll %d past the tail (n=%d vis=%d, max %d)", s, n, vis, n - vis);
        CHECK(onScreen(c, s, vis, n),
              "cursor %d not inside [%d,%d) at n=%d vis=%d", c, s, s + vis, n, vis);
    }
}

// ── Step 1: the cursor into range ────────────────────────────────────────────
//
// A repopulate can shrink the list under a live cursor - a rescan that drops an
// artist, a delete that removes a row. The old browser helper did not do this.
static void test_cursor_clamped_into_range() {
    { int cur = 900, scr = 0; ensureVisible(cur, scr, 500, 40);
      CHECK(cur == 499, "cursor past the end clamps to n-1: got %d", cur); }
    { int cur = -4, scr = 0; ensureVisible(cur, scr, 500, 40);
      CHECK(cur == 0, "negative cursor clamps to 0: got %d", cur); }
    // n == 0 zeroes both - an empty list has no row to be on and no view to hold.
    { int cur = 17, scr = 9; ensureVisible(cur, scr, 0, 40);
      CHECK(cur == 0 && scr == 0, "empty list: got cur=%d scr=%d", cur, scr); }
    // The old browser expression was min(saved, n-1), which yields -1 when the list
    // is empty. Unreachable in the product (refreshDir always pushes the pins), but
    // it is unreachable BY CONSTRUCTION here rather than by luck.
    { int cur = -1, scr = 0; ensureVisible(cur, scr, 0, 40);
      CHECK(cur == 0, "a -1 cursor never survives: got %d", cur); }
}

// ── Step 3: the scroll off the tail ──────────────────────────────────────────
//
// A shrinking list can strand the view past the end, which draws a BLANK pane
// rather than a wrong row. Four delete paths in the browser could do this
// ([FAVs], [Radio] twice, [Books]) and the dir-poll refresh clamped the scroll to
// n-1 instead of n-vis, which is the same stranding with a smaller bound.
static void test_scroll_clamped_off_the_tail() {
    { int cur = 5, scr = 460; ensureVisible(cur, scr, 20, 40);
      CHECK(scr == 0, "list shorter than the pane: scroll 0, got %d", scr); }
    { int cur = 99, scr = 400; ensureVisible(cur, scr, 100, 40);
      CHECK(scr == 60, "scroll clamps to n-vis=60: got %d", scr); }
    // The n-1 bound the dir poll used: with n=100 and a 40-row pane it would allow
    // a scroll of 99 and draw one row plus 39 blanks.
    { int cur = 50, scr = 99; ensureVisible(cur, scr, 100, 40);
      CHECK(scr == 50, "an n-1 scroll is pulled back to cover the cursor: got %d", scr);
      CHECK(scr <= 100 - 40, "scroll %d still past the tail", scr); }
}

// ── Pane not built yet ───────────────────────────────────────────────────────
//
// visible <= 0 means no window, or a terminal too small to have content rows.
// paneVisibleRows returns r-1 (Classic) or r-2 (Awesome), so a short terminal can
// genuinely produce 0 or a negative. Clamp the cursor; leave the scroll alone
// rather than compute it against a meaningless height.
static void test_pane_not_built() {
    { int cur = 900, scr = 77; ensureVisible(cur, scr, 500, 0);
      CHECK(cur == 499, "cursor still clamped with no pane: got %d", cur);
      CHECK(scr == 77, "scroll untouched with no pane: got %d", scr); }
    { int cur = 10, scr = 5; ensureVisible(cur, scr, 500, -2);
      CHECK(scr == 5, "negative visible leaves the scroll alone: got %d", scr); }
}

// ── Boundaries ───────────────────────────────────────────────────────────────
static void test_boundaries() {
    // A one-row pane: the scroll tracks the cursor exactly.
    for (int c = 0; c < 5; ++c) {
        int cur = c, scr = 0;
        ensureVisible(cur, scr, 5, 1);
        CHECK(scr == c, "vis=1: scroll should equal the cursor, got %d for %d", scr, c);
    }
    // Pane exactly the list height, and taller than it: always scroll 0.
    { int cur = 4, scr = 3; ensureVisible(cur, scr, 5, 5);
      CHECK(scr == 0, "vis==n: scroll 0, got %d", scr); }
    { int cur = 4, scr = 3; ensureVisible(cur, scr, 5, 60);
      CHECK(scr == 0, "vis>n: scroll 0, got %d", scr); }
    // A single-row list.
    { int cur = 9, scr = 9; ensureVisible(cur, scr, 1, 40);
      CHECK(cur == 0 && scr == 0, "n==1: got cur=%d scr=%d", cur, scr); }
}

// ── The playlist's contract, unchanged ───────────────────────────────────────
//
// The playlist pane is shipped, hardware-gated behaviour and slice 9 must not
// alter it. Its three steps are now THIS function, so these are the shapes the old
// ensurePlaylistCursorVisible produced, asserted against the shared one. A mistake
// in the wrapper shows up here rather than on Dos's screen.
static void test_playlist_contract_unchanged() {
    // An empty playlist zeroes both, which is what the old body's early return did.
    { int cur = 5, scr = 5; ensureVisible(cur, scr, 0, 30);
      CHECK(cur == 0 && scr == 0, "empty playlist: got cur=%d scr=%d", cur, scr); }
    // Ordinary downward walk off the bottom edge (the j-at-the-bottom case).
    { int cur = 30, scr = 0; ensureVisible(cur, scr, 200, 30);
      CHECK(scr == 1, "one past the bottom scrolls by one: got %d", scr); }
    // Move-up/move-down and the F3 follow-sync land the cursor anywhere; the view
    // must follow minimally from wherever it was.
    { int cur = 150, scr = 0;  ensureVisible(cur, scr, 200, 30);
      CHECK(scr == 121, "follow-sync jump: got %d", scr); }
    { int cur = 150, scr = 140; ensureVisible(cur, scr, 200, 30);
      CHECK(scr == 140, "already visible: view must not move, got %d", scr); }
    // A playlist shrinking under the cursor (removing rows) - both clamps at once.
    { int cur = 199, scr = 170; ensureVisible(cur, scr, 12, 30);
      CHECK(cur == 11 && scr == 0, "shrunk to 12: got cur=%d scr=%d", cur, scr); }
}

int main() {
    test_the_hardware_bug();
    test_minimal_movement();
    test_idempotent();
    test_invariant_holds_everywhere();
    test_cursor_clamped_into_range();
    test_scroll_clamped_off_the_tail();
    test_pane_not_built();
    test_boundaries();
    test_playlist_contract_unchanged();

    std::printf("pane_scroll_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
