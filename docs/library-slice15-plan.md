# LIB-S15 - silent dupe denial, library to playlist

**Design note. Not greenlit. Untracked until it is.**

Branch `experimental/win-pdcurses`, tip `f142ce3`. Every anchor below was read on the live tree
today, not carried in from a debrief.

---

## 0. Disconfirmation report

The brief asked for this section first, and it has three entries. Two confirm it, one refutes it.

### 0.1 CONFIRMED - the hypothesis is right, and it is measurable in Dos's live config

The brief offered the case-spelling explanation as a hypothesis "so it isn't rediscovered - not a
diagnosis". It is correct, and it is not merely reachable in theory: **Dos's real playlist already
contains five duplicate pairs right now**, before this slice adds anything.

`%APPDATA%\RE-MOCT\remoct.conf`, 59 `track=` lines, folded case-and-separator: **54 distinct files,
five folded groups holding two raw lines each.**

| both lines name this file | spelling A | spelling B |
|---|---|---|
| Shirelles, The - I Met Him on a Sunday | `C:\Users\david\Music\...` | `c:\users\david\Music\...` |
| Vampire Weekend - Prep-School Gangsters | `C:\Users\...` | `c:\users\...` |
| Vampire Weekend - A-Punk | `C:\Users\...` | `c:\users\...` |
| Arrested Development - People Everyday (16) | `C:\Users\...` | `c:\users\...` |
| .38 Special - Hold on Loosely | `C:\Users\...` | `c:\users\...` |

`recent=` has one more such pair (50 lines, 49 distinct). `fav=` and `book=` are clean.

The variance is exactly what LIB-S13 measured: **drive letter and `Users` lowercased, `Music` and
everything below it correctly cased.** That shape is a fingerprint, and it names its source - see
§1.3.

### 0.1a SHOWN, on Dos's challenge - all five are case-only, and format variants are a REAL and SEPARATE thing

Dos's read was that what looks duplicated is the same track in several formats. **He is right that
format variants are in there, and right that it was worth checking. He is not right about these
five.** Both facts are measured, and they are disjoint sets.

**The five pairs, verbatim, with the differing bytes named:**

```
PAIR 1  conf line  77  C:\Users\david\Music\Shirelles, The\16 Greatest Hits\03 Shirelles, The - I Met Him on a Sunday.flac
        conf line  93  c:\users\david\Music\Shirelles, The\16 Greatest Hits\03 Shirelles, The - I Met Him on a Sunday.flac
        99 vs 99 bytes; both .flac; differ at bytes 0 and 3 only: 'C'/'c', 'U'/'u'

PAIR 2  conf line  87  C:\Users\david\Music\Vampire Weekend\Only God Was Above Us\05 Vampire Weekend - Prep-School Gangsters.flac
        conf line 107  c:\users\david\Music\Vampire Weekend\Only God Was Above Us\05 Vampire Weekend - Prep-School Gangsters.flac
        106 vs 106 bytes; both .flac; differ at bytes 0 and 3 only: 'C'/'c', 'U'/'u'

PAIR 3  conf line  89  C:\Users\david\Music\Vampire Weekend\Vampire Weekend\03 Vampire Weekend - A-Punk.flac
        conf line 120  c:\users\david\Music\Vampire Weekend\Vampire Weekend\03 Vampire Weekend - A-Punk.flac
        85 vs 85 bytes; both .flac; differ at bytes 0 and 3 only: 'C'/'c', 'U'/'u'

PAIR 4  conf line  90  C:\Users\david\Music\Arrested Development\3 Years, 5 Months & 2 Days in the Life Of\16 Arrested Development - People Everyday.flac
        conf line 122  c:\users\david\Music\Arrested Development\3 Years, 5 Months & 2 Days in the Life Of\16 Arrested Development - People Everyday.flac
        130 vs 130 bytes; both .flac; differ at bytes 0 and 3 only: 'C'/'c', 'U'/'u'

PAIR 5  conf line  95  c:\users\david\Music\38 Special\Flashback; The Best of .38 Special\02 .38 Special - Hold on Loosely.flac
        conf line 124  C:\Users\david\Music\38 Special\Flashback; The Best of .38 Special\02 .38 Special - Hold on Loosely.flac
        104 vs 104 bytes; both .flac; differ at bytes 0 and 3 only: 'c'/'C', 'u'/'U'
```

Every pair is **identical length**, **identical extension**, and differs at **exactly two byte
positions**, both of which are an ASCII letter-case flip in `C:\Users`. No extension variant is
among them. The slice proceeds.

**And separately - the format variants Dos was thinking of are genuinely there.** Grouping the same
playlist by folded stem *ignoring* extension finds two groups:

```
GROUP 1  C:\Users\david\Music\Joan Osborne\Relish\06 Joan Osborne - One of Us.mp3
         c:\users\david\Music\Joan Osborne\Relish\06 Joan Osborne - One of Us.flac
GROUP 2  c:\users\david\Music\re-moct\Jermaine Stewart - Frantic Romantic (1986)\01 - We Don't Have to Take Our Clothes Off.flac
         c:\users\david\Music\re-moct\Jermaine Stewart - Frantic Romantic (1986)\01 - We Don't Have to Take Our Clothes Off.opus
```

**Under `foldPathKey` both groups stay 2 of 2 - not merged.** Zero overlap with the five pairs.

**Group 1 is the trap, and it is the best gate target in the collection**: `.mp3` under `C:\Users`
and `.flac` under `c:\users`. It differs in **case AND extension at the same time**. Any rule that
normalises a path and then compares stems - the shape someone reaches for when "the same track keeps
showing up" - merges those two and deletes a file Dos deliberately keeps. `foldPathKey` does not,
because it folds bytes and never parses a path into parts.

### 0.1b The constraint, planted against the real collection rather than inferred

The brief requires that this be gated deliberately, not reasoned from "the helper doesn't strip
extensions". Measured over `library.idx` - 2,775 records:

**349 groups of same-directory, same-stem, multiple-format files.** Every one stays fully distinct
under `foldPathKey`. Three-format sets are routine:

| track | formats | all exist on disk |
|---|---|---|
| `Crystals, The\...\01 Crystals, The - Da Doo Ron Ron` | `.flac` `.m4a` `.opus` | yes |
| `re-moct\Ace of Base - The Sign (1993)\04 - The Sign` | `.flac` `.mp3` `.opus` | yes |
| `re-moct\Beastie Boys - Hello Nasty (1998)\02 - The Move` | `.flac` `.mp3` `.opus` | yes |

This is not an incidental property of the collection - it is **12.6% of the index**, and the
`re-moct\` tree is full of it because that is where rips and conversions land. A rule that merged
format copies would not be a subtle regression here; it would eat hundreds of files.

The gate (§9) and the test suite (§8) both name specific multi-format tracks, and **the assertion is
that all three copies are added, not that the helper happens not to strip extensions.**

### 0.2 CONFIRMED - `addTrack` does dedup, and the compare is the only thing wrong

`PlaylistManager::addTrack` ([PlaylistManager.cpp:176-177](src/PlaylistManager.cpp#L176-L177)):

```cpp
for (std::size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].path == path) return i;
```

Byte-exact. LIB-AA's finding is accurate and its "adds 15, not 18" behaviour is real. **The dedup is
not missing anywhere. It is present at four sites and byte-exact at all four** (§2).

### 0.3 REFUTED - `q` does not reach the playlist

The brief says "Enter, `a`, `q`, and LIB-AA's whole-album append all reach the playlist." **`q` does
not.** [UIManager.cpp:7719](src/UIManager.cpp#L7719) builds a `PlaylistEntry` and calls
`playlist_.queueAdd(qe)`, which is
[PlaylistManager.cpp:331-332](src/PlaylistManager.cpp#L331-L332):

```cpp
void PlaylistManager::queueAdd(const PlaylistEntry& e) {
    play_queue_.push_back(e);
}
```

`play_queue_` is a **separate `std::deque<PlaylistEntry>`**
([PlaylistManager.h:202](include/PlaylistManager.h#L202)), not `entries_`. It has **no dedup at
all**, and never has had - from any route. Queueing the same track twice from the folder browser
today gives two queue rows, and so does queueing it twice from the library. The library is not
behaving differently from other routes here, so this is not the reported defect and it is not
something a path-compare fix touches.

Whether it *should* dedup is a separate question the brief anticipated. See §6 - **raised, not
built.**

---

## 1. The defect, end to end

### 1.1 What library rows carry

`library.idx` field 1, read today: every record is `C:\Users\david\Music\...` or `D:\Music\...` -
the configured root's spelling, extended by the scanner's own `directory_iterator` walk. The roots
are `library_root=C:\Users\david\Music` and `library_root=D:\Music`.

So **a library row's path spelling is fixed by the root string**, and it is the same on every
keypress, forever.

### 1.2 What the folder browser carries

[UIManager.cpp:9352](src/UIManager.cpp#L9352): `fs::path full = fs::path(current_dir_) / name;` then
`playlist_.addTrack(full.string())`. The spelling of the result is **whatever `current_dir_`
happens to be**, with an OS-cased leaf appended.

### 1.3 Where the second spelling comes from

`gotoClose` ([UIManager.cpp:8552-8553](src/UIManager.cpp#L8552-L8553)):

```cpp
} else if (fs::exists(target) && fs::is_directory(target)) {
    current_dir_   = target;
```

`target` is **the typed text, verbatim**. `fs::exists` and `fs::is_directory` are case-insensitive
on Windows, so typing `c:\users\david\music` succeeds and `current_dir_` keeps the typed case for
the rest of that navigation. Tab-completion does not rescue it either: `gotoGetMatches` builds each
candidate as `de.path().string()` over `fs::directory_iterator(search_dir)`, and `search_dir` is the
typed prefix - so **only the completed leaf is OS-cased and the typed prefix survives**. Type
`c:\users\david\` then Tab onto `Music`, and you get `c:\users\david\Music` - which is the exact
fingerprint in §0.1, `Music` and below correctly cased and the typed part not.

**Labelled as inference, not measurement:** I traced the goto bar as the mechanism by reading it; I
did not watch Dos type it. What *is* measured is that the two spellings both exist in his config and
that the library can only ever produce one of them. The fix does not depend on which route produced
the other one.

### 1.4 The failure

1. A file enters the playlist as `c:\users\david\Music\X\Y.flac` (browser, after a goto).
2. Dos finds the same file in `[Library]` and presses Enter. The library hands over
   `C:\Users\david\Music\X\Y.flac`.
3. `entries_[i].path == path` is false for every entry.
4. A second row appears for one file.

**This is general, not library-specific.** All five duplicate pairs in §0.1 are `C:` versus `c:`,
and the library only emits `C:`, so at least some of them were browser-against-browser. A
library-only fix would leave the defect everywhere else - which the brief correctly calls the shape
this campaign keeps closing.

---

## 2. Where the dedup actually is - four sites, one rule between them

| # | site | what it is |
|---|---|---|
| 1 | [`addTrack`:177](src/PlaylistManager.cpp#L177) | every folder-browser, `[FAVs]`, `[Recent]`, `[Books]`, M3U-load, session-restore and chapter add |
| 2 | [`addIndexedTrack`:115](src/PlaylistManager.cpp#L115) | every library add - Enter, `a`, and album append |
| 3 | [`addDirectoryAsync` prefilter:689-690](src/PlaylistManager.cpp#L689-L690) | `a` on a directory, before the work queue |
| 4 | [`drainPending`:745-746](src/PlaylistManager.cpp#L745-L746) | the same walk again, on the UI thread as results land |

Four hand-written copies of `e.path == p`. That is the same shape as the four scroll-math copies
LIB-S12 deleted and the two info-pane subject copies LIB-S10 collapsed: **not four bugs, one rule
written four times, and the campaign's standing answer is to write it once.**

`addStream`:146 is a **fifth** compare and is deliberately excluded - see §5.

---

## 3. The fix

### 3.1 One predicate, private to `PlaylistManager`

```cpp
// include/PlaylistManager.h - private
// The one answer to "is this file already in the playlist", and the only place
// the question is asked. Returns the existing row's index, or npos.
std::size_t indexOfPath(const std::string& path) const;
```

```cpp
// src/PlaylistManager.cpp
std::size_t PlaylistManager::indexOfPath(const std::string& path) const {
    const std::string key = libidx::detail::foldPathKey(path);   // folded ONCE
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (libidx::detail::foldPathKey(entries_[i].path) == key) return i;
    return std::string::npos;
}
```

All four sites become a call to it. **Fifth use of `foldPathKey`, no sixth path-equality rule** -
and it *removes* four ad-hoc comparisons rather than adding to the count.

### 3.2 Why `foldPathKey` is the right helper here and not merely the available one

Its own header comment says it: *"NTFS is case-insensitive, so two paths differing only in case name
one file and folding is what 'same file' MEANS there. Linux paths are case-sensitive and two such
paths may be two different files."* That is precisely the question `addTrack` is asking.

**On Linux `foldPathKey` is the identity function**, so this slice is a **behavioural no-op on
Linux** - byte-for-byte the same program. Two genuinely different case-differing files can both be
added, because on Linux they *are* two files. The gate item exists to prove it, not to discover it.

### 3.3 Dependency check - no new link dependency

`PlaylistManager.cpp` gains `#include "LibraryIndex.h"`. That header is **pure and std-only** -
`<algorithm> <cctype> <cstddef> <cstdint> <string> <unordered_map> <unordered_set> <vector>` and
nothing else, no filesystem, no TagLib, no seam. So this is a header include in one `.cpp`, not a
new library on the link line.

That distinction is the M4aEncoder / secret-at-rest lesson, and it is why the include goes in the
**`.cpp` and not the header**: `PlaylistManager.h` is included widely, and pushing a new include
through it for a private helper is cost with no benefit. The four tests that already compile
`PlaylistManager.cpp` (`playlist_encoding_test`, `library_scanner_test`, `next_resolver_test`,
`xfade_handoff_test`) already have `-Iinclude`, so **no test's link line changes**.

### 3.4 The one thing to measure rather than assume

Sites 3 and 4 are the only **n x m** ones: `addDirectoryAsync` folds every candidate against every
existing entry. A 2,000-file directory into a 500-row playlist is a million folds, each allocating a
string. Sites 1 and 2 are trivially small (Dos's playlist is 59 rows).

**I will measure `a` on the largest directory in the collection before deciding.** If it is under
~10 ms the simple form ships unchanged. If it is not, sites 3 and 4 - and only those two - build one
`unordered_set` of folded keys per batch instead. The number goes in the debrief either way; I am
not guessing at it here and I am not pre-optimising on a guess either.

---

## 4. Scope - which operations, and what each one does after

| operation | route | after the fix |
|---|---|---|
| Enter on a library track | `addIndexedTrack` | already-present: no row added, **nothing said**, `selectAt` lands on the existing row and plays it |
| `a` on a library track | `addIndexedTrack` | already-present: no row added, **nothing said** |
| `a` on a library album (LIB-AA) | `addIndexedTrack` x N | adds only the missing ones; the count is still `size()` before-vs-after, so it stays honest |
| Enter / `a` in the folder browser | `addTrack` | same silent denial - **this is where the defect also lived** |
| `[FAVs]`, `[Recent]`, `[Books]` Enter | `addTrack` | same |
| M3U / PLS / XSPF load | `addTrack` | a file listed under two spellings loads once |
| session restore | `addTrack` | **see §5.1 - this one needs a ruling** |
| `a` on a directory | sites 3 + 4 | same |
| `q` | `queueAdd` | **unchanged** - not a playlist add, see §0.3 and §6 |

**Silence is already the shape.** None of the single-track paths says anything on a duplicate today;
`addTrack` quietly returns the existing index and Enter plays it. This slice does not add silence,
it makes the existing silence *correct*. No status line, no toast, no popup is added anywhere.

**LIB-AA's album toast is not the per-track denial and stays exactly as written.** It reports on the
*album operation* - "Added 15 tracks", "Already in the playlist", "…3 missing" - and that is a
different thing from announcing each denied track. It is measured before-vs-after
([UIManager.cpp:10429, 10441](src/UIManager.cpp#L10429)) precisely so it stays true when more rows
get denied, which is what this slice causes. Nothing in it changes.

---

## 5. Two consequences that are Dos's call, not mine

### 5.1 THE ONE THAT MATTERS - session restore will drop his five duplicate rows

`main.cpp:227-228` restores the saved playlist by calling `playlist.addTrack(path)` in a loop. With
the fix, **the five case-variant pairs in his live config fold on the way in, and his playlist comes
back with 54 rows instead of 59.** The next `config.save()` writes 54.

The brief says *"no dedup of what is already in the playlist. This denies new additions; it does not
clean up an existing list."* Restore is literally an add path, so the letter of the fix and the
letter of that constraint disagree on this one case. I am not resolving that quietly.

**(a) Let restore fold - recommended.** One rule everywhere, no exemption to explain, and his
playlist self-heals to 54 correct rows the next time he starts the app. The rows removed are, by
measurement, second copies of files already in the list - nothing is lost that is not already there.

**(b) Exempt restore.** Keep a byte-exact add for the restore loop so an existing saved list is
returned untouched. Costs a second code path in `PlaylistManager` whose whole purpose is to preserve
duplicates, and the five stay forever.

I recommend **(a)**, and it is a one-line difference either way. **Say which.**

Side effect of (a), stated so it is not a surprise: `config.playlist_current` is a row *index*, and
five rows vanishing above it means the restored cursor can land on a neighbouring track once, on the
first start after this ships. `selectAt` clamps, so nothing breaks; it is one cosmetic off-by-a-few
on one startup.

### 5.2 The stored spelling is not changed - and does not need to be

The brief asks whether LIB-S13's stat-key normalisation matters here. **It does not, and
deliberately so.** S13 rewrote persisted keys because stats are a *map* - two keys meant two
counters and the data was genuinely wrong on disk. Playlist entries are a *list of files to play*,
and both spellings open the same file on Windows. So this is a **read-side fold**, exactly like
LIB-S10's stat join before S13, and the entry keeps whatever spelling it was added with.

No migration, no `.bak`, no touching a file the user did not ask us to touch. That was S13's
justification for backing up `remoct.conf`, and its absence here is the reason none is needed.

**What the fold does and does not cover**, stated so nobody expects more: case and separator, which
is what was measured. Not `..` segments, not trailing separators, not `\\?\` prefixes, not 8.3 short
names, not symlink or junction resolution. Those are a different rule and there is no evidence any
of them occurs in the wild here. **No sixth rule.**

**And above all, NOT the extension.** `foldPathKey` folds bytes; it never splits a path into stem
and extension, so `.flac`, `.opus`, `.mp3` and `.m4a` copies of one track are four different strings
and stay four playlist rows. **That is not a happy accident of the helper and this note does not
rest on it being one** - §0.1a and §0.1b measure it against 349 real multi-format groups, §8 blocks
7-8 assert it, and §9 step 3 puts it in front of Dos on a track he actually owns in three formats.
**A rule that merged format copies would be wrong, and this one cannot express such a merge.**

---

## 6. RAISED, not built - should `q` dedup?

The brief asked for a raise rather than a toggle if a legitimate reason for the same file twice
turned up, and the queue is that case.

`play_queue_` has never deduped, from any route. And unlike the playlist, **there is an obvious
honest reason to want a file in the queue twice**: the queue is a play order, not a set, so "play
this next, and again after that" is a coherent thing to ask for. The playlist is a collection where
a second copy is meaningless; the queue is a sequence where it is not.

So: **no change proposed**, the library behaves identically to every other route here, and I am
flagging it rather than deciding it. If Dos wants queue dedup it is its own small slice with its own
gate, and it should be argued on what a queue *is*, not folded into a path-compare fix.

---

## 7. Not touched

`AudioManager`, the audio thread, `ar_crc`, the CD path, the rip path, `ConvertJob`, the scrobblers,
`Version.h`, `Config` (no new key, no format change, no migration), `LibraryIndex.h` itself
(`foldPathKey` is *used*, not modified), `LibraryScanner`, every `libnav` function, and every
`[Library]` populate function. `Ctrl+T` stays Classic/Awesome only.

`addStream`:146 keeps its byte-exact URL compare. A URL is not a path: its host is
case-insensitive but its path component is not, and `foldPathKey` would lowercase the whole thing
and normalise `/` to `\`, which for a URL is simply wrong. `addTrack` short-circuits `http://` and
`https://` to `addStream` **before** the dedup loop, so no URL ever reaches the folded compare.
`addCDTrack` is likewise untouched and unreachable from it (`isCDTrackPath` rejects first).

**Files expected to change: `include/PlaylistManager.h`, `src/PlaylistManager.cpp`,
`tests/CMakeLists.txt`, one new test file, `CHANGELOG.md`, and this document.** Anything beyond
that list is a divergence and gets reported, not absorbed.

---

## 8. Tests

New `tests/playlist_dedup_test.cpp`, linking `PlaylistManager.cpp` the way `next_resolver_test`
already does ([tests/CMakeLists.txt:324-332](tests/CMakeLists.txt#L324-L332)) - device-free, both
CI jobs. Paths need not exist on disk; `addTrack`'s TagLib probe fails soft, which
`next_resolver_test` already relies on and states.

Blocks:

1. **Byte-identical re-add** returns the existing index and does not grow the list. The behaviour
   LIB-AA depends on, pinned so this slice cannot break it.
2. **Case-variant re-add**, `C:\A\b.flac` then `c:\a\B.flac`. **On Windows: one row.** On Linux:
   **two rows** - and the assertion is written that way per platform, not `#ifdef`-ed out, because
   "Linux must still add both" is the constraint and an untested constraint is a wish. Mirrors
   `library_index_test`'s own `foldPathKey` block ([:783-792](tests/library_index_test.cpp#L783)).
3. **Separator variant**, `C:\A\b.flac` versus `C:/A/b.flac` - one row on Windows, two on Linux.
4. **Album-append accounting**: seed three of eighteen, add all eighteen via `addIndexedTrack`,
   assert `size()` grew by exactly fifteen. LIB-AA's count, pinned at the `PlaylistManager` level
   where it is testable without curses.
5. **URLs are not folded**: two URLs differing only in path case stay two entries, on both
   platforms. The guard on §7's exclusion.
6. **`addDirectoryAsync`/`drainPending`** reject a case variant of an entry already present.
7. **FORMAT VARIANTS ARE THREE FILES.** Add `.../04 - The Sign.flac`, `.opus` and `.mp3` - the real
   trio from §0.1b. Assert **three rows**, on **both** platforms. Then the same three via
   `addIndexedTrack`, since the library path is the one that reaches them from an album listing.
8. **The trap case, from Dos's live playlist.** `C:\...\06 Joan Osborne - One of Us.mp3` then
   `c:\...\06 Joan Osborne - One of Us.flac` - differing in case **and** extension at once.
   **Two rows on Windows.** This is the block that fails if anyone ever "improves" `indexOfPath`
   into a stem compare, and it is written against the exact bytes measured in his config.

**Mutation-tested, and the mutation verified to have landed** before either result is believed -
revert `indexOfPath` to `==` and confirm blocks 2, 3 and 6 fail on Windows. That step has now paid
three times, and S13's silent no-op `perl` regex is why it is written down rather than remembered.

---

## 9. Gate - eyes-on, both platforms

The reproduction needs the two spellings to exist, so it starts by making one:

1. `g`, type `c:\users\david\music` in lower case, Enter. Browse to a track and add it. Then find
   **the same track** in `[Library]` and press Enter - **no second row, nothing said**, and the
   cursor lands on the row that was already there.
2. The same, with `a`. And again with the two routes reversed - library first, then the browser.
3. **FORMAT VARIANTS - the step Dos asked for, on files he owns.** In
   `C:\Users\david\Music\re-moct\Ace of Base - The Sign (1993)\`, add `04 - The Sign` as `.flac`,
   then `.mp3`, then `.opus`. **All three land. Three rows, three tracks, nothing denied.** Repeat
   from `[Library]`, where the album listing shows all three. Then the three-format
   `01 Crystals, The - Da Doo Ron Ron` (`.flac` / `.m4a` / `.opus`) the same way.
   **If any format copy is denied, the slice is wrong and stops.**
4. `a` on an album where some tracks are already present under either spelling: **only the missing
   ones are added and the count is right** - and an album holding several formats of one track
   still contributes all of them.
5. `q` on a library track behaves exactly as it does today (it queues; queueing twice still gives
   two queue rows - unchanged, see §6).
6. Adding a genuinely new track still works from library, browser, `[FAVs]`, `[Recent]`, `[Books]`.
7. **Restart.** Confirm the playlist comes back as expected under whichever §5.1 ruling was made.
8. **Linux:** two genuinely different case-differing files in one directory can **both** be added,
   and the format-variant trio still lands as three rows there too.
9. `[Radio]` still adds and re-selects stations; an M3U still loads; `[Podcasts]` unaffected.
10. LIB-S3 through S14 still work - `[Library]` browse, `|` search, `%` stats, `g` genres, `@` add
   root, F12 rescan, tag editing.

**Verification split, as in LIB-S4 through S14:** the debrief will state separately what was run and
green (ctest both toolchains, the new test, the mutation, and the `addDirectoryAsync` timing from
§3.4) versus what is the hardware gate's to confirm (everything in this section).

Brace-balance and scoped-diff audit before handoff.

---

## 10a. BUILT - measured results

Greenlit and implemented. Numbers recorded here so nobody re-derives them.

**Gates: Windows 49/49, Linux 50/50** (48/48 and 49/49 before, plus `playlist_dedup_test`).

**§3.4 resolved - the simple form ships, no key set.** `addDirectoryAsync` over
`C:\Users\david\Music\re-moct`, the largest directory in the collection (1,069 files, 693 audio
candidates). Walk subtracted, so this is the prefilter alone:

| playlist rows | folded (shipped) | byte-exact (before) | added by this slice |
|---|---|---|---|
| **59 - Dos's real size** | 3.79 ms | 0.90 ms | **+2.9 ms** |
| 500 | 20.41 ms | 1.72 ms | +18.7 ms |
| 2,000 | 83.67 ms | ~0 ms | +84 ms |

At the size that exists, `a` on the biggest directory in the collection costs **under 4 ms** of
membership testing - against a 48 ms directory walk it is not the expensive part. The hoisted
`unordered_set` is not built: at 2,000 rows it would matter, but no playlist here is within a factor
of thirty of that, and adding it now would be speculative hardening of a working path - the podcast
slice-5 lesson.

**Mutation testing - TWO mutations, each verified to have landed before its result was believed.**

| mutation | what it models | result |
|---|---|---|
| **M1** - `indexOfPath` back to a byte compare | the defect itself | Windows: blocks 2, 3, 9 fail. Linux: passes, correctly - the fold is the identity there, so there is nothing to mutate |
| **M2** - `indexOfPath` compares `path_stem`, ignoring extension | **the forbidden "improvement"** | **Windows 5 failures, Linux 8** - blocks 7 and 8 fail on both, including the live-config Joan Osborne pair |

**M2 is the one that matters**, and running it is what turned "foldPathKey doesn't strip extensions"
from an inference into a gated fact. M1 alone would have left blocks 7 and 8 unproven, because a
byte-exact compare *also* keeps formats apart - it fails the defect, not the constraint.

**The first mutation attempt did not apply.** The anchor was written with `\n` against a file with
762 CRLF line endings, and the assertion caught it at zero matches rather than reporting a clean
pass over an unmutated file. Fourth time this step has paid.

## 10. CHANGELOG

One `Fixed` line under `[1.5.0]`, user-facing, hyphens only, no em-dash - written when the slice is
built, per [[remoct-changelog-discipline]]. It describes the whole fix, not the library half of it,
because the defect was never only in the library.
