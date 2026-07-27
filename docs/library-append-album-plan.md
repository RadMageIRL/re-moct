# DESIGN NOTE - Library: append whole album

**Scope ID:** LIB-AA. **Status:** proposed, awaiting sign-off. Nothing implemented.
**Probed against tip `bf79789`** on 2026-07-26. Every anchor read in the live tree.

Predecessors LIB-S1 through LIB-S5 landed, pushed, CI-green, hardware-gated. Sequenced ahead of
LIB-S6, which is greenlit and waiting (`docs/library-slice6-plan.md`).

**It stays small and self-contained.** No new key, no new playback plumbing, no index or query
surface change, and one new pure function that removes a duplication rather than adding one.

---

## 1. The binding - `a` at level 2, and it costs nothing

**There is no need to split an alias, consume a global, or touch an F-key.** `a` already means
"add to the playlist without playing" in three places: the folder browser
(`src/UIManager.cpp:8163-8167`), and library level 3 as of LIB-S5. At library levels 1-2 it is
currently an **explicit silent no-op** that LIB-S5 wrote deliberately.

So the whole binding question resolves to: **give that existing no-op a meaning at level 2.**

| level | `a` before | `a` after |
|---|---|---|
| 1 artists | silent no-op | silent no-op (unchanged - see §4) |
| 2 albums | silent no-op | **append this album** |
| 3 tracks | append this track | append this track (unchanged) |

`a` means the same thing at every depth - "add what the cursor is on" - and the only thing that
changes is what the cursor is on. Enter stays as LIB-S4 ruled it: descend at levels 1-2, play at
level 3. Nothing is displaced, nothing needs cross-platform proof, and the section-scoped-verb
idiom the brief points at (`d`/`D`, `/`, `a` in `[Podcasts]`; `F12` in `[Drives]`) is followed
exactly.

`A` is an alias of `a` (`case 'a': case 'A':` at `:8141`), so it stays an alias. **If Dos wants
append-and-play**, splitting that alias inside `[Library]` only would give `a` = append, `A` =
append and play, which is precisely the `d`/`D` podcast idiom. **I am not recommending it**: it
doubles the surface for a motion the user can already complete in one more keypress, and the
album is right there in the playlist to press Enter on. Offered because it is cheap if wanted.

## 2. Plays or appends - appends, no play

`a`'s whole meaning is "append without playing", and that is what makes it the right key. Making
it play would either duplicate Enter or make `a` mean two different things by depth.

Consequence worth stating rather than discovering: **appending an album while shuffle is on will
not play it in album order.** `addTrack` calls `rebuildShuffleOrder()`, which re-randomises the
whole order when `shuffle_` is set. That is not a defect and not new - a single add does the same
- but a user who appends an album expecting it in order and has shuffle on will not get it.
Shuffle is shuffle; noting it so it is not read as an ordering bug on the gate.

## 3. Order - the index's, and provably the order shown

`libidx::tracksForAlbum` already sorts **disc, then track number, then title, then path**
(`include/LibraryIndex.h:436-440`). The last two tie-breakers exist so an untagged rip, where
every `track_no` is 0, still has a total and stable order.

**Correcting the brief:** it says "track number then disc number is the obvious reading". The
index does **disc then track**, which is the correct one for a multi-disc set - disc 1's tracks
1-12, then disc 2's - and it is already shipped and already what level 3 displays. No change.

| case | behaviour |
|---|---|
| track numbers absent (all 0) | falls to title, then path - the filename decides, stably |
| track numbers duplicated | same tie-break; total order, no arbitrary swap |
| tags disagree with filename order | **the tags win, deliberately.** That is what a library view is for; the folder browser is there when you want filename order |

**And it is the same list level 3 draws, by construction.** `showLibraryTracks` already builds a
vector of paths in row order (its `ident` vector, for cursor restore). Rather than write a second
loop that must agree with it, I propose factoring that into one pure function:

```
// LibraryNav.h
std::vector<std::string> albumTrackPaths(const libidx::LibraryIndex&,
                                        const std::string& artist,
                                        const std::string& album);
```

used by **both** `showLibraryTracks` (to draw) and the append (to add). "It appends in the order
you see" then stops being two loops that happen to agree - which is the defect class
`BrowserPins.h` was written to end - and becomes one list with one definition.

This lives in `LibraryNav.h`, **not** `LibraryIndex.h`, so the index query surface stays exactly
as it has been since LIB-S1. `LibraryNav.h` already includes `LibraryIndex.h` and is already the
home for library-only derived logic. **No new query; nothing to raise.**

## 4. Which levels - level 2 only; level 1 explicitly excluded

Album rows at level 2 are the ask and are what this builds.

**Level 1 (all albums by this artist) is excluded, and stays the silent no-op it is today.** Not
deferred by omission - decided:

- It is **unbounded from a single keypress with no confirmation**. An artist with 40 albums is
  400+ tracks, and every one is a synchronous tag read (§6), so it is simultaneously the largest
  playlist mutation in the program and its longest UI stall.
- Doing it safely wants a count and a confirmation - "append 437 tracks?" - which is new UI, and
  new UI is what makes a slice stop being small.
- It is one `case` arm away once wanted, so excluding it now costs nothing later.

If Dos would rather have it, it is a separate small slice with a confirmation popup, not a rider
here.

## 5. Feedback, and stale rows - one answer for both

Appending 18 rows silently is exactly the objection the brief raises, and the missing-track
question has the same answer: **a toast that reports what actually happened.**

- normal: `Added 18 tracks` + the album name
- some missing: `Added 15 tracks, 3 missing` - the append is **not** aborted, because 15 tracks
  is more useful than nothing, and the count is what stops silence on 3 of 18
- all missing: nothing is appended, and the toast says so and points at the remedy - the LIB-S5
  rule, that a library row naming a deleted file says so because the fix is a rescan and silence
  cannot convey that
- nothing to add (see below): `Already in the playlist`

**The subtlety that would otherwise make the count a lie:** `addTrack` **dedups by path** -
`for (i...) if (entries_[i].path == path) return i;` at `PlaylistManager.cpp` - so re-appending an
album adds nothing the second time, and appending an album you already own three tracks of adds
15, not 18. The count must therefore report **rows actually added**, by comparing
`playlist_.size()` across the loop, not the number of paths iterated. Reporting the iteration
count would over-report on every partially-owned album, which is the common case for anyone who
built a playlist by hand first.

Per-track missing files are skipped via the same `fs::exists` check LIB-S5 established, through
the existing safe-path helper, never a bare `fs::exists(std::string)`.

## 6. No new plumbing - the loop is the shipped pattern

**`PlaylistManager::addDirectory` already loops `addTrack`:**

```
std::sort(found.begin(), found.end());
for (const auto& f : found) addTrack(f);
```

So appending N tracks by looping `addTrack` is not an invention, it is the existing bulk-add
shape, and it is called synchronously from `main.cpp:432`. `addDirectoryAsync` exists because a
*directory* is unbounded, not because the loop is wrong.

**We do not call `addDirectory` itself**, and the reason is the whole point of the feature: it
sorts by **path**, and an album's order comes from its **tags**. Using it would silently give
filename order.

`playlist_.addTrack(p)` per track, in `albumTrackPaths` order. Nothing else.

### Cost - inference now, measurement at implementation

`addTrack` calls `populateMetadata`, one `TagLib::FileRef` with
`AudioProperties::Fast` - a tag and header read, not a decode. **I have not measured it**, and I
am not going to state a number as though I had. The bound available: the LIB-S2 scanner measured
2,155 files in 15.8 s, which is ~7.3 ms per file *including* the directory walk and stat, so a
tag read is at or under that. On that basis an 18-track album is roughly 130 ms and a 40-track
boxset disc roughly 280 ms - a one-off hitch on a deliberate keypress, not a stall.

**I will measure it on implementation and put the real number in the debrief.** The threshold at
which I would stop and raise rather than invent: if a typical album exceeds ~250 ms, an async or
batched path is a design change and Dos should decide it, not me.

`rebuildShuffleOrder()` also runs per add - O(n) each, so O(n·18) - which is negligible in CPU
terms even on a large playlist, and is pre-existing behaviour for every add path in the program.

## 7. Staleness stays dissolved

`tracksForAlbum` returns **indices**, valid only against the index instance that produced them.
`albumTrackPaths` is where they are converted, and they die at its closing brace: it takes the
index by const reference, resolves each subscript to `tracks[i].path`, and returns
`std::vector<std::string>`. **The vector that leaves the function contains paths only.**

This is the first library operation that genuinely wants a *list* rather than a single row, which
is exactly why the conversion belongs in one named, tested function rather than in a loop inside a
key handler. Nothing outside it sees a subscript, and the rule that has held for five slices holds
here for a stronger reason than before.

An album row's identity remains **tag text** and nothing constructs a path from it; the paths come
from the index, which is OS-origin because the scanner's own directory walk produced them.

## 8. Tests

`albumTrackPaths` is pure, so the ordering contract is machine-testable in
`tests/library_level_test.cpp` (extended, not a new binary):

- order is disc, then track number, then title, then path - asserted against a synthetic
  multi-disc album with deliberately shuffled input
- **every returned element is a path present in the index** - the staleness assertion, in the same
  shape LIB-S4 used: an escaped subscript would fail it by not being a path
- all track numbers 0 -> filename order, stable and total
- duplicated track numbers -> total order, no arbitrary swap
- an album shared by two artists -> disjoint lists (already covered for `tracksForAlbum`; extended
  to the path form)
- a non-existent artist or album -> empty vector, not a crash
- a planted invalid-UTF-8 album NAME still selects the right tracks - the name is compared as
  bytes and never becomes a path
- **and that it returns exactly what `showLibraryTracks` would draw**, which is the point of
  factoring it: assert the same call produces the row order the display uses

**Not machine-tested:** the toast text and counts, the skip-missing behaviour (needs a real
filesystem and a deleted file), the keypress, and the append itself. Gate.

## 9. Gate - eyes-on, both platforms

Windows `wingui` and Linux `ncursesw`.

1. `a` on an album row appends every track, **in the order level 3 shows them**, and the playlist
   plays through in that order.
2. A **multi-disc** album appends disc 1 then disc 2, not interleaved.
3. An album with **absent** track numbers appends in filename order; one with **duplicated** track
   numbers appends with no rows lost or repeated.
4. An album with a **deliberately deleted** track appends the rest and the toast names the missing
   count. An album with every track deleted appends nothing and says so.
5. **Re-appending the same album** adds nothing and says so; appending an album you already own
   part of reports the number **actually added**, not the album's track count.
6. A **planted non-ASCII album name** (deterministic, and one with a raw Latin-1 byte) appends and
   plays.
7. `a` is **inert at level 1** and unchanged at level 3; `a` in the folder browser, `[Podcasts]`
   and everywhere else is unchanged.
8. Single-track Enter, `a`, `q`, `*`, `b` all still behave as LIB-S5 shipped them.
9. Appending a long album does not visibly hang the UI (this is the §6 measurement's hardware
   counterpart).
10. Every other section still enters, draws, exits, and plays.

Machine: ctest both toolchains, `--no-tests=error`, currently 46/46 Windows and 47/47 Linux.
Brace-balance and scoped-diff audit before handoff. **Verification split as in LIB-S4/S5** - what
was run and green versus what is the gate's to confirm; no rendered or audible behaviour asserted
unless run or the mechanism can be pointed at.

## 10. Files expected to change

`include/LibraryNav.h` (`albumTrackPaths`), `src/UIManager.cpp` (the `a` handler's library branch,
`showLibraryTracks` using the new function, the toast), `include/UIManager.h` (only if a helper
needs a decl - it does not), `tests/library_level_test.cpp`, `CHANGELOG.md` under `[1.5.0]`, and
this note as design-of-record.

**Not touched:** `LibraryIndex.h`, `LibraryScanner.*`, `BrowserPins.h`, `Config`, `PlaylistManager`,
`AudioManager`, the audio thread, `ar_crc`, the CD and rip paths, scrobblers, plugins, `Version.h`.
Nothing from LIB-S6 lands here. `Ctrl+T` untouched.

## 11. Confirmation that it stayed small

One new pure function that deletes a duplicate loop; one `case` arm given a meaning at one level;
one toast. No new binding, no alias split, no new plumbing, no query change, and the level-1 case
excluded on purpose to keep it that way. **If the cost measurement in §6 comes back badly, that is
the one thing that could make this stop being small, and I will raise it rather than build an
async path.**
