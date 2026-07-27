#pragma once

// ─── The list panes' cursor/scroll relationship: ONE function, both panes ─────
//
// A list pane is a cursor plus a scroll offset, and they have to agree: the row
// the cursor is on must be inside the window the pane is drawing. Nothing else
// about them is pane-specific, which is why this is one function and not two.
//
// The playlist pane has enforced that agreement since slice 5, as a DRAW-TIME
// INVARIANT: drawPlaylist() reconciles cursor and scroll once at the top, and
// every path that moves the cursor simply moves it and lets the next draw reveal
// it. That is why jumpToPlaylistIndex is a single assignment.
//
// The browser pane did not. It had a partial twin - the minimal-scroll step and
// nothing else - called from four handlers and never from the draw. So every path
// that moved dir_cursor_ without remembering to call it left the cursor off-pane,
// and the four [Library] populate functions did something sharper than forget:
// they restored the cursor BY NAME and then set dir_scroll_ = 0 on the next line
// but one, pinning the view to the top of a list whose cursor was 300 rows down.
// Return to a section and the pane did not show what the cursor was on.
//
// Two panes clamping their own scroll is the same defect class BrowserPins.h was
// written to end: two implementations of one rule, free to disagree. So the rule
// lives here once, both panes call it, and it is enforced where it cannot be
// forgotten - at the top of each pane's draw.
//
// PURE and dependency-free: no curses, no filesystem, no UIManager. The pane
// passes in its own numbers and gets its own numbers back, so the behaviour is
// asserted by a unit test rather than by reading the code and hoping.

#include <algorithm>

namespace panescroll {

// Reconcile a list pane's cursor and scroll offset.
//
// Clamps `cursor` into [0, n) and then scrolls the MINIMUM needed to bring it
// inside a window of `visible` rows starting at `scroll` - never re-centres, so a
// cursor that is already on screen does not move the view at all. That is the
// landing policy for both panes: minimal movement, because it is what the playlist
// has always done and what j/k/PgUp/PgDn/Home/End in the browser have always done.
//
// IDEMPOTENT. Calling it twice changes nothing the first call did not, which is
// what lets a draw-time call coexist with the per-handler calls that predate it
// instead of fighting them.
//
// `visible <= 0` means the pane is not built yet (no window, or a terminal too
// small to have content rows): the cursor is still clamped into range, but the
// scroll is left alone rather than computed against a meaningless height.
//
// The three steps are ordered, and each one exists because something needs it:
//   1. cursor into range   - a repopulate can shrink the list under a live cursor
//   2. minimal scroll      - the actual scroll-to-cursor
//   3. scroll off the tail - a shrinking list can strand the view past the end,
//                            which draws a blank pane rather than a wrong row
inline void ensureVisible(int& cursor, int& scroll, int n, int visible) {
    if (n <= 0) { cursor = 0; scroll = 0; return; }

    // 1. The cursor is a row index and must name a row.
    if (cursor < 0)  cursor = 0;
    if (cursor >= n) cursor = n - 1;

    if (visible <= 0) return;          // pane not built yet: scroll means nothing

    // 2. Scroll the least that puts the cursor on screen.
    if (cursor < scroll)                 scroll = cursor;
    else if (cursor >= scroll + visible) scroll = cursor - visible + 1;

    // 3. Never leave the view past the end of the list. Order matters: this runs
    //    after step 2 so a short list (n < visible) ends at scroll 0 rather than
    //    at a negative offset.
    if (scroll > n - visible) scroll = n - visible;
    if (scroll < 0)           scroll = 0;
}

}   // namespace panescroll
