# DESIGN NOTE - Library slice 6: toggle, music root, rescan, first-run UX

**Scope ID:** LIB-S6. **Status: GREENLIT 2026-07-26, BUILD NOT STARTED - sequenced behind
append-whole-album.** Probed against tip `bf79789`. Every anchor read in the live tree.

Predecessors 1-5 shipped and pushed; slices 3, 4 and 5 gated by Dos. Design of record:
`docs/library-index-plan.md`, `docs/ROADMAP-library-view.md`, `docs/library-slice{4,5}-plan.md`.

---

## RULINGS - settled by Dos at greenlight, do not re-derive

Recorded here rather than left in a chat log, because this note is what the next session reads.
Every proposal below was accepted as written; these are the ones that were genuinely open.

| # | Question | RULING |
|---|---|---|
| 1 | Toggle default | **ON.** The scan is lazy - first *entry*, not startup - so a user who never opens `[Library]` pays no scan, no index, no thread. One extra row is the entire cost. Crossfade defaulted off because it silently changed how playback SOUNDS; this changes nothing until entered, and default-off would make six slices undiscoverable. |
| 2 | What "off" means | **ABSENT**, from the browser AND `[Drives]`. An inert row is a dead end the user will press, which then needs a message explaining a feature they turned off. **The BANK doc's "sees one extra sidebar row" wording was the contradiction and is being corrected on Dos's side.** |
| 3 | Rescan key | **`F12` scoped to `in_library_`.** Approved on the specific evidence, NOT by relaxing the F-key caution: `F12` is already the section-scoped manual-refresh key for `[Drives]` hot-plug, same semantic, same scoping idiom, shipped and gated. **F11 was avoided; F12 is proven.** `Shift+R` scoped to the section is the fallback if `F12` misbehaves on either platform. |
| 4 | Cancel key | **`Esc` while a scan is running.** Established abort key, no new binding, scoped by a condition only true during a scan. |
| 5 | F3 - route `enterDriveList` through `kPins` | **APPROVED, and bounded.** Deliberately authorised scope widening: route it and filter once. **No other `[Drives]` work.** |
| 6 | F5 + F6 - the cancel path | **BUILD IT, and it is not polish.** Esc-to-cancel makes SHIPPED code reachable; slice 3's `lib_scan_cancelled_` and arm-a-retry are dead code today. **Cancelled and unreadable-root must be DISTINGUISHABLE, and each must say what actually happened.** |
| 7 | `library_root` changed between runs | **APPROVED as a narrow, named exception** to "no scan on section entry". Fires only when a config value actually changed, and it settles. |

**Sequencing:** append-whole-album is built FIRST (small, self-contained, separately approved),
then this slice, so first-run UX is written once over the finished feature. The brief's own
sequencing note was wrong twice - it named library-wide search as a predecessor that was never
greenlit, and it under-applied its own reasoning.

**F4 stands as reported, not built.** Config reads once at startup (`src/main.cpp:213`), so a
mid-browse disable cannot happen and the brief's "must not strand state" gate is unreachable. It
becomes real only with an in-app toggle key, which is not proposed and not in scope.

---

## 0. FINDINGS FIRST

Six, and **three of them change what this slice has to build.**

### F1 - SEQUENCING: two slices may sit ahead of this one, not one

The brief's sequencing note anticipates **one** slice ahead of it (library-wide search) and
says the brief would be re-issued after it. But Dos also approved **append-whole-album as its
own slice, after LIB-S5**, in the LIB-S5 greenlight. That is two candidate predecessors, and I
have not seen a library-wide-search slice greenlit at all.

I have written this note anyway, because it is cheap and most of it is independent of both. But
the brief's own reasoning applies with more force than it realises: **first-run UX is the one
part that genuinely would be written twice.** What a first-run pane says, and what the section
advertises in its header, both depend on what the finished feature can do.

**Recommendation: build append-whole-album next** (small, self-contained, already approved),
**then this slice**, and treat library-wide search as a separate question - it is a bigger
feature than either and its scope is not written down anywhere I can read. Dos's call.

### F2 - "Shift+letter is the safe category" does not hold: every letter is taken

Measured, not assumed. Enumerating `case '<c>'` across `handleInput` plus the pre-switch `if`
handlers: **every letter of the alphabet is bound in both cases.** `v`/`V` looked free in the
switch and is handled at `src/UIManager.cpp:6781` by an `if` ahead of it (the visualiser
toggle). Most pairs are aliases - `case 'm': case 'M':` and so on - so taking a Shift+letter
generally means *splitting an existing alias*, which changes the uppercase form's behaviour
rather than using free space.

**But there is a better answer than any letter, and it is already shipped.** `F12` is the
manual-refresh key, scoped to a section, at `src/UIManager.cpp:7310`:

```
case KEY_F(12):   // refresh the drive list (pick up hot-plugged drives)
    // Hot-plug isn't auto-detected ([Drives] only rebuilds on entry, and the
    // periodic dir re-scan skips the drive list); F12 is the manual trigger,
    // re-running the same enterDriveList() rebuild. No-op outside [Drives].
    if (in_drive_list_) {
```

That is **the same semantic slice 6 needs** - "this section deliberately does not auto-detect
changes, so here is the manual trigger" - with the same section scoping, in shipped and gated
code. `F2`, `F3`, `F6`, `F7`, `F8` and `F12` are all bound and working on both platforms, so
the brief's F-key caution is right in general and wrong for `F12` specifically: **F11 was
avoided, F12 is proven.**

**Proposal: `F12` inside `[Library]` = rescan.** No global binding is consumed, nothing is
displaced, `F12` keeps meaning "refresh this section" everywhere it does anything, and it needs
no cross-platform proof beyond the fact that `[Drives]` already ships it. If Dos prefers a
letter, the least-bad is `Shift+R` scoped to `in_library_`, splitting the `r`/`R` repeat alias
inside the section only - `r` would still cycle repeat there - which is exactly what `d`/`D`
already do in `[Podcasts]`. I recommend `F12`.

### F3 - There is a THIRD hand-maintained list of section rows, and the toggle needs it

`refreshDir()` reads `browserpins::kPins` (5 references). **`enterDriveList()` reads it zero
times** - it hand-inserts the same section names as literals, in reverse order, at `begin()`:

```
dir_entries_.insert(dir_entries_.begin(), "[Bookmarks]");
dir_entries_.insert(dir_entries_.begin(), "[Library]");
dir_entries_.insert(dir_entries_.begin(), "[Books]");
...
```

Slice 3b's header scopes its own claim to the directory pane, so this is not a false statement
in `BrowserPins.h` - but it is a third copy of the same list, and **the toggle has to filter
both**. The `[Drives]` copy is the one that will be forgotten, for exactly the reason slice 3b
exists: it is a separate literal list that has to agree with another one, and reverse-insert
order makes it the harder of the two to read.

**Recommendation: route `enterDriveList` through `kPins` as part of this slice**, so the toggle
is one filter at one place. This is not speculative refactoring - the payoff is immediate and
this slice is the thing that needs it. It is a change to shipped, gated code, so it carries
gate lines of its own (`[Drives]` still lists every section, in the same order, and
`[Bookmarks]` - which is NOT in `kPins` - still appears).

### F4 - Config is read ONCE at startup, so "toggling off mid-browse" is unreachable

`config.load()` is called once, at `src/main.cpp:213`. There is no runtime reload. So a
config-file toggle **cannot be flipped while the app is running**, and the constraint *"toggling
the section off must not strand state - a user who disables mid-browse should land somewhere
coherent"* describes a situation that cannot occur.

This is the LIB-S4 gate-item-9 situation again: I am reporting it rather than writing a gate
for something unreachable. It becomes reachable only if slice 6 adds an **in-app** toggle key,
which the brief does not ask for ("a config toggle") and which I am not proposing. If Dos wants
one, say so - stranding then becomes real and needs handling, and it is a different design.

### F5 - `ScanOutcome.completed == false` conflates "cancelled" with "unreadable root"

The field's own comment says one thing and the code does another. `include/LibraryScanner.h`:

```
// FALSE means the scan was cancelled and `index` is PARTIAL. Do not commit it
bool completed = false;
```

But `scanCollection` also returns `completed = false` when the root cannot be iterated
(`src/LibraryScanner.cpp:212-217`), which is not a cancellation. The consequence is live today:
`pollLibraryScan`'s else-branch turns any `completed == false` into

> `Library scan cancelled. Open [Library] again to retry.`

so **pointing `[Library]` at a folder that does not exist reports that you cancelled it.** The
brief's gate requires a nonexistent root to behave as designed "and say so", so this must be
fixed here.

**Fix without touching the scanner:** validate the root in the UI *before* `start()`, and never
begin a scan against a root that is missing or not a directory. That covers unset, empty and
nonexistent in one place and keeps `LibraryScanner` unchanged. A root that disappears
*mid-scan* still lands in the conflated branch; I propose correcting the header comment to say
"cancelled **or the root could not be read**" rather than adding a field, and stating the
residual case rather than papering it.

### F6 - Slice 3's cancel UX is UNREACHABLE in the shipped product

`library_scanner_.cancel()` is called from exactly one place: `src/UIManager.cpp:379`, the
destructor, so a 15.8-second scan does not hold up a quit. **There is no user-facing cancel.**

Which means slice 3's entire cancel-and-retry apparatus - `lib_scan_cancelled_`, the arm-a-retry
logic at `:9587-9589`, and the "cancelled, open again to retry" message - is only reachable by
quitting mid-scan, and quitting destroys the flag before the message can be seen (it is a
member, not persisted). It is dead code in the product.

Combined with F5, the outcome is sharper than either finding alone: **the only way to see the
words "Library scan cancelled" today is to point the library at an unreadable folder** - a
message that is wrong about what happened, produced by the one cause it does not describe.

So "offer cancel" is not a UX polish item in this slice; it is **making shipped code
reachable**. The brief's gate lines - cancel leaves no index, a cancelled rescan leaves the
previous index byte-identical - cannot be exercised at all until this exists.

---

## 1. The toggle

**Key name: `library`**, bool, `library=1` / `library=0`, sitting with the other feature bools
in `remoct.conf`. Rationale for the bare noun rather than `library_enabled` or `library_view`:
`nerd_icons`, `crossfade` and `rec_dir` are all named for the thing they configure with no
suffix, and `library` reads correctly in the file next to them. Load and save follow
`nerd_icons` exactly (`val == "1"`, written as `1`/`0`).

### Default - RULED ON (ruling 1). The reasoning is kept because it is the precedent

The crossfade precedent is "default to the behaviour that changes nothing for the existing
user", and that argued for `0`. **I think it argues differently here, and the difference is
that the scan is already lazy.** The trigger is first *entry*, not startup, so an existing user
who never opens `[Library]`:

- pays **no scan**, no index file, no tag reads, no worker thread
- sees exactly **one extra row** in the browser and in `[Drives]`

That is the entire cost of `library=1`. Crossfade defaulted off because it silently changed how
playback sounded; this changes nothing until it is opened. Against that, **default off makes the
feature undiscoverable** - a folder-player user who never reads the CHANGELOG will never learn
it exists, which is a strange outcome for five slices of work.

**Recommendation: default ON (`library=1`), section absent when off.** If Dos prefers off, the
whole slice is unaffected apart from one initialiser.

### What "off" means - RULED ABSENT (ruling 2)

The bank's wording ("a user who never enables it sees one extra sidebar row and pays no scan
cost") read as *present but inert*, so the two documents disagreed. Dos ruled absent and the
bank is being corrected. The reasoning, kept because it is the general principle:

**Absent.** A row that draws and then refuses to open is a dead end the user
will press, and pressing it must then produce a message explaining a feature they turned off -
which is worse than the row not being there. "A folder-player user can ignore it" is fully
served by absence. Absent from **both** the browser and `[Drives]`, which is what F3 is about.

With `library=0`: `[Library]` is filtered out of `kPins` (so neither reader pushes or pins it),
no `library.idx` is read or written, no scan can start, and `enterLibrarySection` becomes
unreachable. The two `if (name == "[Library]")` dispatches stay as they are - unreachable is not
the same as wrong, and deleting them would mean re-adding them when the toggle flips.

## 2. Music root

**Key name: `library_root`**, string, **mirroring `rec_dir` exactly** - which is the established
precedent for an optional path with a computed default (`include/Config.h:83`: `""` = a
`<music>/re-moct/...` default "resolved at record start"). So:

- **unset or empty** -> `CDRipper::musicRoot()`, resolved at scan start, never stored as a
  literal in the config
- **set** -> used verbatim
- written only when non-empty, exactly as `rec_dir` is (`src/Config.cpp:512`)

**Resolution is memoized.** `CDRipper::musicRoot()` is a COM `SHGetKnownFolderPath`; the
podcast campaign's scroll-crash fix memoized it as a function-local static
(`src/UIManager.cpp:10084`). The resolved root is read at scan start and in one empty-state
message - neither near the draw loop - but it is cached anyway so it cannot drift there later.

**The root is UNTRUSTED text for path purposes.** It comes from a file a user can type into, so
it is not guaranteed valid UTF-8, and on Windows `fs::path()` and even `fs::exists(s, ec)`
throw on invalid UTF-8 because the conversion runs before `ec` applies. Validation therefore
goes through the existing `utf8Path()` helper that `UIManager` already uses for the podcast
directories, inside `try`/`catch`, never through a bare `fs::exists(std::string)`.

**Validation before any scan starts** (this is F5's fix): missing, or present but not a
directory, or a string `utf8Path` refuses -> no scan, and the pane says which folder it could
not read. That is three of the brief's four music-root gate cases handled in one place.

### Root changed between runs

LIB-S2 already invalidates by construction - `scanCollection` keys revalidation on absolute
path, so a different root matches nothing and every record is re-read
(`src/LibraryScanner.cpp:197-199`). What is missing is **detecting it**, because
`enterLibrarySection` currently loads any index file that exists without comparing its root.

**Proposal:** on entry, after loading, compare `library_index_.root` against the resolved root;
if they differ, treat it as first-enable - scan once, with a message saying the music folder
changed rather than a bare progress line.

**This is a narrow, deliberate exception to "no scan on section entry", and I am flagging it as
one rather than smuggling it.** The rule's purpose is that entering must not revalidate every
time; this fires only when a config value actually changed, and it **settles** - the new index
records the new root, so the next entry matches and does nothing. Same shape as slice 3's
"guarded on no index FILE, never on an empty index" reasoning: the guard must be a condition the
scan itself resolves. If Dos would rather a changed root simply do nothing until `F12`, that is
also coherent and is one line different; I recommend detecting it, because a stale index over a
different folder shows the user tracks that are not where it says.

## 3. Rescan, and cancel

**`F12` inside `[Library]`** (F2), calling the same path first-enable uses: `start(resolved
root, index path)`, `lib_scan_running_ = true`, honest status. Available at **all three levels**,
which LIB-S4 already prepared for - scan completion routes through `populateLevel()` rather
than `showLibraryArtists()`, so a rescan finishing at level 3 relists level 3. That routing was
untestable when it shipped; this slice is what makes it reachable, so it now gets gate lines at
each level.

Rescan at level 2 or 3 re-queries from `lib_nav_.artist` / `lib_nav_.album`, which are held
strings - so a rescan under a live listing needs no invalidation, exactly as designed in slice 4.
If the artist or album has vanished from the new index, the existing honest empty state renders
("No albums for X").

**Cancel** is the F6 work. The scan already polls `progress_.cancel` per file and
`LibraryScanner::cancel()` already exists and is already correct; what is missing is a key and a
message. **Proposal: `Esc` while a library scan is running and `[Library]` is focused.** `Esc`
is the established dismiss/abort key throughout this UI, it needs no new binding, and it is
scoped by a condition that is only true during a scan. On cancel:

- `library_scanner_.cancel()`; the worker returns `completed = false` and **commits nothing** -
  the LIB-S2 invariant, unchanged and untouched by this slice
- slice 3's arm-a-retry logic becomes reachable *as designed*: the pane says the scan was
  cancelled and that opening again retries, `lib_scan_cancelled_` clears, the next entry scans
- a cancelled **rescan** leaves the previous `library.idx` byte-identical, because nothing is
  written unless the walk completed

Progress uses the existing atomics (`files_seen`), already rendered by slice 3 in the status
row; no new mechanism. The status text gains the cancel hint, because a cancellable operation
that does not say so is not offered.

## 3b. Library appends skip the tag re-read (ruled in from LIB-AA)

Added to this slice by Dos after LIB-AA measured the cost: a typical album append is
125-158 ms cold and **all** of it is the cold TagLib read (5.5-7.9 ms/track cold against
~0.2 ms warm - file I/O, not computation). The index already holds what that read
produces, so a library append re-reads tags it has in memory.

### Is it really indistinguishable? Probed, and yes - by construction

This only works if the index's fields come from the same read `populateMetadata` does.
Verified in the live tree rather than assumed:

| field | `populateMetadata` (`PlaylistManager.cpp`) | `readTags` (`LibraryScanner.cpp:63`) |
|---|---|---|
| open mode | `FileRef(..., true, AudioProperties::Fast)` | `FileRef(..., true, AudioProperties::Fast)` - identical |
| duration | `ap->lengthInSeconds()` | `ap->lengthInSeconds()` - identical |
| artist / title | `tag->artist()`, `tag->title()`, `.to8Bit(true)` | `tag->artist()`, `tag->title()` via `tagStr` = `toCString(true)` - the same UTF-8 |

The only difference is that `populateMetadata` applies `sanitizeForDisplay` and the index
deliberately stores raw text (sanitising on the way in would be lossy). So the append
sanitises at use, which is what every other library display path already does.

### Route: one new entry point plus one shared formatter

**`PlaylistManager::addIndexedTrack(path, artist, title, duration_sec)`** - mirrors
`addTrack` exactly (same http/CD rejections, **same dedup-by-path**, same
`rebuildShuffleOrder`) and differs only in not calling `populateMetadata`. The dedup
matters: LIB-AA's honest count depends on it, so the new path must keep it.

Building `PlaylistEntry` directly at the call site was the alternative and is rejected:
`entries_` is private, so it needs an entry point regardless, and `addCDTrack` - the one
existing method that sets the fields directly - **does not dedup**, so reusing it would
silently break LIB-AA's count.

**`PlaylistManager::displayTitleFor(path, artist, title)`**, a public static holding the
rule that is currently inline in `populateMetadata`: `artist + " - " + title` when both
are present, else `title`, else the path stem, with `sanitizeForDisplay` applied.
`populateMetadata` is refactored to call it, so **the two paths cannot format
differently** - the same argument as `libnav::albumTracks` in LIB-AA. Passing artist and
title rather than a finished string keeps the rule inside `PlaylistManager` where it
belongs.

That is the whole of the `PlaylistManager` change: one entry point, one extracted static.
Nothing else there is touched.

### Which wins when they disagree - THE INDEX

A file re-tagged since the last scan has index metadata that no longer matches its tags.
**The index wins, deliberately.** The library pane is *already* showing the index's title
and duration on that row, so an append that showed something else would make the pane and
the playlist disagree about the same row. What you saw is what you get.

Staleness is real and the remedy is a rescan - which is `F12`, added by this same slice.
That is the closure: the slice that starts sourcing appends from the index is the slice
that gives the user a way to refresh it.

### Which call sites

All three library appends: Enter at level 3, `a` at level 3, and `a` at level 2 (the album
append). The album append already holds records, so it passes them straight through. The
two single-row paths hold only a path, so they need the record: a small
`libraryTrackFor(path)` linear lookup over `library_index_.tracks`, **called once per
keypress and never per row or per frame**. It deliberately does not cache - a cached query
result surviving a frame is the staleness rule this campaign has kept for six slices - and
falls back to plain `addTrack` if the path is somehow not in the index.

**Everything outside the library is untouched:** the folder browser, `[FAVs]`, `[Recent]`,
radio and podcasts keep calling `addTrack` exactly as they do now.

## 4. Tests

The testable surface here is thin and I am not going to pretend otherwise. Config
load/save round-trip for the two new keys is worth a test; the toggle's effect on the pinned
list is worth a test; everything else is curses, COM, and a real filesystem.

- **`browser_pins_test`** (exists): extend for a filtered list - `[Library]` absent when the
  toggle is off, present and in the right position when on, `[Bookmarks]` unaffected, and the
  strict-weak-ordering property still holding over the filtered set. This is where F3's
  consolidation earns its keep: with one list, this test covers the browser *and* `[Drives]`.
- **`library_scan_test`** (exists): add a case for **an unreadable/nonexistent root** asserting
  `completed == false` and **that the pre-existing index file is untouched**. That is F5's
  behaviour pinned at the level where it is provable, and it complements the existing
  cancellation-invariant case.
- **Config round-trip**: `library` and `library_root` survive save/load, empty `library_root`
  is not written, and an absent key leaves the default. Which existing test links `Config.cpp`
  needs checking before I promise this - the M4aEncoder lesson is that a new dependency in a
  shared TU breaks every test that links it.

**Not tested:** the toggle's visual effect, the rescan key, cancel, progress rendering, and
every root-validation message. Gate.

## 5. Gate - eyes-on, both platforms

Windows `wingui` and Linux `ncursesw`.

**Toggle.** `library=0`: absent from the browser **and** from `[Drives]`; no `library.idx` is
created; no scan runs; `[Bookmarks]` and every other section still listed in `[Drives]` in the
unchanged order (F3's regression risk). `library=1`: present, in the pinned position after
`[Books]`, in both lists. Absent key behaves as the chosen default.

**Music root.** Unset -> the OS music folder. Set to a real directory -> scans that. Set to a
nonexistent one -> **names the folder it could not read, and does not say "cancelled"** (F5).
Set to empty -> same as unset. A **non-ASCII** root path, and a root containing a raw Latin-1
byte, both handled without an exit. Changed between runs -> full rescan, said out loud, and it
settles (the run after does not rescan).

**Rescan.** `F12` fires at level 1, level 2 and level 3; the listing is correct at each
afterwards, including a level-3 listing whose album is still present, and one whose album has
vanished from the new index. `F12` outside `[Library]` still refreshes `[Drives]` and does
nothing elsewhere.

**Cancel.** `Esc` during a scan stops it; the pane says so; **no index file is created** on a
first scan; on a rescan the previous `library.idx` is **byte-identical** afterwards (compare
before and after). Cancel at level 2 or 3, not just level 1. Then reopen and confirm the retry
arms exactly once.

**First run.** Honest progress with a rising count, a visible cancel hint, and the section
usable the moment it completes.

**Regression.** Every other section enters, draws, exits and plays. `[Drives]` hot-plug refresh
via `F12` unchanged. Folder browse, radio, podcasts, books, favourites, recent.

Machine: ctest both toolchains, `--no-tests=error`, currently 46/46 Windows and 47/47 Linux.
Brace-balance and scoped-diff audit before handoff. **Verification split as in LIB-S4/S5** - what
was run and green versus what is the gate's to confirm; no rendered behaviour asserted unless run
or the mechanism can be pointed at.

## 6. Files expected to change

`include/Config.h`, `src/Config.cpp` (two keys), `include/BrowserPins.h` (a filtered accessor),
`src/UIManager.cpp` (toggle filtering in both list builders, root resolution and validation,
`F12`, `Esc`, root-change detection, status text), `include/UIManager.h`,
`include/LibraryScanner.h` (**comment only** - F5's inaccurate `completed` comment),
`tests/browser_pins_test.cpp`, `tests/library_scan_test.cpp`, possibly a config test,
`tests/CMakeLists.txt` if a test is added, `CHANGELOG.md`, and this note as design-of-record.

**Not touched:** `LibraryIndex.h`, `LibraryNav.h`, `LibraryScanner.cpp` (logic), `PlaylistManager`,
`AudioManager`, the audio thread, `ar_crc`, the CD and rip paths, scrobblers, plugins, `Version.h`.
No index format or query-surface change. `Ctrl+T` untouched.
