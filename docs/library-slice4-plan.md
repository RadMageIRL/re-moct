# DESIGN NOTE - Library slice 4: levels 2-3 (albums, tracks)

**Scope ID:** LIB-S4. **Status:** proposed, awaiting sign-off. Nothing implemented.
**Probed against tip `30b8d40`** on 2026-07-26. Every anchor below was read in the live
tree, not quoted from a prior document.

Predecessors LIB-S1 (`c6287ee`), LIB-S2 (`5152b46`), LIB-S3 + 3b (`30b8d40`) are shipped,
pushed, and eyes-on. Design of record: `docs/library-index-plan.md`,
`docs/ROADMAP-library-view.md`.

---

## 0. FINDINGS FIRST - three divergences from the brief

Per the disconfirmation clause. None of these blocks the slice; two change what it does.

### F1 - The `pseudoRank(name)` consolidation has ALREADY LANDED, under another name

The brief sequences slice 4 "behind the `pseudoRank(name)` consolidation slice, which lands
first". There is no `pseudoRank` in the tree and there is no pending slice. The
consolidation shipped **as part of `30b8d40`** (slice 3b) as `include/BrowserPins.h`:
`browserpins::kPins`, `browserpins::rank()`, `browserpins::isPin()`,
`browserpins::before()`.

Both readers are converted, verified by call site:
- the push, `src/UIManager.cpp:9241`, loops `browserpins::kCount` over `kPins`
- the sort comparator, `src/UIManager.cpp:9281`, calls `browserpins::before(ea, eb, decided)`

There is no duplicated comparator left to build on. **Slice 4 is unblocked and starts
now.** The latent strict-weak-ordering violation the brief's predecessor was meant to fix
(`comp(a, a) == true`, undefined behaviour in `std::sort`) is fixed by the rank form and
asserted by a unit test.

### F2 - `populateLevel()` does not exist; slice 4 CREATES the dispatch

The brief asks how "`populateLevel()` extends, and how the descriptor carries per-level
header text, populate, enter, and back behaviour", which reads as though it were shipped.
It was not. What slice 3 shipped is:

| shipped | `include/UIManager.h` |
|---|---|
| `enum class LibLevel { Artists, Albums, Tracks }` | :648 |
| `LibLevel lib_level_ = LibLevel::Artists` | :650 |
| `std::string lib_artist_` - "selection that led to the current level (slice 4)" | :651 |
| `std::string lib_album_` - ditto | :652 |
| `void showLibraryArtists()` - populates level 1 | :779 |

So the enum and the path-taken members are exactly as the hedge promised, and the three
values are already named. The **dispatch** is what slice 4 adds. This is a smaller job than
retrofitting one, and it is why §1 below proposes the function rather than an edit to it.

### F3 - Left is NOT wired for `[Library]` at all, and the shipped drill-down section contradicts the brief

The brief states "`[Back]` and Left ascend one level" as though Left already ascends. In
the live tree, the browser's `KEY_LEFT` handler (`src/UIManager.cpp:8111-8132`) has
**exactly one section branch, `in_drive_list_`** (added this campaign for the `[Drives]`
back row). There is no `in_library_` branch, and there is no `in_podcast_feed_` branch
either.

What Left therefore does inside any virtual section today is fall through to:

```
fs::path p(current_dir_);
... current_dir_ = parent.string(); dir_cursor_ = 0; dir_scroll_ = 0; refreshDir();
```

and `refreshDir()` clears every section flag (`:9085`). Net shipped behaviour: **Left
leaves the section entirely AND silently moves the directory browser up one level** from
wherever `current_dir_` happened to be.

The consequence that matters: **`[Podcasts]`, the only shipped two-level section and the
pattern slice 4 is told to follow, does not ascend on Left.** At level 2 (episodes), Left
exits the whole section. `[Back]` is the only one-level-up. So "Left ascends one level" is
not an existing idiom being extended - it is new behaviour, and adopting it makes library
the only section where Left means something different.

**This needs a ruling and is Q1 in §10.** It is a UX question, not a code question, and
the brief says PM does not specify design - but it also asserted the wiring exists, so the
choice was hidden rather than made.

---

## 1. The level descriptor and `populateLevel()`

One function, one switch, no new section flags. `in_library_` stays the single section
flag; `lib_level_` carries depth.

```
void UIManager::populateLevel();      // dispatch on lib_level_
void UIManager::showLibraryArtists(); // shipped; becomes the Artists case
void UIManager::showLibraryAlbums();  // new; Albums case
void UIManager::showLibraryTracks();  // new; Tracks case
```

`populateLevel()` is a three-way switch and nothing else. Every caller that today calls
`showLibraryArtists()` to mean "relist the section" becomes `populateLevel()`; the three
sites are `pollLibraryScan()` (`:9517`, `:9534`) and `enterLibrarySection()` (`:9461`).
`enterLibrarySection()` keeps calling the level-1 path directly at its other four returns,
because entering the section is definitionally a return to level 1.

**Why a switch and not a table of function pointers or a descriptor struct.** The brief
offers "how the descriptor carries per-level header text, populate, enter, and back
behaviour" as a design space. I propose the switch and recommend against the descriptor.
Three levels, four behaviours, all of them two to six lines, and the header text already
lives in a different function (`drawDirBrowser`, `:2917`) that cannot read a descriptor
without being handed one. A table would put four fields in one place at the cost of
indirection at every use, and the enum switch gives the same compile-time
exhaustiveness. If slice 7 adds genre as a fourth entry point the switch grows one case.

**Header text** extends the shipped nested-if idiom that `[Podcasts]` uses at `:2911-2915`,
because that is where the function already branches:

```
else if (in_library_) {
    switch (lib_level_) {
      case LibLevel::Artists: hdr = " [Library] (Enter:albums  [Back]:leave) ";
      case LibLevel::Albums:  hdr = " [Library] <artist> (Enter:tracks  [Back]:artists) ";
      case LibLevel::Tracks:  hdr = " [Library] <artist> - <album> ([Back]:albums) ";
    }
}
```

Artist and album names in the header are `sanitizeForDisplay`d and **elided to fit** rather
than allowed to push the hint text off the pane; the header is a fixed-width row and a
long album title would otherwise hide the only stated way out. Level 3's hint carries no
`Enter:` verb in slice 4 because Enter is a placeholder here (§4).

## 2. Identity and display per level

The lockstep idiom, unchanged: `dir_entries_` holds identity, `dir_display_` holds what is
drawn, pushed one for one, `[Back]` first at index 0.

| level | `dir_entries_` (identity) | `dir_display_` |
|---|---|---|
| 1 Artists | raw grouping-artist string, from a tag | `sanitizeForDisplay`, empty renders `(no artist)` |
| 2 Albums | raw album string, from a tag | `sanitizeForDisplay`, empty renders `(no album)` |
| 3 Tracks | **the absolute path**, `LibraryTrack::path` | `NN. Title  (M:SS)` |

**Track identity is the absolute path, confirmed not assumed.** `LibraryTrack::path` is
documented as "absolute; IDENTITY - must round-trip byte-exact"
(`include/LibraryIndex.h:70`), it is the field the index is keyed on for revalidation, and
it is OS-origin - produced by the scanner's directory walk, never assembled from tag text.
That is precisely the class the corrected CP1252 rule calls safe.

**Level 3 display fields**, all present on `LibraryTrack`: `track_no` (:76), `title` (:74),
`duration_sec` (:79). Proposed format `NN. Title  (M:SS)` via the existing
`UIManager::formatTime(double)` (`include/UIManager.h:954`) so a library duration and a
playlist duration read identically. `track_no == 0` (untagged rip) omits the number and
its separator rather than printing `0.`; a zero `duration_sec` omits the parenthetical.
Title empty falls back to the **filename stem of the path** - `path` is safe to take a stem
from, unlike anything at levels 1-2.

## 3. Staleness at three levels - the rule holds, stated per level

LIB-S3's rule: **the UI holds identity strings and never indices**, and a query result is
consumed inside the one populate call that asked for it and never survives a frame. How it
holds at depth:

- **Level 1.** `libidx::artists()` returns strings. Consumed in `showLibraryArtists`.
- **Level 2.** `libidx::albumsForArtist(idx, lib_artist_)` returns strings. `lib_artist_` is
  itself a string, so the query is re-runnable from held state after any rebuild.
- **Level 3.** `libidx::tracksForAlbum(idx, lib_artist_, lib_album_)` returns
  **`std::vector<std::size_t>` - indices** (`include/LibraryIndex.h:423`), and its own
  comment flags the lifetime hazard. **Those indices must not survive `showLibraryTracks`.**
  The loop reads `library_index_.tracks[i].path` and pushes the path string, then the vector
  dies at scope exit. Nothing else in the codebase ever sees an index.

The re-query key at every level is a pair of strings the UI already holds, which is why a
scan completing underneath a live level-3 listing needs no invalidation: `pollLibraryScan`
calls `populateLevel()`, which re-runs the query against the new index from `lib_artist_`
and `lib_album_`. **A generation counter remains unnecessary at depth.**

## 4. Enter, `[Back]`, and descend/ascend

Extends the shipped `in_library_` branch of `activateSelection` (`:8800`).

| level | Enter on a content row | `[Back]` |
|---|---|---|
| 1 | set `lib_artist_ = entry`, level := Albums, `populateLevel()` | leave section (shipped) |
| 2 | set `lib_album_ = entry`, level := Tracks, `populateLevel()` | clear `lib_album_`, level := Artists, `populateLevel()` |
| 3 | **placeholder toast** | clear `lib_album_`, level := Albums, `populateLevel()` |

**Enter on a track row does nothing in slice 4** beyond a toast, matching how slice 3 left
Enter on an artist. Playback and queue are slice 5, where Enter **appends to the playlist**
per Dos's ruling. The toast should say so honestly rather than "not implemented".

Ascending clears the member for the level being left, so `lib_album_` is never stale while
`lib_level_ == Albums`. Descending sets it before the populate call, never after.

The status and empty rows that levels 1-3 share push `""` into `dir_entries_`, which
`browserEntryPath` and the `[Back]` comparison both already treat as non-selectable
(`:9343`). Enter on one is a no-op at every level by construction.

## 5. Cursor behaviour across descend and ascend

`libidx::restoreCursor(remembered, rows)` is pure, tested, and matches case-insensitively,
returning `0` for a non-empty list when nothing matches and `-1` only for an empty one
(`include/LibraryIndex.h:387`). Slice 3 holds one remembered name, `lib_selected_`.

**Proposal: one remembered name per level, three members** - `lib_selected_` (artist,
shipped), plus `lib_sel_album_` and `lib_sel_track_`. Descend remembers the row descended
from; ascend restores it.

This is the only place I propose new members, and the reason is that a single shared
`lib_selected_` cannot do it: descending from artist "Muse" to album "Absolution" would
overwrite the artist memory, so ascending would land on row 0 instead of back on Muse.
Three strings is the smallest state that makes ascend land where the user left.

Level 3's remembered value is a **path**, so its match is exact-by-identity rather than by
display text; `restoreCursor`'s case-insensitive compare is harmless on a path and no
second function is needed. Every restored row is offset `+1` for `[Back]`, with the
shipped bounds check (`:9501-9502`) repeated per level.

`lib_sel_album_` clears when `lib_artist_` changes and `lib_sel_track_` clears when
`lib_album_` changes - otherwise descending into a *different* album would try to restore a
track from the previous one, which at best lands on row 0 and at worst looks like a bug.
Both reset sites clear all three (§7).

## 6. Sort order, and `O`

**Library levels do not go through the browser sort, at any level.** `showLibraryArtists`
populates `dir_entries_`/`dir_display_` directly and returns (`:9475-9504`); the sort at
`:9281-9287` runs inside `refreshDir()`, which library populates never call. Slice 4's two
new populates follow that exactly.

Order is therefore the index's, which is already total and already correct:
- levels 1-2: `detail::sortUniqueCI` - case-insensitive, deduplicated
- level 3: `tracksForAlbum` sorts **disc, then track number, then title, then path**
  (`include/LibraryIndex.h:436-440`). The last two tie-breakers exist so untagged rips,
  where every `track_no` is 0, still get a stable total order from the filename.

**`O` (cycle browser sort, `:7710`) calls `refreshDir()`**, which clears `in_library_` and
returns the pane to the directory browser. So pressing `O` inside `[Library]` today exits
the section as a side effect. That is the same shipped behaviour every other virtual section
has, it is not new to slice 4, and I propose **leaving it alone**: name/modified/size are
directory concepts and none of the three is meaningful over an artist list. It goes in the
gate list as a stated expectation, not a fix. If Dos wants `O` inert inside sections, that
is its own slice across all seven, not a library special case (Q3).

## 7. The reset trap - already paid, verified

Both sites already clear all three level members, shipped in slice 3:

- `refreshDir()` `:9085-9087` - `in_library_ = false; lib_level_ = Artists;
  lib_artist_.clear(); lib_album_.clear();`
- `enterDriveList()` `:9224-9226` - identical

**Slice 4 adds the two new remembered-cursor members to both sites** and nothing else. That
is the entire delta, because slice 3 anticipated the trap rather than leaving it. The gate
exercises refresh at all three levels deliberately (§9) because "it is already handled" is
what was believed the last two times it bit.

## 8. The hazard sites, re-decided one at a time

The brief requires each `!in_library_` site to be re-decided for track rows rather than
assumed. All 20 `in_library_` mentions, classified. **The load-bearing decision is the last
row.**

| site | what it is | slice 4 decision |
|---|---|---|
| `:2917` | header text | **extend** - switch on `lib_level_` (§1) |
| `:3002` | draw loop, `fs::is_directory(current_dir_ / name)` | **stay excluded, all levels.** No library row is a directory at any level, so exclusion is now correct on semantics rather than only on safety - and it avoids an `fs` stat per visible row per frame |
| `:3742` | pane label "Library" | unchanged |
| `:7501` | `q` add-to-queue | **stay excluded** - queue is slice 5 |
| `:7620` | fav toggle | **stay excluded** - marking is slice 5 |
| `:7719` | `b` books toggle | **stay excluded** - slice 5 |
| `:7737` | bookmark `current_dir_` | **stay excluded** - operates on `current_dir_`, not the row; bookmarking from inside a virtual section is meaningless, matching radio and podcasts |
| `:7855` | `x` convert `convert_src_dir_` | **stay excluded** - converting is slice 5 |
| `:8089` | `a` add-to-playlist | **stay excluded** - slice 5 |
| `:8800` | `activateSelection` library branch | **extend** - the descend/ascend logic (§4) |
| `:9085`, `:9224` | reset sites | **extend** - two new members (§7) |
| `:9347` | **`browserEntryPath` returns `{}` for library** | **stay excluded at ALL levels, including Tracks** |

**Why `browserEntryPath` stays excluded even though a track row's identity is a genuine
absolute path.** It is the single seam that makes a row reachable: its five callers are the
draw-loop glyph (`:3022`) and four key handlers (`:7781`, `:7821`, `:7847`, `:7940`).
Returning a path for `LibLevel::Tracks` would silently enable playback, chapters, and three
other operations across all five in one edit - which is slice 5's scope, arriving without a
gate. Keeping `{}` in slice 4 makes slice 5 **one line** and gives it a real gate.

This is the answer to the brief's warning in both directions: the sites are not
blanket-enabled because identity became a path, and the exclusions are not carried over
unexamined either - each is excluded for a reason stated above, and the reason for eight of
them is "this belongs to slice 5", which is a scope boundary rather than a safety claim.

**One thing slice 5 must cost when it flips that line:** `browserEntryPath` is called **in
the draw loop** at `:3022`, per visible row per frame. It builds an `fs::path` and its
callers stat. Level 3 returning a real path puts that cost in the frame. Flagged here so
slice 5 budgets it rather than discovering it on a gate.

**Levels 1-2 remain hard-excluded on safety, not scope.** An artist or album identity is tag
text and may be invalid UTF-8; `fs::path()` and even `fs::exists(s, ec)` throw on it, `ec`
included, because the conversion runs before `ec` applies. Nothing may construct a path from
those strings at any point, in any slice.

## 9. Tests

**Machine-testable, and worth it:** the level state machine is pure. Proposed
`tests/library_level_test.cpp` over a small synthetic `LibraryIndex`:

- descend artist -> album -> track, then ascend back, asserting `lib_level_`,
  `lib_artist_`, `lib_album_` at each step, and that ascending clears exactly one member
- the three remembered-cursor members: descend, ascend, land on the same row; then descend
  into a *different* album and assert the track memory did not leak
- `tracksForAlbum` index-to-path conversion: **no index escapes the populate**, asserted by
  checking every produced identity is a path present in the index
- degenerate rows: `track_no == 0` omits the number, empty title falls back to the stem,
  zero duration omits the parenthetical
- an album name shared by two artists yields disjoint track lists (§10 Q2)
- **a planted invalid-UTF-8 (Latin-1) artist and album name** survive populate at levels
  1-2 with no throw - the regression test for the class of bug that took the six guards

This requires the level logic to be reachable without curses. Levels 1-2 already are, being
pure `libidx` queries plus formatting; the descend/ascend transitions are the part that
today would live inside `activateSelection`. **Proposal: the transition rules go in a small
pure helper** (a `libidx::` free function or a header-inline struct taking current level and
members, returning the next) so the test links nothing. If that shape is rejected the state
machine stays in `UIManager` and this becomes hardware-only, which I would rather not do -
three levels times two directions is where an off-by-one lives.

**Not machine-tested:** rendering, header elision, glyphs, and anything requiring a font
path. Those are the gate.

`LibraryIndex.h` does **not** split. The corrected trigger is non-inline definitions, and
slice 4 adds none - it adds no `libidx` queries at all. Confirmed the surface suffices:
`artists()`, `albumsForArtist()`, `tracksForAlbum()` are all three levels need.

## 10. Open questions - need a ruling before implementation

**Q1 - Left. Blocking, because it decides code that has no shipped precedent (F3).** Three
options:

- **(a) Match the shipped podcast idiom.** Left exits `[Library]` entirely at every level;
  `[Back]` is the only one-level-up. Consistent with all six existing sections, zero new
  wiring, and it means the brief's "Left ascends" does not happen.
- **(b) Left ascends one level, exits at level 1** (what the brief describes). Better UX and
  matches the roadmap's §4 sentence. Costs an `in_library_` branch in `KEY_LEFT` and makes
  library the one section where Left differs from the other six.
- **(c) (b), plus retrofit `[Podcasts]`** so Left ascends there too. Consistent again, and
  the right end state - but it is a second section's behaviour change riding a library
  slice, and its gate belongs to podcasts.

**Recommendation: (b).** A three-level section where Left cannot ascend will read as broken
to anyone who did not implement it, and `[Drives]` this campaign already established that
Left gets a section branch when the section needs one. (c) is correct eventually and should
be its own small slice; I would not smuggle it in here. Note that under (a) *or* (b), Left
at level 1 should **leave the section without also moving `current_dir_` up a directory** -
the `[Drives]` precedent (`:8117-8121`) - because the shipped fall-through does both, and
the directory move is invisible until the user looks at the pane and finds themselves
somewhere else.

**Q2 - An album name shared across artists.** `albumsForArtist` and `tracksForAlbum` both
filter on `groupingArtist` *and* album, so "Greatest Hits" under two artists is already two
separate level-3 listings and there is no collision. No design needed - confirming the brief's
listed degenerate case is already handled, and it gets a test rather than code.

**Q3 - `O` inside a section.** Exits the section today, all six sections, shipped. Proposing
to leave it and state it in the gate (§6). Flagging in case it reads as a defect on Dos's
hardware, in which case it is its own cross-section slice.

**Q4 - Artist with one album, album with one track.** No auto-collapse proposed: entering an
artist with a single album shows a one-row list, not a jump straight to tracks. Skipping a
level would make the pane's depth depend on the data, so `[Back]` would land somewhere
different each time. Stating it because the brief asked and someone may want the shortcut;
I recommend against.

## 11. Gate - eyes-on, both platforms

Windows `wingui` and Linux `ncursesw` both, because they are different font paths and one
does not infer the other.

1. Descend artist -> album -> track on the real 2,155-file collection; the rows at each
   level are the right ones.
2. `[Back]` at levels 3, 2, 1 - ascends, ascends, leaves.
3. Left at every level - behaviour per whichever Q1 option is chosen, stated explicitly in
   the debrief as "this is what it does" rather than "this is what it should do".
4. Header reflects the level, and a long artist or album name does not push the `[Back]`
   hint off the pane.
5. Cursor: descend from a mid-list artist, ascend, land back on that artist. Repeat at album
   level. Then descend into a *different* album and confirm the cursor does not land on a
   remembered row from the other one.
6. **Refresh leaks nothing at any level**, exercised deliberately at all three: press
   whatever reaches `refreshDir()` from levels 1, 2 and 3 and confirm the pane returns to a
   clean directory browser with no library rows and no stale header.
7. **Non-ASCII artist, album and track names render** on both platforms.
8. **A planted Latin-1 album and track name**, deterministic - a file written with a raw
   `0x92`/`0xE9` in the tag rather than hoping the collection supplies one. Enter and
   `[Back]` over it, no throw, no exit.
9. A scan completing while a level-3 listing is open re-lists in place and keeps the level.
10. Every existing section still enters, draws and exits: `[Drives]`, `[Recent]`, `[FAVs]`,
    `[Radio]`, `[Podcasts]` at both its levels, `[Books]`.
11. Pinned row order unchanged, and `[Library]` still renders after `[Books]` in **both** the
    normal browser and `[Drives]` - the specific claim that failed a gate in slice 3 and the
    reason `BrowserPins.h` exists.
12. Playback, rip, and CD paths untouched: play a file, and run the trimmed CD gate if
    anything outside `UIManager` moved. Nothing outside it is expected to.

Machine gates: ctest both toolchains, `--no-tests=error`, expected 45/45 Windows and 46/46
Linux plus the new test. Brace-balance and scoped-diff audit before handoff.

**Check the outcome, not the change.** Nothing in the debrief asserts rendered behaviour
unless it was run or the mechanism can be pointed at; everything else goes in this list as a
question.

## 12. Files expected to change

`src/UIManager.cpp`, `include/UIManager.h`, `tests/library_level_test.cpp` (new),
`tests/CMakeLists.txt`, `CHANGELOG.md` under `[1.5.0]`, and this document committed as
design-of-record. Plus a pure transition helper if §9's proposal is accepted -
header-inline, in `include/` or added to `LibraryIndex.h`.

**Not touched:** `LibraryIndex.h` queries, `LibraryScanner.*`, `BrowserPins.h`, `Config`,
`Version.h`, audio thread, `ar_crc`, CD path, rip path, scrobblers, plugins.
