# DESIGN NOTE - Library slice 9: browse pane scroll-to-cursor

**Scope ID:** LIB-S9. **Status:** proposed, awaiting sign-off. Nothing implemented.
**Probed against tip `fed8658`** on 2026-07-26. Every line number below was read, not recalled.

Predecessors LIB-S1 to LIB-S8 and LIB-AA shipped, pushed, CI-green, hardware-gated.

**Renumbering, recorded so nothing floats:** this note takes the number LIB-S9. Genre, stat
views, the remaining scale measurements and the `libnav::searchRowLabel` compilation fallback -
designed in `docs/library-slice8-plan.md` §2-§5 with Dos's rulings pinned - become **LIB-S10**.
That design is unaffected and is not re-derived here.

---

## 0. THE PREMISE, VERIFIED - AND IT IS HALF RIGHT

The brief flags this as Dos's recollection and says verify before building. Verified. The
recollection is right about the playlist and **wrong in one detail about the browser**, and the
detail changes what gets built.

### The playlist fix exists, and it is a draw-time invariant

`UIManager::ensurePlaylistCursorVisible()` - `src/UIManager.cpp:3150-3159`. It does three
things:

```cpp
if (n == 0) { pl_cursor_ = 0; pl_scroll_ = 0; return; }
pl_cursor_ = std::clamp(pl_cursor_, 0, n - 1);                        // 1. cursor into range
const int visible = win_playlist_ ? paneVisibleRows(win_playlist_) : 0;
if (visible <= 0) return;                                             //    pane not built yet
if (pl_cursor_ < pl_scroll_)                 pl_scroll_ = pl_cursor_; // 2. minimal scroll
else if (pl_cursor_ >= pl_scroll_ + visible) pl_scroll_ = pl_cursor_ - visible + 1;
pl_scroll_ = std::clamp(pl_scroll_, 0, std::max(0, n - visible));     // 3. scroll off the tail
```

**It has exactly one call site: `src/UIManager.cpp:3235`, the top of `drawPlaylist()`.** That is
the whole mechanism. Every path that moves `pl_cursor_` just moves it and lets the next draw
heal the view - `jumpToPlaylistIndex` (`:3747`) says so in its comment and deliberately never
touches `pl_scroll_`.

### A browser twin ALSO already exists - this is the correction

`UIManager::ensureDirCursorVisible()` - `src/UIManager.cpp:3772-3777`. So "the browse pane never
was [fixed]" is not quite the state of the tree; a partial helper is already there, added
alongside `jumpToBrowserIndex` and its own comment says what it is:

```cpp
// The browser has no draw-time cursor-visible invariant (j/k nudge per-handler), so
// several sites clamp dir_scroll_ by hand after a cursor move; this is that clamp,
// factored. NOT idempotent scroll math like ensurePlaylistCursorVisible - it only
// pulls the view to cover dir_cursor_ (never re-centres), matching the prior inline.
void UIManager::ensureDirCursorVisible() {
    int vis = paneVisibleRows(win_dir_);
    if (dir_cursor_ < dir_scroll_) dir_scroll_ = dir_cursor_;
    else if (vis > 0 && dir_cursor_ >= dir_scroll_ + vis)
        dir_scroll_ = dir_cursor_ - vis + 1;
}
```

**It is step 2 only.** It has no cursor-into-range clamp (step 1), no scroll-off-the-tail clamp
(step 3), and no null-window guard - `paneVisibleRows(win_dir_)` dereferences through `getmaxyx`
with no `win_dir_ ?` test, where the playlist twin has one. And critically:

**It is called from four handlers, never from `drawDirBrowser()`.** `drawDirBrowser()`
(`:2897`) goes from `werase` straight to `int idx = dir_scroll_ + i;` at `:2985` with nothing in
between. **There is no invariant.** The four call sites are `jumpToBrowserIndex` (`:3762`),
the `[Drives]` F12 refresh (`:7353`), `navigatePage` (`:8799`) and `navigateHomeEnd` (`:8816`).

**So the defect is real, and it is sharper than "the fix is missing": the fix is present,
incomplete, and wired the way the playlist's own comment warns against** - "do NOT reintroduce
per-handler `if cursor < scroll then nudge` math" (`include/UIManager.h:289-290`). The browser is
that per-handler math, and the paths nobody remembered to add it to are the bug.

---

## 1. THE REPORTED DEFECT, REPRODUCED STATICALLY

The hardware report is "cursor scrolled near the bottom, navigate away and back, the pane does
not show what the cursor is on." Here it is, in `showLibraryArtists()`, `:9841-9844`:

```cpp
const int row = libidx::restoreCursor(lib_nav_.sel_artist, rows);
dir_cursor_ = (row < 0) ? 0 : row + 1;                // +1 for the [Back] row
if (dir_cursor_ >= (int)dir_entries_.size()) dir_cursor_ = 0;
dir_scroll_ = 0;                                      // <-- the view is forced to the top
```

**LIB-S3's restore-by-name is defeated by the next line but one.** The cursor is faithfully
re-seated on the artist the user left - row 300 of 500 - and then the view is pinned to row 0,
with no invariant to reconcile them. `drawDirBrowser` renders rows 0..visible and the cursor is
nowhere on screen.

**Identical, character for character, at all four library levels:** `showLibraryArtists`
`:9844`, `showLibraryAlbums` `:9882`, `showLibraryTracks` `:9932`, `showLibrarySearch` `:10090`.

**And it is not a library bug**, exactly as the brief says. `[Podcasts]` has the same shape at
`:7772-7773` and `:8162-8163`:

```cpp
int keep = dir_cursor_;
config_.removePodcastFeed(nm);
showPodcastFeedList();                       // sets dir_cursor_ = 0; dir_scroll_ = 0;
dir_cursor_ = std::clamp(keep, 0, ...);      // cursor restored, scroll left at 0
```

Delete a feed from row 40 of a long subscription list and the cursor is off-pane. Library only
made it *routine*, because it is the first section that reliably produces hundreds of rows.

---

## 2. EVERY PATH - ENUMERATED, NOT SAMPLED

The brief asks for all of them, on the grounds that a path nobody decided about is a path nobody
looked at. Every `dir_cursor_`/`dir_scroll_` mutation in `src/UIManager.cpp`, classified:

| # | Path | Site | Today | Off-pane? |
|---|---|---|---|---|
| 1 | `showLibraryArtists` restore | `:9841-9844` | cursor by name, `scroll = 0` | **YES - the report** |
| 2 | `showLibraryAlbums` restore | `:9879-9882` | same | **YES** |
| 3 | `showLibraryTracks` restore | `:9929-9932` | same | **YES** |
| 4 | `showLibrarySearch` restore | `:10087-10090` | same | **YES** |
| 5 | `[Podcasts]` feed delete (Del) | `:7769-7773` | cursor kept, scroll 0 | **YES** |
| 6 | `[Podcasts]` feed delete (`d`) | `:8159-8163` | same | **YES** |
| 7 | Resize (shrink) | `resizeWindows` `:476` | touches neither | **YES** |
| 8 | Dir-poll refresh on mtime change | `:1724-1729` | restores both, wrong clamps | **YES** (see §6.1) |
| 9 | `[FAVs]` delete | `:7746-7747` | cursor down, scroll untouched | scroll can strand (§6.2) |
| 10 | `[Radio]` delete (Del) | `:7761-7762` | same | scroll can strand |
| 11 | `[Radio]` delete (`d`) | `:8150-8151` | same | scroll can strand |
| 12 | `[Books]` delete | `:7793-7794` | same | scroll can strand |
| 13 | `jumpToBrowserIndex` (`\` search) | `:3759-3762` | calls the helper | no |
| 14 | `[Drives]` F12 refresh | `:7346-7353` | calls the helper | no |
| 15 | `navigatePage` | `:8791-8799` | calls the helper | no |
| 16 | `navigateHomeEnd` | `:8811-8816` | calls the helper | no |
| 17 | `navigateDown` / `navigateUp` | `:8768-8786` | inline nudge, correct | no |
| 18 | ~20 sites setting `cursor = 0; scroll = 0` | `refreshDir`, section enters, empty states | consistent | no |

**Seven live off-pane paths, four of them the reported one, plus resize. Four more where the
scroll can strand past the end of a shrunken list.** The F12 library rescan is covered: it routes
through `populateLevel()` (`:10030`) into rows 1-4, so it is the same bug, not a separate one.
Ascend and descend between library levels likewise - `libraryAscend` (`:10101`) and
`sectionAscend` (`:10133`) both repopulate through those four functions.

**Note what the table says about scope.** Rows 5-12 are `[Podcasts]`, `[FAVs]`, `[Radio]`,
`[Books]` and the folder browser. A fix scoped to `[Library]` would leave all of them.

---

## 3. THE SHARED HELPER - LIFT, DO NOT DUPLICATE

The brief's one non-negotiable. The playlist's math **is** general - it reads `pl_cursor_`,
`pl_scroll_`, `n` and `visible` and nothing else. There is no entanglement with playlist state to
report; it is liftable as-is.

**Proposal: `include/PaneScroll.h`, pure and header-inline**, the `BrowserPins.h` /
`LibraryNav.h` / `LibraryIndex.h` precedent - no curses, no filesystem, so it links into a test
with nothing else, per the repo norm.

```cpp
namespace panescroll {

// Clamp `cursor` into [0, n) and scroll the MINIMUM needed to bring it inside a
// window of `visible` rows starting at `scroll`. Idempotent: calling it twice
// changes nothing the first call did not. visible <= 0 means the pane is not
// built yet - the cursor is still clamped, the scroll is left alone.
inline void ensureVisible(int& cursor, int& scroll, int n, int visible) {
    if (n <= 0) { cursor = 0; scroll = 0; return; }
    if (cursor < 0)  cursor = 0;
    if (cursor >= n) cursor = n - 1;
    if (visible <= 0) return;
    if (cursor < scroll)                 scroll = cursor;
    else if (cursor >= scroll + visible) scroll = cursor - visible + 1;
    if (scroll > n - visible) scroll = n - visible;
    if (scroll < 0)           scroll = 0;
}

}   // namespace panescroll
```

Both existing members become thin wrappers, so **all 20 existing call sites keep working
unchanged** and the playlist's behaviour is byte-identical - same three steps, same order, same
minimal-scroll rule:

```cpp
void UIManager::ensurePlaylistCursorVisible() {
    panescroll::ensureVisible(pl_cursor_, pl_scroll_, (int)playlist_.size(),
                              win_playlist_ ? paneVisibleRows(win_playlist_) : 0);
}
void UIManager::ensureDirCursorVisible() {
    panescroll::ensureVisible(dir_cursor_, dir_scroll_, (int)dir_entries_.size(),
                              win_dir_ ? paneVisibleRows(win_dir_) : 0);
}
```

**Then the actual fix is one line** - `ensureDirCursorVisible()` at the top of
`drawDirBrowser()`, mirroring `:3235`:

```cpp
void UIManager::drawDirBrowser() {
    werase(win_dir_);
    ensureDirCursorVisible();   // the invariant: every offscreen-cursor path self-heals here
```

**That single call closes all seven live paths in §2 at once, resize included**, because the draw
runs after everything - after a repopulate, after a delete, after `resizeWindows`, after a poll
refresh. It is the same reason the playlist needs no per-path handling. Resize therefore needs
**no separate handling**, which the brief asked me to state either way.

**The four surviving per-handler calls (rows 13-16) stay.** They become redundant but not wrong -
the helper is idempotent, so a call before the draw and a call during it produce the same state.
Removing them is a real simplification and I am **not** folding it into this slice: it edits four
working, hardware-gated paths for no user-visible gain, which is the "additive only" constraint.
Recorded as **LIB-S12 (cleanup)**, not deferred silently - S11 is multi-root library, which is
designed and waiting on its brief after S10.

---

## 4. WHERE THE CURSOR LANDS: MINIMAL MOVEMENT

Proposed: **minimally-scrolled-into-view**, not top, not centred. Three reasons.

1. **It is what the playlist already does**, and the brief requires the playlist not regress.
   One policy in one function is the whole point of lifting it; two panes with different landing
   rules would be the two-lists defect in a new costume.
2. **It is what the browser already does** for `j`/`k`/PgUp/PgDn/Home/End. Centring would change
   scroll behaviour on every keypress a user has muscle memory for - the opposite of additive.
3. **Centring needs a policy nobody has asked for** - when to centre versus when to nudge - and
   scroll margins are an explicit non-goal.

**The honest consequence, stated rather than discovered on hardware:** with `dir_scroll_ = 0`
left in the four library populates, a by-name restore to row 300 lands the cursor on the pane's
**last visible row**. It is visible, which is the defect fixed - but it is pinned to the bottom
edge, which is not the view the user left. That leads to the one behaviour change worth ruling
on, in §5.

---

## 5. THE ONE THING THAT CHANGES EXISTING BEHAVIOUR - DOS'S CALL

The brief says to raise it if the fix alters scroll behaviour a user relies on. This is that.

**Proposal: delete the literal `dir_scroll_ = 0;` from the four library restore paths** (`:9844`,
`:9882`, `:9932`, `:10090`) - the ones that restore a cursor by name. Leave every
`cursor = 0; scroll = 0;` reset elsewhere alone, including the empty/status-row early returns in
those same functions.

**Why it is safe, and why it is better:**

- **Safe by construction.** Once the invariant runs at draw time, *any* value of `dir_scroll_` is
  valid input: step 3 clamps it to `[0, n - visible]` against the new list, and step 2 pulls it
  to cover the cursor. A stale scroll from the artist list cannot survive into the album list as
  anything wrong. If the restored cursor is 0 - the common descend case, no remembered row - the
  invariant drives scroll to 0 anyway, which is exactly today's behaviour.
- **Better, because return-to-section keeps the view.** The user's scroll position is preserved
  and the cursor is already inside it, so returning shows the pane they left rather than a pane
  scrolled to put their selection on the bottom edge.

**What changes for a user:** returning to a library level no longer snaps the view to the top of
the list. Given that snapping to the top is precisely what hides the cursor, I do not believe
anyone relies on it - but it is a shipped behaviour, so it is Dos's to rule, and the slice works
either way.

**If Dos prefers to keep `dir_scroll_ = 0`:** drop this section, ship §3 alone. The reported
defect is still fixed; the cursor just lands on the last visible row instead of where it was.
Everything else in this note is unchanged.

---

## 6. ADJACENT FINDINGS - NUMBERED, NOT FOLDED IN SILENTLY

Three things the probe turned up that the brief did not ask about. None is a licence to widen
the slice; each gets a disposition.

### 6.1 The dir-poll refresh clamps scroll to the wrong bound

`:1728-1729`, the every-N-ticks "directory changed on disk" path:

```cpp
dir_cursor_ = std::min(saved_cursor, (int)dir_entries_.size() - 1);
dir_scroll_ = std::min(saved_scroll, std::max(0, (int)dir_entries_.size() - 1));
```

The scroll bound is `n - 1`, not `n - visible`, so a directory shrinking under the browser can
leave the scroll far enough down that the pane draws mostly blank. **Fixed for free by §3** - the
draw-time invariant re-clamps both. No separate edit proposed; the two lines can stay as a
harmless pre-clamp, and I would rather not touch a working poll path in a rendering slice.

**Also, `std::min(saved_cursor, n - 1)` yields `-1` when `dir_entries_` is empty.** I checked
reachability rather than reporting it as a bug: `refreshDir()` (`:9405`) pushes
`browserpins::kPins` before anything else, so the list is never empty on this path and the `-1`
is **latent and currently unreachable**. §3's step 1 (`if (cursor < 0) cursor = 0`) makes it
unreachable by construction instead of by luck. Recorded, not claimed as a live defect.

### 6.2 Four delete paths can strand the scroll past the end

Rows 9-12 in §2 - `[FAVs]`, `[Radio]` x2, `[Books]`. They pull the cursor back into range after
removing an entry but never touch `dir_scroll_`, so deleting enough rows leaves the view below
the new end of the list. **Also fixed for free by §3**, step 3. No separate edit.

### 6.3 `ensureDirCursorVisible` has no null-window guard

`paneVisibleRows(win_dir_)` at `:3773` calls `getmaxyx` on `win_dir_` with no test, where
`ensurePlaylistCursorVisible` guards with `win_playlist_ ? ... : 0`. Not currently reachable -
all four callers run from input handlers, after windows exist - but adding a draw-time call and
keeping the wrapper honest costs one ternary. **Folded into §3's wrapper**, since I am rewriting
that function body anyway; it is a strictly-safer rewrite of a line I am already touching.

---

## 7. TESTS

The lift is what makes this testable at all - the math leaves `UIManager` and becomes pure.

**New `tests/pane_scroll_test.cpp`** (links nothing, like `browser_pins_test`):

- **Idempotence** - `ensureVisible` twice changes nothing the first call did not, across a grid
  of (cursor, scroll, n, visible).
- **The reported case** - `n=500, visible=40, cursor=300, scroll=0` scrolls to `261` and the
  cursor is inside `[scroll, scroll+visible)`. This is the hardware bug as an assertion.
- **Minimal movement** - a cursor already visible moves the scroll by **zero**; a cursor one row
  below the window moves it by exactly one. This is the assertion that the policy in §4 is what
  shipped, and it is what would fail if anyone later "improved" it to centre.
- **Scroll off the tail** - `n` shrinking under a large scroll clamps to `n - visible`, and never
  below 0 when `n < visible`.
- **Cursor into range** - negative and `>= n` both clamp; `n == 0` zeroes both.
- **Pane not built** - `visible <= 0` clamps the cursor and leaves the scroll untouched.
- **Boundaries** - `visible == 1`, `visible == n`, `visible > n`, `cursor == n-1`, `cursor == 0`.

**`tests/library_level_test.cpp`:** nothing to add. The levels are unchanged; what changed is
when the scroll is reconciled, which is a rendering property.

**Playlist non-regression, per the brief's "additive rather than assumed":** the same
`pane_scroll_test` grid **is** the playlist's contract, since it now runs the same function. On
top of that, an assertion that `ensureVisible` reproduces the shipped three-step sequence exactly
on the playlist's own shapes, so a wrapper mistake shows up as a test failure rather than on
hardware.

**Mutation-testing, per the standing rule:** I will break the helper three ways - invert the
`cursor < scroll` comparison, drop the tail clamp, make it non-idempotent - and confirm each
fails, **verifying the mutation landed in the file before trusting either outcome** (the LIB-S8
false-pass lesson).

**Not machine-testable, and therefore gate:** everything rendered. This is a rendering defect;
most of it is Dos's to confirm.

---

## 8. GATE - EYES-ON, BOTH PLATFORMS

1. **The reported case:** `[Library]`, a long artist list, cursor scrolled near the bottom,
   descend into an album and back out - **the cursor is visible.** Then the same ascending from
   tracks to albums to artists.
2. Same in a **large plain directory** (a few hundred files), scrolled near the bottom, leave to
   a section and return.
3. Same in **`[FAVs]`, `[Recent]`, `[Books]`**, and in **podcast episodes** on a long feed.
4. **A 500-row `|` search result**, cursor near the bottom, `[Back]` and re-enter.
5. **F12 library rescan** with the cursor deep in a long list - cursor still visible after the
   repopulate. **`Esc` cancel** likewise.
6. **Remembered-cursor restore:** leave `[Library]` entirely, re-enter - the remembered artist is
   selected *and* on screen.
7. **Terminal resized small** with the cursor near the bottom, both Classic and Awesome (the pane
   height differs by one - `paneVisibleRows` returns `r-2` vs `r-1`). Then resized back.
8. **Delete paths:** remove a podcast feed from row 40 of a long list; remove a `[FAVs]`,
   `[Radio]` and `[Books]` entry from near the bottom. Cursor visible, pane not blank.
9. **THE PLAYLIST PANE BEHAVES EXACTLY AS TODAY** - `j`/`k`, PgUp/PgDn, Home/End, F3 follow-sync,
   `\` search jump, move-up/move-down. This is the regression check that matters most.
10. Every section still enters, draws, exits and plays. `Ctrl+T` still Classic/Awesome only.

Machine: ctest both toolchains, `--no-tests=error`, currently **46/46 Windows, 47/47 Linux**;
expected 47/48 with `pane_scroll_test` added. Brace-balance and scoped-diff audit before handoff.
**Verification split as in LIB-S4 through LIB-S8** - no rendered behaviour asserted unless run or
the producing mechanism can be pointed at.

---

## 9. FILES EXPECTED TO CHANGE

| file | change |
|---|---|
| `include/PaneScroll.h` | **new** - the pure helper |
| `include/UIManager.h` | comments at `:214-216` and `:286-292` (both now describe a shared invariant) |
| `src/UIManager.cpp` | 2 wrapper bodies, 1 call in `drawDirBrowser`, and §5's 4 deleted lines if ruled |
| `tests/pane_scroll_test.cpp` | **new** |
| `tests/CMakeLists.txt` | register it |
| `CHANGELOG.md` | one Fixed line under `[1.5.0]`, user-facing, hyphens only |
| `docs/library-slice9-plan.md` | this note, as design-of-record |

**Not touched:** `LibraryIndex.h`, `LibraryNav.h`, `LibraryScanner`, `PlaylistManager`,
`Config`, `Version.h`, the audio thread, `ar_crc`, the CD path, the rip path, the plugin ABI.
No index format change. No new dependency. Roughly **35 lines of new code**, most of it the
helper, plus its test.

Also uncommitted and unrelated: `docs/ROADMAP-library-view.md` carries LIB-S8's status update
from the previous session. It rides this slice's commit or Dos's word, whichever comes first,
and its table needs the S9/S10 renumbering from the top of this note.
