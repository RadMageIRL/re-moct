# DESIGN NOTE - Library slice 8: compilations, genre, stat views, scale

**Scope ID:** LIB-S8. **Status:** proposed, awaiting sign-off. Nothing implemented.
**Probed against tip `bba2c8e`** on 2026-07-26, and **measured against the real 2,156-record index
and Dos's real `remoct.conf`** before anything below was decided.

Predecessors LIB-S1 to LIB-S7 and LIB-AA shipped, pushed, CI-green, hardware-gated.

## SPLIT, RULED BY DOS AT GREENLIGHT

This note designs four things. They ship as **two numbered slices**, not sub-letters - a slice list
should read 1 through N with nothing floating, which is the same shape of problem as an unnumbered
item.

| slice | contents | this note |
|---|---|---|
| **LIB-S8** (this one) | compilations + the `artists()` scale fix | §0, §1, §4's `artists()` fix, §7-§9 |
| **LIB-S9** (next, still 1.5.0) | genre + stat views + the remaining scale measurements | §2, §3, the rest of §4, §5 |

Compilations ship first because they are a **correctness defect** - ten compilations currently render
as roughly 150 one-track albums - and gating that alongside three new views makes the broken thing
wait on the merely absent ones. The `artists()` fix rides with it because it touches the same queries
and is five lines.

**Campaign remainder: LIB-S8, LIB-S9, then the 1.5.0 ceremony.**

**Ruling 2 (stat-view binding) lands in LIB-S9, not here.** Recorded so it is not lost: `p` is
REJECTED - transport controls must keep working while browsing, and `[Library]` is exactly where
someone reaches for previous-track. Order of preference for S9: sweep ASCII punctuation first (the
`|` precedent, no cross-platform proof needed), then F5 **with proof on both platforms** (the F12
standard), then a second press of `g` as fallback.

---

## 0. WHAT THE REAL COLLECTION SAYS

The brief says to decide against real files. Everything in §1 and §2 comes from this, not from
reasoning about what tags are usually like.

### Compilations - the shape is unambiguous

Grouping every album name that has more than one distinct track artist, with the album-artist tag
each carries:

| album | tracks | distinct artists | album_artist |
|---|---|---|---|
| The Rock 'N' Roll Era: 1964 | 22 | **22** | *(none)* |
| Pure 80's | 40 | **20** | *(none)* |
| The Rock 'N' Roll Era 1957 | 22 | **20** | `Various` |
| Pure '80s #1s | 36 | **18** | *(none)* |
| Pure Reggae | 18 | **17** | *(none)* |
| The '80s Hit(s) Back! | 24 | **12** | *(none)* |
| Sleepless in Seattle: Original Motion... | 24 | **11** | *(none)* |
| How High [Original Soundtrack] | 20 | **11** | `Soundtrack` |
| Sleepless in Seattle | 12 | **11** | *(none)* |
| Decade of Music | 9 | **9** | *(none)* |
| **Plastic Beach** | 16 | **6** | `Gorillaz` |
| **The Ultimate Collection** | 21 | **3** | `Jackson 5, The` |
| Mermaid Avenue, Floored, Gutterflower, Total, Back to Back, Playlist: ... | | 2 each | real artists |

**Three facts that decide the rule:**

1. **Eight of the ten real compilations have NO album-artist tag at all.** Matching on
   `album_artist == "Various Artists"` would catch **two of ten**. And neither of those two says
   "Various Artists" - they say `Various` and `Soundtrack`, so variant matching is required even for
   the two it would catch. 766 of 2,156 records (35%) have an empty album-artist.
2. **The two false-positive shapes the brief predicted are both in the collection**, in bold above.
   `Plastic Beach` is the guest-artist album (Gorillaz, 6 credited artists); `The Ultimate Collection`
   is artist-tag variance within one artist's record. **A distinct-artist-count threshold alone
   misclassifies both.**
3. **What separates them is the album-artist tag naming a real artist.** Every genuine compilation
   has album-artist empty or various-ish; both false positives have it set to the actual artist.

### Genre - the mess is compound separators, not case

35 distinct raw values over 1,478 tagged records (678 untagged). **Zero case variants** - no `Rock`
vs `rock`. What is actually there:

```
748 Rock      65 Pop / Rock          22 Rock / Funk  /  Soul / Pop
173 Pop       50 Electronic / Rock   21 Pop;Rock
 39 Rap       49 Hip-Hop / Rap       15 Rock / Folk, World, & Country
 38 Reggae    12 Pop-Rock            15 Rock / Blues / Folk, World, & Country
```

So the problem is one track carrying several genres in one string, with `/` and `;` as separators.

### Stats

`Config::track_stats` is `unordered_map<path, TrackStats{play_count, last_played}>`, and Dos's real
config holds **295 entries** against 2,156 indexed tracks. So a play-count view has real data, and
"never played" is ~86% of the collection - a real rediscovery surface, not a curiosity.

### Scale - a finding already, before anything is built

`libidx::artists()` at 100,000 records: **56.6 ms**. That is on the path into `[Library]` and on
every repopulate, so it is felt. It pushes one string per track and then `sortUniqueCI`s 100,000
strings down to 900.

Deduplicating through a hash set first and sorting only the unique values:

| n | `artists()` today | dedup-first | unique |
|---|---|---|---|
| 2,773 | 1.4 ms | **0.5 ms** | 900 |
| 100,000 | **56.6 ms** | **1.4 ms** | 900 |

**40x at 100k, for about five lines, with byte-identical output** (asserted in the probe). This is
the scale pass's main find and §4 proposes it.

---

## 1. Compilations

### The rule

An album is a compilation when **both** hold, over all tracks sharing that album name:

1. **every non-empty album-artist on it is "various-ish"** - matched case- and
   accent-insensitively through `detail::foldAscii` against `various`, `various artists`, `va`,
   `compilation`, `soundtrack`, `ost`, `original soundtrack`, `original motion picture soundtrack`
   - or there is no album-artist at all; **and**
2. **at least 3 distinct track artists.**

On the real collection this flags **exactly the ten genuine compilations and neither false
positive**. Test 1 is the discriminator; test 2 is the backstop. Measured: the flagged set is
identical for thresholds of 2, 3, 4 and 5, because every real compilation here has at least 9
artists and no empty-album-artist album has 2. **3 is chosen as the safe middle** - low enough that a
genuine 3-artist split EP is caught, high enough that two unrelated albums sharing a name and both
missing album-artist cannot collide into one.

**The false-positive mode I am accepting, stated:** a genuine compilation whose album-artist is set
to one of its contributing artists will not be detected. There is no signal left to detect it with -
the tags say it is that artist's album - and inventing one would need MusicBrainz, which is a
non-goal.

### Where it lands: `groupingArtist`, as designed

`groupingArtist` has been the seam since LIB-S1 and stays it. It becomes index-aware:

```
// was: album_artist non-empty ? album_artist : artist
// now: compilation ? kVariousArtists : (album_artist non-empty ? album_artist : artist)
```

Because the rule needs the whole album's tracks, not one record, the per-album verdict is computed
**once per index** into a `std::unordered_set<std::string>` of compilation album names, held on
`LibraryIndex` beside `tracks`. It is derived, not stored: **no format change, no new field on
disk**, built after parse and after every scan. `groupingArtist(idx, track)` gains an index
parameter; the one-argument form stays for callers that have no index (the scanner) and behaves as
today.

Every browse query already routes through `groupingArtist`, so **artists, albumsForArtist,
tracksForAlbum and search all become compilation-aware at once** - which is what the seam was for.

A compilation therefore appears as **one album under a single `Various Artists` artist node**,
sorted into the artist list by that name. Not a separate section, not a parallel tree: it is an
artist row like any other, and everything already built for artist rows works on it.

### Track rows inside a compilation must show the per-track artist

Otherwise the view is a list of titles with no way to tell who is playing, which is the whole point
of a compilation. `libnav::trackRowLabel` gains an `include_artist` flag, set when the album is a
compilation:

- normal album: `03. Song Title  (3:21)` (unchanged)
- compilation:  `03. Artist - Song Title  (3:21)`

**Width:** the artist is inserted before the title, and when the row will not fit the **title is
preserved over the artist** - the same priority LIB-S7's result rows use, because the title is what
identifies the row. The existing column-aware clipping in the pane does the actual cutting.

## 2. Genre

### Compound splitting

A genre string is split on `/` and `;`, each part trimmed, empties dropped. `Pop / Rock` becomes
`Pop` and `Rock`; `Pop;Rock` likewise; `Rock / Folk, World, & Country` becomes `Rock` and
`Folk, World, & Country`.

**Commas and hyphens are deliberately NOT separators.** Splitting commas would turn
`Folk, World, & Country` into `Folk`, `World`, `& Country` with a dangling ampersand; splitting
hyphens would break `Hip-Hop` into `Hip` and `Hop`. So `Pop-Rock` remains its own genre, and that is
a stated imperfection rather than an oversight - on this collection it is 12 records.

Comparison and dedup use the existing case-insensitive path, so a future `rock` would fold into
`Rock` even though this collection has no case variants today.

On the real data this collapses 35 raw strings into roughly 20 real genres, and rolls
`Pop / Rock` + `Electronic / Rock` + `Rock / Funk / Soul / Pop` into the 748-strong `Rock`.

### Where it lives: one new level, then the existing three

`libnav::Level` gains `Genres`, and `State` gains `genre` (a filter string).

```
[Library] -> Genres -> Artists (filtered by genre) -> Albums -> Tracks
```

**Only one new level**, because the artist/album/track levels take the filter rather than being
duplicated: `artists(idx, genre_filter)` restricts to tracks whose split genre list contains it, and
the two levels below inherit it through `State`. Entering `[Library]` still lands on the artist list
as it does today; `Genres` is reached by a key (§5) and `[Back]`/Left unwinds through it.

An untagged track appears under no genre. **Not under a synthetic "(no genre)" node** - 678 records
have no genre, and a bucket holding a third of the collection under a name that is not a genre would
be the largest row in the list and useless. They remain reachable by artist, album, and search, which
is where someone looks for them.

## 3. Stat views

### `[Recent]` already exists, so one of the three proposed views is CUT

The brief asks how these differ from `[Recent]`. **`[Recent]` is already "recently played"** - it is
`config_.recent_tracks`, maintained on every play by exactly the path this campaign has been calling.
Shipping a library "recently played" view would be a second thing that looks the same and drifts.

**CUT: recently-played. Reason: `[Recent]` is that view and supersedes it.** Recorded here rather
than left floating.

That leaves two, both of which are genuinely new because neither can be expressed today:

- **Most played** - `track_stats` sorted by `play_count` descending, ties by `last_played`, joined on
  path at query time. 295 entries of real data.
- **Never played** - indexed tracks with no `track_stats` entry or a zero count. ~86% of this
  collection, and the only view that answers "what have I not listened to".

### Where they live

One new level, `Level::Stats`, with a small `which` selector on `State` - the same shape as
`Results`: a flat list of track rows, every level-3 operation working by construction because
`rowIsPath` already covers path-identity levels. Two views, one level, no new section.

**The join is at query time and nothing is copied into the index**, per the constraint. It is a
`LibraryIndex` plus a `const std::unordered_map<std::string, TrackStats>&` in, records by value out -
so the index stays the single description of the files and `Config` stays the single source of
play data.

## 4. Scale pass

### The `artists()` fix - measured, and the main find

Dedup through a hash set before sorting (§0). **56.6 ms to 1.4 ms at 100k, 1.4 ms to 0.5 ms at the
real scale, byte-identical output.** It applies to `artists()`, to the new `genres()`, and to
`albumsForArtist()`, which has the same push-everything-then-sort shape.

This is worth doing not because 100k libraries are common but because `artists()` is on the path
**into** the section and on **every repopulate after a rescan**, so it is the one query whose cost the
user waits on rather than asks for.

### What else gets measured, in the debrief

Against the LIB-S1 (parse) and LIB-S7 (search) baselines, with the new views present: scan time,
index memory, `artists()`/`genres()`/`albumsForArtist()`, search, and the compilation-set build. The
compilation set is one pass over the index at load - I expect it to be small, and **I will measure it
rather than say so**, because LIB-S7's lesson is exactly that a design-note estimate is not a
measurement.

### The pre-folded search cache: raised, and recommended AGAINST

LIB-S7 measured 100k search at 55.9 ms per keystroke after `partial_sort`, and identified a
pre-folded blob as the remedy (7.0 ms search, 38.9 ms build, 6.4 MB). **I do not propose building
it.** At the real scale a keystroke costs 0.85 ms; the cache buys nothing anyone can perceive and
costs a parallel copy of every tag string that has to be invalidated on rescan. Recorded as a known,
measured option if a 100k library ever appears.

## 5. Keys

Two new entry points are needed: genres, and the stat views. Every letter is bound and most pairs are
aliased (LIB-S6), so this follows the section-scoped idiom.

**Proposal, both scoped to `[Library]` and inert everywhere else:**

- **`g`** - go to the genre list. `g` is currently goto-directory, which is meaningless inside
  `[Library]` (there is no directory to go to), so this is the `d`/`D`-in-`[Podcasts]` pattern
  exactly: a key that means something else where it has nothing to do.
- **`p`** - cycle the stat views: most played, never played, off. `p` is previous-track globally;
  **this is the one that displaces something reachable**, so the alternative is below.

**I am uneasy about `p` and would rather Dos ruled.** Previous-track is a transport control and a
user may well want it while browsing the library. Two alternatives, both cleaner:

- **`F5`** - unbound, and the free F-keys were surveyed in LIB-S6 (F1, F4, F5, F9, F10 all free).
  Needs cross-platform proof on both `wingui` and `ncursesw`, which F12 has and F5 does not, so I
  would prove it before shipping rather than assume.
- **A second press of `g`** cycling genre -> most played -> never played -> artists. One key, no
  displacement, at the cost of being less discoverable.

**Recommendation: `g` for genres, and `F5` for stat views subject to proving it on both platforms** -
with the second-press-of-`g` fallback if F5 misbehaves anywhere.

## 6. Size, and what I would cut if this is too big

All four parts are proposed and I believe all four are buildable, but this is visibly the largest
slice of the campaign: two new levels, a filter dimension, an index-derived set, a new label mode,
two new queries, a query-time join, and a scale change to three existing queries.

**If it needs to be smaller, the split I would make - and my recommendation is to take it:**

- **LIB-S8a (this slice): compilations + the `artists()` scale fix.** Compilations are the
  *correctness* item - the browse tree currently misrepresents the collection, showing ten
  compilations as roughly 150 one-track albums. The scale fix rides along because it touches the same
  queries and is five lines.
- **LIB-S8b (immediately after, still 1.5.0): genre + stat views + the remaining scale measurements.**
  Both are additive browse surfaces; neither fixes anything that is currently wrong.

That ordering means the thing that is *broken* ships first and gets its own gate, rather than being
gated in the same pass as three new views. **If Dos prefers one slice, I will build one slice** - the
design above is complete either way, and nothing is deferred silently: every part has a number.

## 7. Tests

Nearly all of this is pure, which is why it is proposable at this size.

**`library_index_test`:**
- the compilation rule against a fixture reproducing all four real shapes: no-album-artist
  multi-artist (the common case), `Various`/`Soundtrack` variants, the guest-artist album with a real
  album-artist, and the artist-variance album - asserting the first two flag and the last two do not
- accent- and case-insensitive variant matching (`various`, `VARIOUS ARTISTS`, `Vàrious`)
- the 3-artist threshold boundary: 2 artists does not flag, 3 does
- `groupingArtist` returns `Various Artists` for a compilation track and the real artist otherwise,
  and the one-argument form is unchanged
- genre splitting: `/`, `;`, mixed, extra spaces, and that commas and hyphens are NOT split
- `genres()` dedups case-insensitively and omits the untagged
- the stat queries: most-played ordering with ties, never-played including tracks absent from the map
  entirely, and that **no subscript escapes** - records by value, as every query since LIB-S1
- **the dedup-first rewrite produces byte-identical output** to the current `artists()` on a fixture
  with duplicates, case variants and non-ASCII
- scale assertions for `artists()` and the compilation build at 100k, generous bounds, as regression
  guards rather than benchmarks

**`library_level_test`:** the `Genres` and `Stats` levels, `return_to` from each, the genre filter
surviving descend and clearing on ascend, `reset` clearing both, and the compilation track-row label.

**Not machine-tested:** every rendering, both keys, and the width behaviour of the compilation row.
Gate.

## 8. Gate - eyes-on, both platforms

1. **`Pure 80's` browses as ONE album with 40 tracks**, under `Various Artists`, with each track
   showing its own artist - not as 20 one-track albums under 20 artist rows.
2. **`Plastic Beach` is unaffected** - still one Gorillaz album, not a compilation, despite six
   credited artists. Same for `The Ultimate Collection`.
3. `The Rock 'N' Roll Era 1957` (album-artist `Various`) and `How High` (`Soundtrack`) both group as
   compilations, proving variant matching.
4. Genre list shows folded compound genres; `Rock` includes tracks tagged `Pop / Rock`. Entering a
   genre filters the artist list; `[Back]`/Left unwinds correctly from three levels deep inside it.
5. Most-played shows plausible data against the real 295 stat entries; never-played is large and does
   not include anything just played. Both are visibly different from `[Recent]`.
6. **Non-ASCII, planted and deterministic:** a compilation with an accented album name, an accented
   genre, and a compilation track whose artist carries a raw Latin-1 byte.
7. **Everything from LIB-S3 to LIB-S7 still works:** three-level browse, `|` live search, `a` at
   level 2, `F12` rescan, `Esc` cancel, `library=0`, and `\` unchanged.
8. Every other section still enters, draws, exits and plays.

Machine: ctest both toolchains, `--no-tests=error`, currently 46/46 Windows and 47/47 Linux.
**Measured in the debrief** per §4. Brace-balance and scoped-diff audit before handoff.
**Verification split as in LIB-S4 through LIB-S7.**

## 9. Files expected to change

`include/LibraryIndex.h` (the compilation set, `groupingArtist`'s index-aware form, genre splitting,
`genres()`, the two stat queries, the dedup-first rewrite), `include/LibraryNav.h` (`Genres` and
`Stats` levels, the genre filter, the compilation row label), `src/UIManager.cpp` (two keys, two
populates, headers, the `populateLevel` cases), `include/UIManager.h`, `src/LibraryScanner.cpp`
(build the compilation set after a scan), `tests/library_index_test.cpp`,
`tests/library_level_test.cpp`, `CHANGELOG.md`, and this note as design-of-record.

**No index FORMAT change.** The compilation set is derived at load from fields already stored, so no
shipped `library.idx` is invalidated. **No new stored copy of play-count data** - `Config` remains the
one source, joined at query time.

**Not touched:** `BrowserPins.h`, `PlaylistManager`, `AudioManager`, the audio thread, `ar_crc`, the
CD and rip paths, scrobblers, plugins, `Version.h`. `Ctrl+T` unchanged.
