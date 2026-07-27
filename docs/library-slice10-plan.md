# DESIGN NOTE - Library slice 10: genre, stat views, scale

**Scope ID:** LIB-S10. **Status:** BUILT. Greenlit with the §0 ruling and two additions;
Addition 2 turned out to be separate work and is recorded as LIB-S14 in §10.
**Probed against tip `23d0f52`** on 2026-07-26, and **measured against Dos's real 2,157-record
`library.idx` and his real `remoct.conf`** before anything below was decided.

Predecessors LIB-S1 to LIB-S9 and LIB-AA shipped and pushed.

**Two things the tree said that the brief did not, one of them blocking. §0 first.**

---

## 0. STOP AND REPORT - THE STAT VIEWS CANNOT BE BUILT AS SPECIFIED

The brief says stats "join on path at query time", `TrackStats` stays in `Config`, no stored copy in
the index. That design is right. **The join key is broken on real data, and it is broken today in
shipped code.**

### Measured, on Dos's real files

`Config::recordPlay(path)` (`src/Config.cpp:220`) keys `track_stats` on **whatever
`track.path` the playlist entry happens to hold**. That path comes from the folder browser, a loaded
`.m3u`, a favourite, a bookmark, `[Recent]` - and they do not agree on case.

| | |
|---|---|
| index paths (from the OS directory walk) | `C:\Users\david\Music\...` |
| stat keys (241 of 295) | `c:\users\david\Music\...` |
| stat keys (54 of 295) | `C:\Users\david\Music\...` |

So:

| join | stat entries matched | "never played" would read |
|---|---|---|
| **byte-exact `find(path)`** - what the brief implies | **20 of 295** | **99.1%** of the collection |
| **case-insensitive** | **236 of 295** | **89.1%** |

**A byte-exact join finds 20 of 295.** Most-played would draw twenty rows and look like a short but
plausible list. Never-played would draw 99.1% and look like the ~86% the brief predicts. **Both
would pass a glance.** This is the LIB-S8 fixture lesson landing on the product instead of a
fixture: check what the query actually produced, not that it produced something.

The remaining 59 keys match neither way and are **correctly** absent - they are on `D:\Music\...`,
outside the library root. Not a defect; some of them come back under LIB-S11 multi-root.

### It is worse than a join problem: counts are already split

17 files hold **two** `track_stats` entries whose keys differ only in case, and their play counts are
split between them:

| file | entries | true total |
|---|---|---|
| `Da Doo Ron Ron` | 7 + 51 | **58** |
| `One of Us` (the gate disc) | 73 + 22 | **95** |
| `Girls Just Want to Have Fun` | 34 + 21 | **55** |
| `Rockin' Robin` | 2 + 34 | **36** |

A most-played view that picks one entry ranks `One of Us` at 73 or at 22 depending on which it
happens to find. Neither is the number.

### And a shipped defect falls out of the same root cause

`UIManager.cpp:4987` - the info pane's **"Times Played"** - does `config_.track_stats.find(path)`,
byte-exact. So on Dos's machine it already shows the wrong count for those 17 files, and already
says **"never"** for files whose only stat entry differs in case from the path he opened them by.
**Pre-existing, visible, and not something LIB-S10 introduced** - but the stat views would put a
whole pane on top of it.

### What I propose, and what I am NOT proposing

**In LIB-S10:** the stat query folds case **on Windows only** and **aggregates** across variants, so
the view shows 236 played tracks and ranks `One of Us` at 95. Windows filesystems are
case-insensitive, so folding is what "same file" means there; Linux paths are case-sensitive and
folding could merge two genuinely different files, so Linux stays byte-exact. There is **no existing
path-equality helper in the tree** (I looked - no `samePath`, no `stricmp` on paths anywhere), so
this slice adds one small pure predicate rather than open-coding the comparison at each use.

**NOT in LIB-S10, and raised rather than silently absorbed:** fixing `Config` itself - normalising
the key in `recordPlay` and merging the 17 split pairs on load. That **mutates persisted user data**,
it fixes the shipped "Times Played" defect as a side effect, and it deserves its own gate and its own
rollback story. **Proposed as LIB-S13.** If Dos would rather have the root fix first, S10's stat
views should wait behind it - they are the only part of this slice that depends on it, and genre does
not.

**This is the blocking question in this note.** Genre, the scale pass and `searchRowLabel` are
unaffected and can proceed on any ruling.

---

## 1. WHAT THE REAL COLLECTION SAYS ABOUT GENRE

2,157 records, 35 distinct raw genre strings, **679 untagged**. The S8 design's reading holds and the
numbers moved by one record. Applying the ruled rule - split on `/` and `;`, trim, drop empties:

**35 raw values collapse to 27 genres.**

```
979 rock          44 funk                 12 pop-rock          7 podcast
312 pop           44 soul                 12 stage & screen    7 indie
 95 electronic    41 folk, world, & country  10 comedy         6 technology
 88 rap           38 r&b                  10 contemporary pop  5 post punk
 49 hip-hop       38 reggae                9 classical         3 folk
                  18 breakbeat / 18 electro  17 blues          2 post-punk
                  13 synthpop  12 indie pop                    1 audiobooks
```

**The rule does exactly what it was ruled to do**, and the two cases it exists to protect are both
present and both intact: `Folk, World, & Country` survives whole (41 records) rather than becoming a
dangling `& Country`, and `Hip-Hop` survives (49) rather than becoming `Hip` and `Hop`. `R&B` (38)
and `Stage & Screen` (12) also survive, because `&` is not a separator either.

**Rock goes from 748 to 979** - the fold is doing real work, rolling in `Pop / Rock`,
`Electronic / Rock`, `Rock / Funk  /  Soul / Pop` and the rest. Trimming handles the spacing
variants (`Pop/Rock`, `Pop / Rock`, `Funk  /  Soul` with doubled spaces) without a special case.

**Two stated imperfections, both measured rather than predicted:**

- `Pop-Rock` (12) stays its own genre, separate from `Pop` and `Rock`. Known and ruled.
- **New, not in the S8 analysis:** `Post Punk` (5) and `Post-Punk` (2) are two genres. Same cause as
  `Pop-Rock` - the hyphen we deliberately do not split. Seven records. **Recorded, not fixed:** any
  rule that merges them also breaks `Hip-Hop`, which is 49 records and seven times as common.

**Zero non-ASCII genre values in the real collection**, so the gate's non-ASCII genre must be
**planted** to be tested at all. Genre is still tag text and inherits every tag-text rule: it is
identity in `dir_entries_`, it is sanitised for display, and **nothing constructs a filesystem path
from it**.

---

## 2. THE KEY SWEEP - AND THE HANDOFF'S FREE LIST IS WRONG

Ruled method: sweep ASCII punctuation first. Done, across every `case` label and every
`ch ==` comparison in `handleInput` (`src/UIManager.cpp:6118-8294`), plus the pre-switch `if` block.

**`?` IS BOUND.** It toggles the Help pane, via a pre-switch `if` at **`src/UIManager.cpp:6662`**.
The LIB-S8 handoff's free list names `?` first. It is not free.

That is the `v`/`V` trap the same handoff warns about, caught by its own warning: a `case` sweep
cannot see a pre-switch `if`, and the free list was built by a sweep that missed one. **Corrected
free list, verified two ways** (targeted `case 'X'` / `ch == 'X'` grep, then a literal-character grep
over the whole file to confirm each hit is string parsing or a drawn glyph rather than a binding):

**Free:** `"` `#` `$` `%` `&` `'` `(` `)` `:` `@` `^` - eleven.
**Bound punctuation:** `!` `*` `+` `,` `-` `.` `/` `;` `<` `=` `>` `?` `[` `\` `]` `_` `` ` `` `{`
`|` `}` `~` and space.

**So the sweep succeeds and the fallbacks are not needed.** No F5, no cross-platform proof, no
second-press-of-`g`.

**Proposal:**

- **`g` = genres**, scoped to `[Library]`. As ruled - goto-directory is meaningless in a section with
  no directories, the `d`/`D`-in-`[Podcasts]` pattern exactly.
- **`%` = cycle the stat views** (most played -> never played -> off), scoped to `[Library]`.
  `%` reads as statistics, it is plain printable ASCII so it needs no cross-platform proof (the `|`
  precedent), and it is genuinely unbound. **Named alternative if Dos dislikes it: `#`**, which reads
  as a count and is equally free.

Both are inert outside `[Library]`, so no global behaviour changes.

---

## 3. GENRE - ONE LEVEL AND A FILTER

As designed in S8 §2, unchanged:

```
[Library] -> Genres -> Artists (filtered) -> Albums -> Tracks
```

`libnav::Level` gains **`Genres`**; `State` gains **`genre`** (a filter string) and **`sel_genre`**
(the remembered cursor for that level, the fourth alongside artist/album/track and for the same
reason the other three exist).

**Only one new level**, because the three beneath take the filter rather than being duplicated:
`artists(idx, genre)` restricts to tracks whose split genre list contains it, and `Albums` and
`Tracks` inherit it through `State`. Entering `[Library]` still lands on the artist list; `g` reaches
`Genres`; `[Back]`/Left unwinds through it.

**`rowIsPath` is untouched** - `Genres` is a tag-text level like `Artists` and `Albums`, so it is
excluded by construction and no `fs::path` guard changes.

**Compilation awareness comes through `groupingArtist` as constrained** - the filtered `artists()`
is the same function with a predicate, so a compilation stays one `Various Artists` row inside a
genre exactly as it does outside one.

**Untagged tracks get no `(no genre)` node**, as ruled: 679 records is a third of the collection and
the row would be the largest in the list and not a genre. They remain reachable by artist, album and
search.

---

## 4. STAT VIEWS - `Level::Stats`, ONE LEVEL, TWO VIEWS

Subject to §0's ruling. As designed in S8 §3: one new `Level::Stats` with a `which` selector on
`State`, the same shape as `Results` - a flat list of track rows where every level-3 operation works
by construction because `rowIsPath` already covers path-identity levels.

**Naming, so nobody reads them as `[Recent]`:** the header says
`[Library] most played (N)` and `[Library] never played (N)`. `[Recent]` is a *section* showing a
short chronological list; these are *library levels* over the whole collection, reached by a key
inside `[Library]`, and their headers say so.

**Row format.** Most-played reuses `searchRowLabel`'s shape - artist, title, album, extension, same
elision priority - with the **play count prefixed**: `[N] Artist - Title  [Album] (ext)`. The count
is the reason the row is there, so it leads. Never-played has no count and is exactly
`searchRowLabel`. Reusing that function is deliberate: it already solved the width budget and the
seven-format-copies problem, and a second row builder is a second thing to disagree.

**Never-played at 89.1% - the cap.** 1,921 rows on this collection. **It gets the LIB-S7 treatment:
cap at `kLibSearchMax` (500) with the true total in the header**, `(500 of 1921)`. Same constant,
same header idiom, and the same reason - the pane draws `visible` rows and nobody scrolls 1,921 of
them, but the total is the honest and interesting number.

**Most-played takes the same cap for free** and will never reach it (236 today).

**Ordering.** Most-played: count descending, ties by `last_played` descending, then by path so the
order is total and a redraw cannot shuffle it. Never-played: the index's own order, which is already
total.

**No stored copy in the index**, as constrained. The query takes
`const LibraryIndex&` plus `const std::unordered_map<std::string, TrackStats>&` and returns records
by value - `Config` stays the one source of play data, the index stays the one description of files.

---

## 5. `libnav::searchRowLabel` - FOLD IT IN, IT IS CHEAP

Flagged in LIB-S8, ruled into this slice, with permission to cut it if it costs more than the
inconsistency is worth. **It does not.** The whole of it is one line:

```cpp
const std::string artist = t.artist.empty() ? groupingArtist(t) : t.artist;   // index-free
```

`LibraryNav.h` already includes `LibraryIndex.h` and already calls `libidx::` throughout, so there is
**no new dependency** - only an overload taking the index and calling the two-argument
`groupingArtist(idx, t)` that S8 already built. The existing two-argument form stays for callers
without an index in hand, exactly as `groupingArtist` itself kept both forms.

`showLibrarySearch` has `library_index_` in scope at the call site. **Recommendation: build it.**

---

## 6. SCALE - WHAT GETS MEASURED

Against the LIB-S1/S7/S8 baselines with the new views present: scan, index memory, `artists()`,
the new `genres()`, `albumsForArtist()`, search, the compilation build, **and the two stat queries**.

`genres()` gets the S8 **dedup-first** treatment from birth - hash set before `sortUniqueCI` - rather
than shipping the shape S8 had to fix. It has strictly more work per record than `artists()` (a split
per record, not one push), so it is the one to watch.

**Two lessons applied rather than cited.** LIB-S7: time the shipped function, including its sort -
the stat queries sort, so a probe that skips the sort measures a different program. LIB-S8: **check
what the fixture produced**, so the 100k genre fixture must be asserted to yield a plausible genre
count and a plausible row count before any timing from it is quoted. A fixture with one genre on
every record would time an empty problem, which is precisely how S8's compilation fixture measured
nothing.

**The LIB-S7 pre-folded search cache stays cut**, as constrained.

---

## 7. TESTS

**`library_index_test`:**
- genre splitting: `/`, `;`, mixed, doubled spaces, leading/trailing spaces, empty parts dropped
- **commas and hyphens are NOT split** - `Folk, World, & Country` and `Hip-Hop` **by name**, since
  those two are the whole reason the rule is what it is
- `R&B` and `Stage & Screen` survive intact
- `genres()` dedups case-insensitively, omits the untagged, and is byte-identical to a
  sort-everything reference implementation on a fixture with duplicates, case variants and non-ASCII
- `artists(idx, genre)` filters correctly and stays compilation-aware through `groupingArtist`
- the stat queries: most-played ordering including the tie rule, never-played including tracks absent
  from the map entirely, the cap and the true total, and **no subscript escapes** - records by value
- **the case-fold join**: a stat key differing only in case matches on Windows and does not on Linux,
  and **two case-variant entries for one file AGGREGATE** rather than one winning
- scale assertions for `genres()` and the stat queries at 100k, generous bounds

**`library_level_test`:** the `Genres` and `Stats` levels, `return_to` from each, the genre filter
surviving descend and clearing on ascend, `sel_genre` remembered independently of the other three,
`reset` clearing all of it, and the stat-view `which` selector cycling.

**`pane_scroll_test`:** nothing to add - LIB-S9's invariant covers the new levels for free, which is
the point of having built it. **No per-handler scroll nudges**, as constrained.

**Mutation-tested**, with the verify-it-landed step.

**Not machine-tested:** every rendering, both keys, the width behaviour of the count-prefixed row.
Gate.

---

## 8. GATE - EYES-ON, BOTH PLATFORMS

1. `g` opens genres. The list shows **27 rows** on Dos's collection, not 35.
2. **`Folk, World, & Country` is ONE row** and **`Hip-Hop` is ONE row** - the two the rule protects.
   `R&B` and `Stage & Screen` intact.
3. `Rock` includes tracks tagged `Pop / Rock` and `Electronic / Rock`. Entering it filters the artist
   list; `[Back]`/Left unwinds correctly from three levels deep inside a genre.
4. `Pop-Rock`, `Post Punk` and `Post-Punk` appear as separate rows - the stated imperfection, so it
   is confirmed rather than discovered.
5. `%` cycles most played -> never played -> off, and is **inert outside `[Library]`**. Transport
   controls including previous-track still work while browsing.
6. **Most-played is plausible against the real data** - and specifically, `One of Us` shows **95**,
   not 73 and not 22. That single number is the §0 fix, visible.
7. Never-played shows `(500 of 1921)` and stays usable at that size.
8. Both are visibly distinct from `[Recent]` by header and content.
9. **Play something, then re-open most-played: the count moved.**
10. `searchRowLabel` shows `Various Artists` for a planted compilation track with no artist tag.
11. Planted non-ASCII genre draws correctly on both platforms.
12. **Cursor stays visible** in long genre and never-played lists, and after `%` switches view -
    LIB-S9's invariant, confirmed on the new levels.
13. Everything LIB-S3 to LIB-S9 still works: three-level browse, `|` search, `a` at level 2, `F12`,
    `Esc`, `library=0`, `\`. Every other section still enters, draws, exits, plays.

Machine: ctest both toolchains, `--no-tests=error`, currently **47/47 Windows, 48/48 Linux**.
**Measured in the debrief** per §6. Brace-balance and scoped-diff audit before handoff.
**Verification split as in LIB-S4 through LIB-S9.**

---

## 9. FILES EXPECTED TO CHANGE

`include/LibraryIndex.h` (genre split, `genres()`, `artists(idx, genre)`, the two stat queries, the
path-equality predicate), `include/LibraryNav.h` (`Genres` and `Stats` levels, the genre filter,
`sel_genre`, the stat `which` selector, the index-aware `searchRowLabel` overload),
`src/UIManager.cpp` (two keys, two populates, headers, `populateLevel` cases),
`include/UIManager.h`, `tests/library_index_test.cpp`, `tests/library_level_test.cpp`,
`CHANGELOG.md`, and this note as design-of-record.

**No index format change.** Genre is already field 6 of a 12-field record and is already parsed;
nothing new is stored. **No new stored copy of play data.** No new dependency.

**Not touched:** `LibraryScanner`, `PlaylistManager`, `PaneScroll.h`, `BrowserPins.h`, `Config`
(see §0 - the `Config` fix is LIB-S13, not this slice), `Version.h`, the audio thread, `ar_crc`, the
CD path, the rip path, the plugin ABI.

**Numbered, so nothing floats:** **LIB-S11** multi-root (designed, awaiting brief). **LIB-S12** the
redundant per-handler scroll calls. **LIB-S13 (from §0)** normalise `track_stats` keys in `Config`
and merge the 17 split pairs. **LIB-S14 (from §10)** make the info pane follow the browser cursor.

---

## 10. ADDITION 2 - DIAGNOSED, AND IT IS NOT A LIBRARY BUG

**Symptom (hardware):** `I` on a library search result shows `.38 Special - Hold on Loosely`
whatever row is highlighted.

**Cause, read rather than inferred.** `drawTrackInfo` picks its subject at
`src/UIManager.cpp:4796-4803`, and its own comment states the design:

```cpp
// Which track to show: cursored row if browsing the playlist, else the playing
// row (stream-aware), else the last-known index for the nothing-playing floor.
std::size_t idx;
if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) idx = pl_cursor_;
else if (auto r = nowPlayingRow())                                  idx = *r;
else                                                                idx = playlist_.current();
...
const PlaylistEntry& entry = pod_pane ? pod_entry : playlist_.at(idx);
```

**The info pane has never read the browser cursor for any section except `[Podcasts]`**, which was
wired specially during the podcast campaign (`:4824-4842`, and its comment calls that "Dos's normal
behavior"). Everything else falls through to `playlist_.at(idx)` - a playlist row, which is constant
while you scroll a browser pane. `.38 Special` is whatever is playing or last selected there.

**Answering the brief's question directly:** it is not Results-specific, and it is not both-levels
either. It affects **every browser row in every section** - level 3 tracks, search results, artists,
albums, `[FAVs]`, `[Recent]`, `[Books]`, `[Radio]`, and plain folder files. So it is exactly what the
brief said it would be if it were in both: **an operation nobody classified.** LIB-S5 wired
Enter/`a`/`q`/`*`/`u`/`x`/`;` for library rows through `browserEntryPath` and `libraryRowPath`; `I`
is not among them because `I` never read the browser at all, in any section, since long before this
campaign.

### RULED IN, and built here - the decoupled route

**Dos's position, and it is the right one:** highlighting a row and being shown a different
track's metadata looks broken whatever the mechanism, and shipping that alongside two new
browsable views makes it more visible, not less. The user-facing outcome was not negotiable.

**The refusal below was not overruled - it was routed around**, and the route is Dos's: fix the
subject resolution for DISPLAY, and make `e` inert when the subject did not come from the playlist.
Then `I` is correct now and `e` cannot write the wrong file, because it does not write at all.

**It is cleaner than the status quo, because `e` and the pane were already two copies of one rule.**
The `e` handler's resolution at `:7111-7118` was a VERBATIM copy of `drawTrackInfo`'s at
`:4796-4803`. That is the same defect class as the two pinned-row lists (`BrowserPins.h`) and the two
scroll clamps (`PaneScroll.h`) - the third instance in this campaign.

**Built:**

- **`UIManager::infoPaneSubject()`** - ONE resolver returning `{source, path, display_title,
  duration_sec, pl_index}`. Order: a `[Podcasts]` row under the cursor, then **any other browser
  row**, then a standalone playing episode, then the playlist. Both readers consult it, so the pane
  and the tag editor cannot mean different files.
- **`browserRowIsFile()`** - `browserEntryPath` already rejects pseudo-rows and tag-text levels; this
  adds the directory case for the plain folder browser, with the same `!in_*` guard the draw loop
  uses at `:3075` for the same CP1252 reason. Cost is ONE `fs::is_directory` for the cursor row,
  where the draw loop already makes that call for every VISIBLE row.
- **`e` gated on `InfoSource::Playlist`.** Browser or podcast subject: refuse with
  "Tag editing works on playlist rows - press Enter to add this first". This also closes a latent
  bug that predates the slice - `e` on a browsed PODCAST row would have edited a playlist track.
- **The header advertises `e` only when `e` will act.** A header offering an action that then
  refuses is how a user learns to distrust the header.
- **The podcast art block no longer computes a second copy of the subject** - it computes art, and
  the subject comes from the resolver. That second copy was maintenance, not structure, and
  maintenance is what let the two drift.

**THE RULE, in one sentence: when the browser has focus, the pane shows the row under the cursor or
says it has nothing.** No exceptions - "sometimes follows the cursor, sometimes shows the playing
track" is the behaviour that reads as broken.

**Two consequences worth stating rather than discovering on hardware.** A non-file browser row (an
artist, album or genre row, a section pin, `..`, a directory) now shows **"No track info for this
row"** instead of the playing track. And browsing files while a podcast episode plays now shows the
highlighted FILE rather than the episode. Both follow from the one-sentence rule; both are behaviour
changes to shipped paths, and both are in the gate.

### Why it was NOT proposed for this slice originally

**Because the info pane is not read-only.** The same pane hosts tag editing - its header says
`e:edit tags` - and `e` resolves its target the same way (`:7106`, `tag_edit_path_ = path`, from the
same `playlist_.at(idx)`), then `saveTagEdits()` **writes the file** and syncs the playlist row
(`:4017-4047`).

So a display-only fix would make the pane show one file while `e` edited a different one: a
file-writing mismatch, strictly worse than the bug. Moving both together is not a display change at
all - it makes tag-editing operate on browser rows, which means deciding what `TagEditability`,
`PlayingLocked` and the playlist-sync loop mean for a file that is not in the playlist. That is a new
write capability, and it needs its own gate across eight browser contexts.

**LIB-S14 is now narrower than it was**, and better defined for it: the display half shipped here, so
S14 is exactly the write half - make tag editing work on a browser row, which means deciding what
`TagEditability`, `PlayingLocked` and `saveTagEdits`'s playlist-sync loop mean for a file that is not
in the playlist. The `e` refusal is the placeholder, and it is an honest one: it says what to do
instead.

---

## 11. MEASURED - real collection, and the fixture that lied first

**Timed against Dos's real `library.idx` (2,156 records) and real `remoct.conf`, `-O2`, calling the
shipped functions** (not a reimplementation - the probe includes `LibraryIndex.h` and calls
`libidx::` directly, per LIB-S7's lesson that a probe which skips a step measures a different
program):

| query | real collection | 100k synthetic (default build, no `-O2`) |
|---|---|---|
| `parseIndex` | 2.24 ms | 824 ms |
| `artists()` | 0.14 ms (90 rows) | 35.8 ms |
| **`genres()`** | **0.08 ms (27 rows)** | **53.5 ms** |
| `artists(idx, "Rock")` | 0.12 ms (51 rows) | - |
| **`buildPlayStats()`** | **0.05 ms** (295 raw -> 278 folded) | 4.7 ms |
| **`mostPlayed()`** | **0.86 ms** (219 of 219) | 46.3 ms |
| **`neverPlayed()`** | **0.36 ms** (500 of 1,937 = 89.8%) | 27.8 ms |

**The §0 numbers, confirmed through the shipped query rather than a script:** a byte-exact join
matches **20 of 295** stat entries; the folded join yields **219 played files**. And those two
numbers cross-check the whole diagnosis - the earlier script counted **236** matching stat *entries*,
and 236 minus the **17 split pairs** is exactly **219 files**. `One of Us` reads **95**.

**`genres()` output on the real collection, all 27**, with every protected case and every stated
imperfection visible in one line:

```
Audiobooks | Blues | Breakbeat | Classical | Comedy | Contemporary Pop | Electro |
Electronic | Folk | Folk, World, & Country | Funk | Hip-Hop | Indie | Indie Pop |
Podcast | Pop | Pop-Rock | Post Punk | Post-Punk | R&B | Rap | Reggae | Rock |
Soul | Stage & Screen | Synthpop | Technology
```

`Folk, World, & Country`, `Hip-Hop`, `R&B` and `Stage & Screen` intact; `Pop-Rock`, `Post Punk` and
`Post-Punk` present as separate rows - the imperfection confirmed rather than discovered.

### The fixture lied first, and the check I wrote for it caught me

The 100k scale fixture initially set no `album_artist`, so **every synthetic album was correctly
flagged a compilation and `artists()` returned ONE row.** That is LIB-S8's failure reproduced exactly,
in a fixture written by someone who had just documented it. The fixture-sanity assertion added in the
same commit - assert the fixture PRODUCED something plausible before quoting a timing from it - is
what failed, and it named the cause. Two more failures in the same run were my own arithmetic on
expected genre counts.

**The lesson survives contact with the person who wrote it down. The assertion is what worked.**

### Mutation-tested, each verified to have LANDED before the outcome was trusted

| mutation | caught by |
|---|---|
| also split genres on `,` | `splitGenres("Folk, World, & Country") gave 3 parts, wanted 1` |
| `buildPlayStats` overwrites instead of summing | `counts must SUM to 95, got 22` |
| `foldPathKey` drops the case fold | 4 failures incl. `the two case-variants must fold to ONE key` |

Each mutation named the real-world defect it protects against, which is the point of writing the
tests around measured cases rather than invented ones.

**Gates: Windows 47/47, Linux 48/48**, `--no-tests=error`. `library_index_test` 307 checks,
`library_level_test` 161 checks.
