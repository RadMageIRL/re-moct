# DESIGN NOTE - Library slice 5: playback, queue, and `\` search

**Scope ID:** LIB-S5. **Status:** proposed, awaiting sign-off. Nothing implemented.
**Probed against tip `0eb7cd6`** on 2026-07-26. Every anchor was read in the live tree.

Predecessors 1-4 shipped, pushed, CI-green, and slice 4 reported good by Dos. Design of
record: `docs/library-index-plan.md`, `docs/ROADMAP-library-view.md`,
`docs/library-slice4-plan.md`.

The roadmap's framing for this slice is **"wiring over a confirmed-solved path"**, and the
probe agrees: no new playback plumbing, no audio-thread involvement, nothing in the rip or
CD path. Most of this slice is one line plus deciding, deliberately, what that line turns on.

---

## 0. FINDINGS FIRST

### F1 - The folder-browser precedent PLAYS IMMEDIATELY. This changes what Enter does.

The ratified ruling is *"Enter on a library track row APPENDS TO THE PLAYLIST
(folder-browser precedent), not standalone playback (podcast precedent)."*

I read the precedent rather than assuming it. `activateSelection`'s browser branch,
`src/UIManager.cpp:8990-8996`:

```
} else if (PlaylistManager::isSupportedAudio(full.string())) {
    size_t idx = playlist_.addTrack(full.string());
    playlist_.selectAt(idx);
    if (auto p = playlist_.currentPath(); p.has_value()) {
        audio_.play(p.value());
        config_.addRecentTrack(p.value());
    }
}
```

So the precedent is **append, select, play, and record as recently played** - four things, not
one. "Appends to the playlist" is therefore not the opposite of playing; the contrast the
ruling draws is with the **podcast** path, where an episode plays standalone and never becomes
a playlist member at all (which is also why it needs its own scrobble guard and its own resume
bookkeeping).

**Two readings, and they produce different products:**

- **(a) Faithful to the named precedent** - Enter on a library track appends, selects, plays,
  and adds to Recent. Identical to pressing Enter on that same file in the folder browser.
- **(b) Literal to the word "appends"** - Enter adds the track to the end of the playlist and
  does **not** start playing it, so you can walk an album adding tracks.

**Recommendation: (a).** It is the precedent that was named, it makes a library row and a
browser row behave identically for the same file, and (b) would leave Enter with no way to
actually play the thing you selected. Note also that **(b) is nearly what `q` already does**
(§3), so choosing (b) would make Enter and `q` near-duplicates while removing the ability to
play from the library at all.

**This is the one question in the slice that needs a word before I build**, because it is not
recoverable by reading the code - both readings are consistent with the ruling as written.

### F2 - The BANK document Dos pasted is stale and self-contradictory

Not code, so it blocks nothing, but it is design-of-record and it was pasted as the brief:

- Header says **"Status: BANKED, POST-HTOA. Not scheduled. Not a green light to build."**
  while its own §6 says **"Library view is CURRENT WORK and ships as 1.5.0."** Those cannot
  both be true, and the second is the correct one.
- §8 calls `docs/ROADMAP-library-view.md` **untracked**, "commits as design-of-record if and
  when the campaign starts". It has been tracked since `c6287ee`, five commits ago.
- §5's own CP1252 bullet is the **corrected** version, so that trap is right. Good.
- §2's rationale cites "the slice-5 speculative-hardening mistake" - that is the **podcast**
  campaign's slice 5, not this one. Worth disambiguating before someone reads it as a library
  precedent.

There is **no BANK file in the repo** (`git ls-files docs/ | grep -i bank` is empty), so this
lives outside the tree and I cannot correct it in place the way I corrected the roadmap. Flagged
for whoever owns it.

### F3 - The draw-loop cost is already mitigated, and it is smaller than I told you

In the slice-4 debrief I flagged that `browserEntryPath` is called per visible row per frame
from the draw loop and that slice 5 must budget for it. Having now read the call site
(`src/UIManager.cpp:3056-3060`), that was **overstated**:

```
bool marked = false;
if (!marked_.empty() && !is_dir) {
    std::string ep = browserEntryPath(idx);
    marked = !ep.empty() && marked_.contains(ep);
}
```

It is gated on **`!marked_.empty()`** - the mark set being non-empty. In the common case
nothing is marked and the call never happens at any zoom level. The per-frame cost appears
only while the user is holding marks, which is a deliberate, transient state. Correcting my
own earlier statement: this does not need a redesign, and the flip is cheap.

---

## 1. The seam - one line

`browserEntryPath` (`src/UIManager.cpp:9386`) currently returns `{}` for the whole section:

```
if (in_drive_list_ || in_radio_ || in_podcasts_ || in_library_) return {};
```

Slice 5 narrows the library term to levels 1-2 only:

```
if (in_drive_list_ || in_radio_ || in_podcasts_) return {};
// Library: levels 1-2 are tag text and must never become a path. A level-3 row's
// identity IS an absolute OS-origin path, produced by the scanner's own directory
// walk, so it resolves like any [FAVs] row.
if (in_library_ && lib_nav_.level != libnav::Level::Tracks) return {};
```

Everything in §4 follows from that line. **Levels 1-2 stay hard-excluded on safety** and that
does not change in this slice or any later one: an artist or album identity is tag text, and
invalid UTF-8 throws out of `fs::path()` and out of `fs::exists(s, ec)` too, `ec` included,
because the conversion runs before `ec` applies.

## 2. Enter at level 3

Replaces slice 4's placeholder toast in `activateSelection`'s library branch (`:8822`). Under
reading (a) of F1, it does exactly what the browser does for the same file: `addTrack`,
`selectAt`, `play`, `addRecentTrack`.

**It routes through `browserEntryPath(dir_cursor_)`, not through `dir_entries_` directly**, so
there is one definition of "what path is this row" and Enter cannot disagree with `q`, `u`,
`x` and `;` about it.

**`fs::exists` is checked first, and this is not defensive boilerplate.** The index is a
cache: a track can be deleted or moved between the last scan and this keypress, and the
library is the one place in the program where a row is guaranteed to be describing a
possibly-stale snapshot rather than a live directory read. A missing file gets an honest toast
naming it, not a silent no-op and not a failed `audio_.play`. `[FAVs]` and `[Recent]` both
already do this (`if (!fs::exists(name)) return;` at `:8901` and `:8778`) - the difference is
that they fail silently, and a library row should say so, because the fix is a rescan and the
user needs to know that.

Enter at levels 1-2 keeps slice 4's descend behaviour, untouched.

## 3. Queue (`q`) and add-without-playing (`a`)

Both currently break out for the whole section. Both become level-3-only.

**`q`** (`:7537`) builds a `PlaylistEntry` and enqueues. The generic browser branch below it
already resolves absolute entries (`in_recent_ || in_favs_ || ... || fs::path(nm).is_absolute()`),
so the minimal change is to let level 3 fall into it - but I propose adding `in_library_` to
that list **explicitly** rather than relying on `is_absolute()`, because relying on it means
the safety of levels 1-2 depends on a tag string happening not to look absolute, which is not
a property tag text has.

**`a`** (`:8127`) is add-to-playlist without playing. At level 3 it appends and stops there.
Note this is exactly reading (b) of F1 - so **whichever way F1 is decided, both behaviours
remain available**, one on Enter and one on `a`; F1 only decides which key is which.

## 4. What the seam turns on, site by site

The brief for slice 4 required each hazard site to be re-decided rather than assumed; the same
applies to what slice 5 switches on. All 21 `in_library_` sites, classified.

| site | what it is | slice 5 |
|---|---|---|
| `:9386` | `browserEntryPath` | **flip for Tracks** (§1) |
| `:8822` | `activateSelection` library branch | **Enter plays at level 3** (§2) |
| `:7537` | `q` enqueue | **enable at level 3** (§3) |
| `:8127` | `a` add to playlist | **enable at level 3** (§3) |
| `:2926` | header text | **update** - level 3 gains an `Enter:` verb it deliberately lacked |
| `:3056` | draw loop, marked-file glyph | **enabled for free by the flip.** A marked library track now shows its `*`, which is required: `u` can mark one, and a mark you cannot see is worse than no mark |
| `:7859` | `u` mark / unmark | **enabled for free.** `convertSupportedInput` gates it |
| `:7885` | `x` convert single file | **enabled for free.** Converts that one track, output beside the source |
| `:7978` | `;` chapters | **enabled for free.** MP4-family only, exactly as for a browser file |
| `:7819` | `Shift+S` playlist-reformat popup | **enabled for free and inert by construction** - it only fires on `.m3u/.pls/.xspf`, and the index holds only `AUDIO_EXTS`, so no library row can ever match |
| `:3038` | draw loop, `fs::is_directory` | **stays excluded.** No library row is a directory at any level; exclusion is now correct on semantics, and it avoids an `fs` stat per visible row per frame |
| `:7775` | bookmark `current_dir_` | **stays excluded.** Operates on `current_dir_`, not the row |
| `:7893` | `x` scope `convert_src_dir_` | **stays excluded.** A library level is not a directory, so "convert this folder" has no referent |
| `:3778` | pane label | unchanged |
| `:9701`, `:9746`, `:9776` | reset / ascend / poll | unchanged (slice 4) |

**`f` (favourite) and `b` (add to Books) - the two genuine judgement calls.** Both currently
exclude the whole section via their own chains (`:7658`, `:7757`) rather than through
`browserEntryPath`, so neither is enabled by the flip; each needs a decision.

- **`f` favourite: ENABLE at level 3.** `[FAVs]` is a path-keyed set, a library track is a
  path, and "I found this by artist and want it in favourites" is the obvious motion. The
  handler already re-checks `isSupportedAudio` and `isCDTrackPath`.
- **`b` Books: ENABLE at level 3.** Same argument, and its handler already gates on
  `PlaylistManager::isAudiobook(full) && fs::exists(full)`, so a music track is rejected on
  its own terms rather than by the section guard.

Both are one term each in an existing conjunction, and both are listed in the gate.

## 5. `\` search - confirm, do not build

The roadmap said this "should work by construction - confirm rather than build". **Confirmed
by reading, and there is nothing to write.**

`\` (`:7799`) sets `search_source_` from `focus_` and opens the shared input bar. The match
loop (`:8443`) iterates `dir_display_` for the browser source and its own comment states it
"covers dirs, feeds, episodes, radio, books, favs, recent, drives in one branch (all funnel
through `dir_display_`) - **NOT** the `in_*` sub-mode flags". Library populates
`dir_display_` in lockstep at all three levels, so all three are searchable already.

The pick path is `jumpToBrowserIndex` (`:6949`), which I read in full: it sets `dir_cursor_`,
calls `ensureDirCursorVisible`, sets focus, and returns. **No path is constructed**, so it is
safe at levels 1-2 as well as 3. Staleness is handled by the existing validate-on-use guard -
the row's live display text must still equal the snapshot taken at search time, or the jump is
refused - which is the same discipline as `restoreCursor` matching by name.

`browserSectionLabel()` already returns `"Library"` (`:3778`), so a not-found message names the
right list.

**Slice 5 adds no code here. It adds gate lines**, because "works by construction" is a claim
about a mechanism and the mechanism has never been run at three levels.

## 6. Open question - appending a whole album

The thing a user will reach for within a minute of this shipping: standing on an album row and
wanting **all of it** in the playlist. Slice 5 as scoped cannot do it - Enter on an album
descends (slice 4's ruling) and `q`/`a` operate on the row under the cursor, which at level 2
is an album name and not a path.

**Recommendation: not in slice 5, and it should be its own small slice rather than slice 7
filler.** It is genuinely new behaviour, not wiring: it needs a key that is free at level 2, a
decision on order (the index's disc/track order, which `tracksForAlbum` already provides) and
on whether it plays or only appends, and it is the first library operation that touches many
playlist rows at once. All of that deserves a gate of its own. Raising it here rather than
discovering it on the gate.

## 7. Tests

**Machine-testable and worth it:** nothing new in `libnav`, so no new pure surface. What can be
covered is the level-gating predicate - "does this level resolve to a path" - which is the
whole slice in one function. I propose extending `tests/library_level_test.cpp` with a small
pure helper rather than adding a second test binary:

```
// in LibraryNav.h
inline bool rowIsPath(Level l) { return l == Level::Tracks; }
```

asserted for all three levels, and used by `browserEntryPath` so the product and the test read
the same predicate rather than two copies of `!= Level::Tracks`. That is a two-line helper
earning its place only because the alternative is the polarity of that comparison living
uncovered in the middle of a curses function - and a flipped polarity there is exactly a
"levels 1-2 build a path from tag text" crash.

**Not machine-testable:** everything else in this slice is playback, curses key handling, and
real files. That is the gate.

## 8. Gate - eyes-on, both platforms

Windows `wingui` and Linux `ncursesw`.

1. Enter on a library track: it appends to the playlist, selects, plays, and appears in
   `[Recently Played]` - **or** appends without playing, per the F1 ruling. State which was
   built and check that.
2. Enter at levels 1-2 still descends; nothing about slice 4 regressed.
3. `q` on a library track enqueues it; the queue plays it in turn.
4. `a` on a library track appends without playing.
5. `u` marks a library track and **the `*` is visible on the row**; `U` clears; `x` converts
   that single track and writes beside the source.
6. `;` on an MP4-family library track opens chapters; on a FLAC it says so honestly.
7. `f` adds a library track to `[FAVs]` and it is there after a restart; `b` on a music track
   is refused on its own terms, and on a genuine audiobook file is accepted.
8. **A track deleted from disk since the last scan**: Enter names the missing file rather than
   silently doing nothing. This is the one case the index makes reachable and the folder
   browser cannot.
9. `\` search at **each of the three levels**: matches, jumps, and the not-found message says
   "browser: Library". Then search, let a rescan land underneath it, and confirm the
   validate-on-use guard refuses a stale jump rather than jumping to the wrong row.
10. Level-3 header now shows an `Enter:` verb and still fits with a long artist and album.
11. Non-ASCII and a planted Latin-1 track name: mark it, convert it, favourite it, play it. The
    identity is a real path here, so this is the first slice where those keys touch library
    text at all.
12. Every existing section still enters, draws, exits; `[Podcasts]` playback, download and
    resume unaffected; folder-browser Enter/`q`/`u`/`x`/`;` unchanged.
13. Rip and CD paths untouched - run the trimmed CD gate only if anything outside `UIManager`
    moved. Nothing outside it is expected to.

Machine: ctest both toolchains, `--no-tests=error`, expected 46/46 Windows and 47/47 Linux.
Brace-balance and scoped-diff audit before handoff.

**Check the outcome, not the change.** No rendered behaviour in the debrief unless it was run
or the mechanism can be pointed at; everything else goes in this list as a question.

## 9. Files expected to change

`src/UIManager.cpp`, `include/UIManager.h` (only if the `rowIsPath` helper needs a decl - it
does not, it lives in `LibraryNav.h`), `include/LibraryNav.h`, `tests/library_level_test.cpp`,
`CHANGELOG.md` under `[1.5.0]`, and this document as design-of-record.

**Not touched:** `LibraryIndex.h` queries, `LibraryScanner.*`, `BrowserPins.h`, `Config`,
`Version.h`, `PlaylistManager`, `AudioManager`, the audio thread, `ar_crc`, the CD and rip
paths, scrobblers, plugins.

**No index or query-surface change**, per the standing constraint: levels 1-3 need
`artists()`, `albumsForArtist()` and `tracksForAlbum()`, which all exist and all shipped in
slice 1.
