# DESIGN NOTE - Library slice 11: multi-root library

**Scope ID:** LIB-S11. **Status:** BUILT. Greenlit with §0 ruled to keep `kFormatVersion = 1`.
Measurements and the two build-time findings are in §10.
**Probed against tip `db9f482`** on 2026-07-26, and measured against Dos's real `D:\Music`,
`library.idx` and `remoct.conf`.

Predecessors LIB-S1 to LIB-S10 and LIB-AA shipped and pushed.

**Three divergences from the brief. One of them dissolves the stop-and-raise in §1, so it leads.**

---

## 0. THE FORMAT CHANGE IS AVOIDABLE - §1 IS A SMALLER RULING THAN IT LOOKS

The brief says the index header carries the root, that a list of roots is a format change, and that
this invalidates every shipped `library.idx`. **The first is true, the second and third need not be.**

### What the format actually is

```
remoct-library-index<TAB>1          <- line 0: magic + version, exactly 2 fields
root<TAB>C:\Users\david\Music       <- line 1: the root, exactly 2 fields
<12 tab-separated fields>           <- lines 2+: one record each
```

`parseIndex` (`include/LibraryIndex.h`) requires line 0 to be magic + version and line 1 to be
`root` + value, then treats **every subsequent line that is not exactly `kFieldCount` (12) fields as
a skipped record** - counted honestly, never fatal.

### Therefore repeated `root` lines are compatible in BOTH directions

Write additional roots as further `root` lines after line 1:

```
remoct-library-index	1
root	C:\Users\david\Music
root	D:\Music
<records>
```

| direction | what happens |
|---|---|
| **New binary, existing shipped index** | Reads one `root` line, yields a list of one. **No rescan, no invalidation, nothing a user sees.** |
| **New binary, new index** | Reads the run of `root` lines. |
| **Old binary, new index** | Line 1 gives root #1. The second `root` line has 2 fields, not 12, so it is counted as one skipped record. **All records still load**, including the second root's. No crash, no loss. |

**So `kFormatVersion` does not have to move, and the upgrade path costs nothing.** The version exists
to mean "discard and rescan"; nothing here requires that.

**The one honest cost, stated:** a user who upgrades, adds `D:\Music`, then **downgrades** to an
older build gets a binary that loads all the records but believes in one root - and its next rescan
walks only `C:\` and drops the `D:\` records. Not a crash, and recoverable by upgrading and
rescanning. That is the entire downgrade exposure.

### The ruling I am asking for

**Recommendation: keep `kFormatVersion = 1` and use repeated `root` lines.** The alternative - bump
to 2 - buys cleaner semantics and costs **every existing user a full rescan on upgrade** (15.8 s for
2,155 files, measured in LIB-S2) to arrive at an index identical to the one they already had.

**If Dos prefers the version bump**, everything else in this note is unchanged; first run after
upgrading shows the LIB-S6 "Scanning the music folder... N files" row and then behaves normally.
Either way it is his call and this note does not assume it.

---

## 1. THE OTHER TWO DIVERGENCES

### 1a. The count is 619, not 618

`find` over `D:\Music` with **exactly `kAudioExts`** (`include/AudioExts.h` - the list the scanner
actually uses, shared with `PlaylistManager`) returns **619**. The brief's 618 comes from a debrief
that omitted `.mp4`. One file. Recorded because the gate counts it.

`C:\Users\david\Music` currently indexes 2,156 records, so a two-root library is **~2,775** - and
the brief's core claim holds exactly as stated: **619 tracks are invisible to browse and to `|`
search today.**

### 1b. `lib_status_` is NOT the bottom-left status line

The brief says rejection messages should use "the bottom-left status line ... the `lib_status_`
mechanism from LIB-S6". Those are two different things.

- **`lib_status_`** (`UIManager.h:714`) is pushed into `dir_display_` at `:9993`. It is a **row
  inside the library pane** - the "Scanning the music folder... N files" line. It is not on the
  command line and it has no colour or timeout.
- **The bottom-left yellow line with a ~5 s life is `status_msg_` + `status_msg_yellow_`**
  (`UIManager.h:646-648`), rendered `CP_MODE` when the flag is set (`:5391`) and expired at
  `> 60` ticks (`:1643`). It came from the F6-confirm slice, which is exactly where "yellow, in
  place, never a toast" was ruled.

**So the rejection messages use `status_msg_` with `status_msg_yellow_ = true`**, which is what the
brief describes behaviourally. `lib_status_` keeps its job: the in-pane scan state.

---

## 2. CONFIG - REPEATED KEYS, DEFAULT UNCHANGED

`std::vector<std::string> library_roots` beside the existing `library_root`.

**Parse:** each `library_root=` line appends, empties ignored. **Write:** one line per root, written
only when non-empty - `rec_dir`'s rule, which `library_root` already follows.

**Backward compatible by construction:** a config with one `library_root=` line yields a vector of
one, and a config with none yields an empty vector, which still means the OS music folder. **The
default is unchanged and an untouched config produces byte-identical behaviour.**

`libraryRoot()` becomes `libraryRoots()` returning `std::vector<std::string>`; empty vector -> the
memoized `CDRipper::musicRoot()` (still a function-local static, still one COM call).

---

## 3. PATH COMPARISON - ONE HELPER, REUSED

The brief asks whether LIB-S10's helper fits. **It does, and reusing it is the point.**
`libidx::detail::foldPathKey` already folds case and separators on Windows and is the identity on
Linux - which is exactly "case-insensitive on Windows, case-sensitive on Linux". It exists because
the stat join needed it; two path-equality rules that must agree is the defect class this campaign
keeps closing.

Two small additions beside it, both pure:

```cpp
// Trailing separators stripped, so "D:\Music\" and "D:\Music" are one root.
inline std::string normaliseRoot(const std::string& p);

// Is `path` inside `root` (or equal to it)? Folded compare, and the prefix must end
// ON A SEPARATOR BOUNDARY so "C:\Music" is NOT a parent of "C:\Musicology".
inline bool isPathUnder(const std::string& path, const std::string& root);
```

`isPathUnder` earns its keep three times: dedupe (§4), carry-forward for an offline root (§5), and
removing a root's records (§6).

**Roots from a config file are UNTRUSTED text.** Every filesystem touch goes through the existing
`utf8Path`/`toPath` route, never a bare `fs::path(std::string)` - the corrected CP1252 rule.

---

## 4. DEDUPE - THREE REJECTIONS, ONE MESSAGE MECHANISM

When adding root **N** against existing roots **E**:

| case | test | outcome |
|---|---|---|
| already a root | `foldPathKey(N) == foldPathKey(E)` | reject: "Folder is already in the library" |
| **child** of an existing root | `isPathUnder(N, E)` | reject: "Already covered by `<E>`" |
| **parent** of an existing root | `isPathUnder(E, N)` | **reject**, naming the root it would swallow |

All three are yellow `status_msg_`, ~5 s, and change nothing.

**The parent case is REJECTED, not absorbed** - the brief asks which, so: rejected. Absorbing means
silently deleting a root the user configured, and the one-keystroke undo for that is a 15.8 s
rescan. Rejecting with a message that names the conflict lets them remove the child first if that is
what they meant. It is also the only one of the three that could be a typo with a large blast radius
(`C:\` swallowing everything).

---

## 5. THE SCANNER - PER-ROOT SKIP, AND HOW THREE OUTCOMES COMPOSE

`scanCollection(const std::vector<std::string>& roots, previous, progress)`, walking each root in
config order and accumulating into one `out.index.tracks`.

**Deletion today is implicit** (`src/LibraryScanner.cpp:191-273`): the output holds only what the
walk found, so anything unreached is simply not carried forward. That is what makes an unplugged
drive dangerous, and it is the mechanism to modify.

**An unreadable root is SKIPPED, and its previous records are CARRIED FORWARD verbatim** - selected
out of `previous` with `isPathUnder`. `ScanOutcome` gains `std::vector<std::string> skipped_roots`
(a struct field, not a format change) so the UI can say which.

**The three outcomes, composed:**

| situation | `completed` | committed | deletions apply to |
|---|---|---|---|
| **cancelled** mid-walk | `false` | **nothing** - the LIB-S2 invariant, untouched | - |
| all roots walked | `true` | yes | everything |
| **some** roots unreadable | `true` | yes | **only the roots actually walked**; skipped roots' records survive byte-identical |
| **no** root readable | `false` | nothing | - |

**The last row is deliberate: with a single root, this is byte-for-byte today's behaviour.** One
unreadable root out of one means nothing was walked, `completed` stays false, and LIB-S6's
"Cannot read the music folder" path fires exactly as it does now. Multi-root adds a case; it does
not change the single-root one.

**Cancellation still beats everything.** A cancelled walk returns before any carry-forward decision,
so a partial multi-root scan cannot commit a half-deleted index - which is the same reason the
invariant existed for one root.

`counts.removed` is computed against **walked roots only**, or it would report the carried-forward
records as deletions and the toast would lie the way LIB-AA's append count would have.

---

## 6. THE KEY AND THE UI

**Sweep re-run against the tree** (`case` labels and `ch ==` comparisons across `handleInput`, since
the last list handed over was wrong about `?`). Free: `"` `#` `$` `&` `'` `(` `)` `:` `@` `^`.
`%` is now bound (LIB-S10). The brief's list is otherwise correct.

**Proposal: `@`** - "this location", the closest thing to a mnemonic for "make this folder a place
the library looks". Plain printable ASCII, so no cross-platform proof (the `|` precedent), and inert
outside the browser.

**Scope: the folder browser, on a directory row.** Not inside `[Library]` - there are no directories
there to make a root of. This is the mirror of `g`: a key that means something where it has something
to do.

**Adding.** Cursor on a directory, `@` -> a confirm popup naming the folder and saying a scan will
run. On confirm: append the root, save config, start a scan.

**Adding scans EVERYTHING, not just the new root, and that is the cheap answer** - because the
existing walk is already incremental. Measured in LIB-S2: a rescan of 2,155 unchanged files is
**251 ms** (mtime+size revalidation, no tag reads). So "rescan all roots" costs 251 ms plus the new
root's first read, and it is **one code path** rather than a second merge-only path that could
disagree with the first.

**Removing.** `@` on a row that is a root -> confirm popup -> the root is dropped from the config and
**its records are filtered out of the in-memory index immediately, and the index file rewritten.**
No walk needed: `isPathUnder` already identifies them. So removal is instant, and the popup says so
rather than leaving the user wondering whether a rescan is pending.

**Popup shape: reuse `drawConvertScope`'s box idiom**, as `drawPodcastPlayConflict` did - a titled
box, the folder name, `[Y]`/`[Esc]`. One new `UIOverlay` value. No new drawing primitive.

**Header.** Level 1 is unchanged - the key hints are what matter there and a root count is not
actionable. **The empty state changes**: "No audio found under `<root>`" becomes a form that names
all configured roots, because with three roots "under X" would be wrong about where it looked.

---

## 7. TESTS

**`library_scanner_test`** (already exercises a real temporary tree):
- two roots both readable: records from both, counts correct
- **one root unreadable: its previous records SURVIVE, the other root's deletions still apply** -
  the assertion that matters most
- no root readable: `completed == false`, nothing committed
- cancel mid-walk with two roots: `completed == false`
- a file present under two overlapping roots is indexed once (belt and braces behind §4's rejection)

**`library_index_test`:**
- `normaliseRoot` - trailing separators, both kinds, repeated
- `isPathUnder` - equal, child, grandchild, parent, sibling, and **`C:\Music` is NOT a parent of
  `C:\Musicology`** (the boundary case, by name)
- platform split: case-insensitive on Windows, case-sensitive on Linux, mirroring `foldPathKey`'s
  own test
- multi-root serialise/parse round-trip; **a one-root v1 file parses to a list of one**; an
  unknown extra header-shaped line is skipped and counted, not fatal

**Mutation-tested**, with the verify-it-landed step.

**Not machine-testable:** the popup, the key, the status-line colour and timeout, and anything
involving a physically absent drive. Gate.

---

## 8. GATE - EYES-ON, BOTH PLATFORMS

1. Two roots by hand in `remoct.conf`; both index. **`|` search finds a `D:\Music` track** - the
   619 are the check.
2. `@` on a directory: popup, confirm, scan runs, tracks appear.
3. `@` on a configured root: popup, confirm, **records gone immediately**, no rescan needed.
4. **Duplicate, nested child, and parent** each show the yellow bottom-left message and change
   nothing. `C:\Music` vs `C:\Musicology` are not treated as related.
5. **`D:` unplugged or renamed, then rescan: its 619 records SURVIVE, `C:\` deletions still work,
   nothing silently deleted.** The most important line here.
6. Cancel mid-scan with two roots: previous index byte-identical.
7. Single root configured: **everything behaves exactly as it does today**, including the unreadable
   -root message.
8. First run after upgrading: whatever §0 was ruled.
9. A non-ASCII root path, and a root typed with a trailing backslash.
10. Everything LIB-S3 to LIB-S10 still works; every other section enters, draws, exits, plays.

**Measured in the debrief:** scan time across both real roots, index size, and rescan time.

Machine: ctest both toolchains, `--no-tests=error`, currently **47/47 Windows, 48/48 Linux**.
Brace-balance and scoped-diff audit. **Verification split as in LIB-S4 through LIB-S10.**

---

## 9. FILES EXPECTED TO CHANGE

`include/LibraryIndex.h` (multi-root header write/parse, `normaliseRoot`, `isPathUnder`),
`include/LibraryScanner.h` + `src/LibraryScanner.cpp` (roots vector, per-root skip, carry-forward,
`skipped_roots`), `include/Config.h` + `src/Config.cpp` (repeated key parse/write),
`src/UIManager.cpp` + `include/UIManager.h` (the `@` key, the popup, root add/remove, the empty
state), `tests/library_index_test.cpp`, `tests/library_scanner_test.cpp`, `CHANGELOG.md`, and this
note as design-of-record.

**Not touched:** `LibraryNav.h`, `PaneScroll.h`, `BrowserPins.h`, `PlaylistManager`, `Version.h`, the
audio thread, `ar_crc`, the CD path, the rip path, the plugin ABI. No new dependency.

**Explicitly out, per the brief:** level-1 append (**LIB-S8's cut**, and this slice building a
confirm popup does not reopen it - Dos rules it in or it stays cut), the per-handler scroll calls
(**LIB-S12**), `Config` stat-key normalisation (**LIB-S13**), tag editing on browser rows
(**LIB-S14**). No watcher, no auto-rescan, no removable-media detection beyond "not readable now".

---

## 10. WHAT WAS MEASURED, AND TWO THINGS THE BUILD FOUND

### Measured on both real roots, timing the shipped `scanCollection`

| | |
|---|---|
| first scan, `C:\Users\david\Music` + `D:\Music` | **14.8 s COLD**, 649 ms warm |
| records | **2,775** = 2,156 (C:) + **619** (D:) |
| index size | **504,306 bytes (0.48 MB)** |
| rescan, nothing changed | **220 ms**, 2,775 unchanged, 0 re-read |
| rescan with a root offline | **221 ms** |

The 619 matches the `find` count exactly, which is the check that the scanner and the survey were
counting the same thing. The **cold/warm gap is 23x and it is file I/O, not computation** - the same
shape LIB-AA measured for the album append, and the reason the first-scan figure quoted anywhere
must say which it is. 14.8 s for 2,775 files is consistent with LIB-S2's 15.8 s for 2,155.

**The offline case, simulated honestly and measured:** a third root scanned in (25 records), then
its directory deleted, then a rescan with the root still configured.

```
OFFLINE ROOT rescan    : completed=1 skipped=1
  records under the vanished root SURVIVE: 25 (was 25)
  D:\Music untouched                     : 619 (was 619)
  counts.removed = 0
```

### Finding 1 - MY FIRST PROBE SIMULATED THE WRONG SCENARIO, AND SAID SO LOUDLY

The first version pointed the scan at `D:\Music-NOT-THERE` while the records were under `D:\Music`,
and printed `records after a scan with it missing: 0` with `counts.removed = 619`. That reads as the
exact disaster this slice exists to prevent.

**It was the probe, not the code.** Skipping a root carries forward records *under that root*, and
the root I skipped had never held any - I had simulated **replacing** a root, not a drive going
offline. Replacing is a different case and dropping the old root's records is correct for it: that
root is no longer configured.

The corrected probe keeps the root string identical and removes the directory it names, which is the
real scenario, and the records survive. **Both cases are now in the probe output** so the distinction
is visible rather than something to re-derive. This is LIB-S7's lesson wearing a different hat: a
probe that models a different situation measures a different program.

### Finding 2 - THE LINUX GATE CAUGHT A REAL BUG IN `normaliseRoot`

The first cut stripped both `/` and `\` as trailing separators on every platform. **On Linux a
backslash is an ordinary filename character**, so a directory really can be called `weird\` and
stripping it names something else - or nothing. `normaliseRoot("C:\\")` returned `"C:"` there.

Fixed with a platform-split `isSep()`, which `isPathUnder` now uses too. Two test assertions moved
into the Windows branch for the same reason: a Windows-shaped path on Linux is one filename with no
separators in it, so it is correctly under nothing, and asserting otherwise was asserting the wrong
thing rather than finding a bug. **Neither platform's behaviour was guessed at; both are asserted.**

### Mutation-tested, each verified to have LANDED

| mutation | caught by |
|---|---|
| carry-forward removed | `the offline root's records SURVIVE, got 0` |
| `isPathUnder` boundary check removed | `C:\Music is NOT a parent of C:\Musicology` (+ the Linux twin) |
| a walked root never counted | 5 failures incl. `removed=0` and `completed` |

### Finding 3 - THE HARDWARE GATE FAILED ON PANE OWNERSHIP, AND THE DESIGN NEVER SAID

**Symptom:** `@` in the folder browser wiped the browse pane. Header still read `Dir: D:\`, the
listing was gone, and the pane held `[Back]` plus `Added a library folder - scanning...` - library
grammar in a pane the library does not own - until Esc.

**Cause, one line:** `startLibraryScan()` ended with an unconditional `populateLevel()`. **Every
caller of that function was inside `[Library]` until this slice added `@`**, so the call had always
been correct and nothing about it was flagged. `populateLevel()` clears `dir_entries_` and pushes
the library's `[Back]` row and `lib_status_`, so calling it from the folder browser destroyed the
directory listing. The header still said `Dir:` because `drawDirBrowser` reads `in_library_`, which
was correctly false - the pane and its header had different ideas about what was being shown.

**And it could not recover**, which is why it persisted: the COMPLETION path's `populateLevel()` was
already correctly guarded on `in_library_`, so nothing ever put the listing back.

**The sharpest part: this is the §1b correction not being applied to its own consequence.** This
note established that `lib_status_` is a row inside the library pane rather than the bottom-left
line. Having written that down, the slice then wrote `lib_status_` into a pane the library was not
showing. Knowing which mechanism owns which surface did not stop it being used on the wrong one.

**Fixed - the rule is now stated and enforced at every site:**

- `startLibraryScan` and `cancelLibraryScan` repopulate **only when `in_library_`**; otherwise the
  message goes to the bottom-left yellow line and the pane is untouched.
- `pollLibraryScan` reports progress on that line when the library pane is not showing, **refreshed
  every poll on purpose** - `status_msg_` expires after ~60 ticks (~5 s) and a cold scan takes 15, so
  a set-once message would vanish two-thirds through and read as a finished scan.
- Completion says so on that line too. Without it the progress line would simply stop updating and
  expire, which reads as a scan that died rather than one that finished.
- **Esc is no longer gated on `in_library_`**, only on `lib_scan_running_`. A scan startable from the
  folder browser must be cancellable from there, or the message saying "Esc to cancel" is produced by
  a key that does nothing.
- Every other `populateLevel()`/`showLibraryArtists()` call site audited: all are either inside the
  library Enter handler, inside `libraryAscend` (reached only through `sectionAscend`'s `in_library_`
  test), or inside `enterLibrarySection`, which sets the flag itself.

**Whose gap:** the greenlight approved "adding a root rescans everything" and neither side asked what
the browser shows for the 15 seconds that takes. It is a hole in the design, not a misreading of it -
recorded here because the next slice that starts background work from a new surface will have the
same hole unless the rule is written down. **The rule: a background scan reports on the bottom-left
line; only the pane the user is actually looking at may be repopulated.**

### Gates

**Windows 47/47, Linux 48/48**, `--no-tests=error`. `library_index_test` **340 checks**,
`library_scanner_test` **143 checks**, `library_level_test` 161.

**`skipped_records` is NOT user-visible anywhere** (§0 condition 2, answered): both `loadIndexFile`
call sites omit the optional out-parameter, so an older build reading a multi-root index counts the
extra `root` lines and displays nothing about them. Zero cosmetic noise on the downgrade path.
