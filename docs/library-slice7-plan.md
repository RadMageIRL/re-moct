# DESIGN NOTE - Library slice 7: collection-wide search

**Scope ID:** LIB-S7. **Status:** proposed, awaiting sign-off. Nothing implemented.
**Probed against tip `b955fe6`** on 2026-07-26, and **measured** against the real 2,156-record
index before any of the design below was settled.

Predecessors LIB-S1 to LIB-S6 and LIB-AA shipped, pushed, CI-green, hardware-gated.

---

## RULINGS - settled by Dos at greenlight, do not re-derive

| # | Question | RULING and what probing found |
|---|---|---|
| 1 | Live or on submit | **LIVE, as you type** - overriding my submit-on-Enter recommendation. Probed: `handleGotoInput` has a SINGLE exit point, so the hook is one insertion - **extending the input-bar machinery, not rebuilding it**, so the stop-and-raise condition does not trigger. Coalescing falls out free: the hook fires only when the TEXT changed, so cursor moves and Tab do not re-query. **No debounce framework**, per the ruling. |
| 2 | ASCII case fold | **BUILD IT - probed and it is cheap.** A static table, no dependency, no format change. **And the ruling's example needs more than case folding**: case-folding `Ö` gives `ö`, so `bjork` still would not match. It needs case AND DIACRITICS folded together, which is what was built. Verified against real names: `BJÖRK`/`Björk`→`bjork`, `Mötley Crüe`→`motley crue`, `Sigur Rós`→`sigur ros`, `Beyoncé`→`beyonce`, `Niño`→`nino`, `straße`→`strasse`, `Łódź`→`lodz`, `Dvořák`→`dvorak`. |

**Precedent found while probing ruling 2:** `sanitizeForDisplay` **already ASCII-folds accented Latin**
- é/è/ê/ë→e, ç→c, ô/ø→o, æ→ae, œ→oe - as an ad-hoc partial list grown by need (its own comment records
the "Prep-School"→"Prep?School" bug that grew the dash cases). It has no `ö`, no `ñ`, no `á`. So
folding Latin to ASCII is an ESTABLISHED IDIOM in this codebase; this slice does it completely, in one
place, for matching only. `sanitizeForDisplay` is not touched - display keeps its accents.

---

## 0. THE MEASUREMENT FIRST, because it removes a whole design question

The brief warns that "interactive search has a much tighter budget than a one-time parse" and asks
for an auxiliary structure if needed. **It is not needed, and that is measured rather than hoped.**

A prototype of the query below, run against Dos's real `library.idx`, 20 iterations averaged:

| query | hits | allocation-free | lowercase-per-field |
|---|---|---|---|
| `love` | 124 | **0.31 ms** | 0.47 ms |
| `the` | 970 | **0.19 ms** | 0.31 ms |
| `a` | 2045 | **0.03 ms** | 0.09 ms |
| `beastie` | 147 | **0.29 ms** | 0.44 ms |
| `zzzznomatch` | 0 | **0.28 ms** | 0.45 ms |
| `muse absolution` (two terms) | 0 | **0.32 ms** | 0.49 ms |

**100,000 synthetic records, `love`: 16.6 ms** for 5,739 hits.

Conclusions, all load-bearing below:

1. **No index, no cache, no auxiliary structure.** A linear pass over six fields is a third of a
   millisecond at the real scale. Proposing a trie or an inverted index here would be inventing
   maintenance for a cost that does not exist.
2. **Live-as-you-type is affordable** - 0.3 ms per keystroke - so if it is not built, performance is
   not the reason (see §5).
3. The allocation-free compare is ~1.5x faster for the same amount of code, so it is what §2 uses.
4. A one-character query matches **2045 of 2156 records**. That is not a bug but it is a shape the
   design has to answer for (§4's cap and count).

## 1. FINDING - `/` is the wrong key, and it is wrong for a reason worth stating

The brief suggests section-scoped overloading and names `/` among the precedents. **`/` is taken, and
its established meaning is the opposite of what this slice needs.** From its own comment at
`src/UIManager.cpp:8136`:

```
// "/ = find something new online" in the section you're in. Radio: station
// search. Podcasts (feed level): Podcast Index search for new feeds.
// '\' stays focus-aware local search; these are distinct.
```

So the codebase already draws exactly the distinction this slice sits on: **`/` searches a remote
catalogue for something you do not have; `\` searches what you do have.** A library search is the
second thing. Overloading `/` for it would break a two-key semantic that is documented, shipped and
consistent across two sections.

**And `\` alone is not the answer either.** Making `\` collection-wide inside `[Library]` would
silently take away pane-local search where it is genuinely useful - at level 1, jumping to one artist
among five hundred. That is a real capability, and losing it to gain another is not a trade the user
asked for.

### Proposal: `|` - Shift and the same key

`|` (0x7C) is **unbound anywhere in the program** (verified by sweeping every `case` and every
pre-switch `if`). The mapping reads itself:

- `\` - search **what you are looking at** (unchanged, everywhere, including inside `[Library]`)
- `|` - search **everything you have**

Same physical key, shifted, escalating scope. Nothing is displaced, no alias is split, and no
existing semantic is bent.

**And it needs no cross-platform proof, which an F-key would.** `|` is a plain printable ASCII
character, so it arrives as itself through both `wingui` and `ncursesw` - no terminfo, no keypad
mapping, none of what made F11 unsafe and made F12 in LIB-S6 need shipped evidence. The free F-keys
(F1, F4, F5, F9, F10) are all worse than a free ASCII character for exactly this reason.

**Scope: `|` works inside `[Library]` at any level, and also from the folder browser** when the
library is enabled, because "find a track without knowing where it lives" should not require first
navigating to the place you do not know it is in. From outside, it enters the section and lands on
results; `[Back]`/Left then unwinds through §3's rule. When `library=0` it does nothing at all -
there is no collection to search.

## 2. The index query - the authorised surface change

New in `LibraryIndex.h`, beside `artists` / `albumsForArtist` / `tracksForAlbum`:

```
std::vector<LibraryTrack> search(const LibraryIndex& idx,
                                const std::string& query,
                                std::size_t limit,
                                std::size_t* total_out);
```

**Returns records BY VALUE**, which is the `libnav::albumTracks` precedent and the sixth consecutive
slice of the staleness rule: no subscript leaves the function, so nothing the UI holds can dangle
when a rescan replaces the index.

**No format change.** This reads the fields already in every shipped index, so no existing
`library.idx` is invalidated. That is the line the brief drew and this does not cross it.

**Fields matched** - the six a person actually searches by:
`title`, `artist`, `album`, `album_artist`, `genre`, and the **filename stem**. The stem is included
because an untagged rip has nothing else, and those are exactly the tracks hardest to find by
browsing. `path` is matched only via its stem, never whole, so a query cannot accidentally match a
directory name and return every track under it.

**Matching rule: case-insensitive substring, AND across terms, OR across fields.** Split the query
on spaces; a record matches when *every* term is found in *at least one* field. That is what makes
`beastie sabotage` work, which is how people actually search. Prefix or word-boundary matching would
fail `sabotage` against `"Sabotage (Remastered)"`-style tags less often but would fail mid-word
queries always, and substring is what `\` already does, so the two keys behave alike.

**The compare is deliberately its own function, not `detail::icmp`.** `icmp` is a whole-string
compare - correct for `restoreCursor` and level identity, useless for substring.

**`detail::foldAscii` (ruling 2)** folds a UTF-8 string to lowercase ASCII, doing case and diacritics
in one pass: Latin-1 Supplement and Latin Extended-A mapped to base letters, with the two-letter
expansions that actually occur (`æ`→`ae`, `œ`→`oe`, `ß`→`ss`, `þ`→`th`, `ĳ`→`ij`). Query and fields
both go through it, so matching is fold-insensitive in both directions. Anything outside those two
blocks - CJK, Cyrillic, Greek, emoji - is **passed through unchanged and matched byte-exactly**, which
is the stated limit: this is a Latin fold, not a Unicode collation, and growing it further would be a
case table this project should not carry for one feature.

Measured cost of folding, real index, 20 iterations averaged: **0.11-0.54 ms** per whole-collection
search (against 0.03-0.33 ms unfolded), so the fold roughly doubles a cost that was already nothing.

**The 100k scale limit, measured and named rather than left to be found:** folding every field on
every search is **23.8 ms** at 100k records, which as a per-keystroke cost while typing would be felt.
The remedy is identified and measured - a pre-folded blob per record, built once at index load: **7.0
ms** per search, **38.9 ms** to build, **6.4 MB** of cache. **It is deliberately NOT built here**,
because the real collection is 2,156 records where the cost is half a millisecond, and inventing a
cache for a scale nobody has is the mistake this campaign has avoided six times. LIB-S8 owns the scale
pass; if a 100k library ever appears, this is the shape of the answer and the numbers are already in
hand.

**Result order: artist, then album, then disc, then track, then path.** Deterministic, and it makes
the duplicate problem legible: LIB-AA measured 84 records for a 12-track album (seven format copies),
and this order puts those copies **adjacent** rather than scattered through the results.

## 3. Where results live - a fourth level

`libnav::Level` gains `Results`. This is the hedge from the very first roadmap doing its job for the
second time: depth arrived in LIB-S4 by extending an enum and a switch, and so does this.

**Why a browser level rather than the existing right-pane results view.** `\` renders into
`RightPane::SearchResults`, whose Enter *jumps the cursor* to a row in the current pane. That is
right for pane search and useless here - a collection result is not in the current pane, and the
whole point is to *act* on it. Reusing that pane would mean duplicating Enter, `a`, `q`, `*`, `b`
into it: new wiring, and precisely the "functions but you cannot use it" trap the brief warns about.

As a level, results are `dir_entries_` + `dir_display_` in lockstep like every other library level, so
**every operation LIB-S5 and LIB-AA wired works by construction** (§6), and `[Back]`/Left already
route through one ascent path.

`libnav::State` gains:

```
std::string query;           // the live query - a STRING, so results survive a rescan
Level       return_to = Level::Artists;   // where ascending from Results goes
```

`return_to` is set at entry: search from level 1/2/3 returns there; search from the folder browser
enters the section, so it returns to `Artists` and a second Left leaves - predictable, and no new
state beyond one enum.

**Results survive a rescan for free**, which is a gate item the brief lists: `populateLevel()` at
`Results` re-runs `search(index, query, ...)` from the held string. Nothing to invalidate. The same
property the artist/album levels have had since LIB-S4.

`libnav::reset` clears `query` and `return_to` along with the rest - both reset sites, unchanged in
shape.

## 4. Row format, cap, and the empty case

A result row must be actionable, and the brief is right that a bare title is not.

**`Artist - Title  [Album] (ext)`**, elided by priority: title and artist are kept longest, album
gives way first, **and the extension is never dropped** - it is 3-5 columns and it is the only thing
distinguishing seven format copies of one track from each other. Built by a pure function beside
`libnav::trackRowLabel`, so it is unit-testable like that one.

**Cap: 500 rows, with an honest count.** A one-character query matches 2045 of 2156 records
(measured), so an uncapped list is both useless and slow to draw. The header carries
`"N of M"` when the cap bites, so the user is told the list is partial rather than
left to assume it is complete. `search()` reports the true total through `total_out` - it counts every
match and collects only the first `limit`, so the count never lies.

**No match:** the pane shows one non-selectable row, `No match for "<query>"`, exactly as the empty
artist/album states already do. Not a toast - the pane is where the user is looking.

## 5. Interaction: LIVE, as you type (ruling 1)

`|` opens the shared input bar (`openInputBar`, a new `InputMode::LibrarySearch`) and the results
narrow on **every keystroke**.

**It extends the input-bar machinery rather than rebuilding it, which is why the ruling's
stop-and-raise condition does not trigger.** `handleGotoInput` mutates `goto_input_` at three places -
character insert, backspace, and the cursor keys - but has **one exit point**, a single
`redraw_needed_.store(true)` after the switch. So the hook is one insertion at one place and cannot
miss a mutation path:

```
if (goto_active_ && input_mode_ == InputMode::LibrarySearch && goto_input_ != lib_nav_.query) {
    lib_nav_.query = goto_input_;
    showLibrarySearch();
}
redraw_needed_.store(true);
```

**Coalescing falls out free, so no debouncer is built.** The `goto_input_ != lib_nav_.query` compare
means a cursor move, a Home/End, or a Tab re-queries nothing - only a change to the text does. That is
the whole of what a debounce would have bought at this scale, for one comparison.

The `goto_active_` guard matters: Esc and Enter both call `gotoClose` inside the switch, which clears
the bar, and without the guard the hook would fire afterwards and re-run the search on an emptied
query.

Esc closes the bar; the results stay on screen, which is what makes typing-then-acting one motion.

## 6. Which operations apply to a result row - each decided

A result row's identity is an absolute OS-origin path, exactly like a level-3 row, so the honest
default is that everything working at level 3 works here. Made explicit rather than inherited:

| key | at a result row | why |
|---|---|---|
| **Enter** | append, select, **play**, record as recent | The level-3 behaviour and the folder-browser precedent. This is the payoff of the whole slice: find it, press Enter, hear it |
| **`a`** | append without playing | Level-3 behaviour. Builds a playlist from several searches |
| **`q`** | enqueue | Level-3 behaviour |
| **`*`** | favourite | Level-3 behaviour |
| **`b`** | add to `[Books]` if it is an audiobook | Level-3 behaviour, re-gated on its own terms |
| **`u` / `x` / `;`** | mark / convert / chapters | Enabled by `browserEntryPath` resolving, as at level 3 |
| **`a` on a "whole album"** | **N/A** | A result row is a track, never an album. LIB-AA's album append stays `Albums`-only |
| **`\`** | searches the visible result rows | Unchanged and useful - narrow a wide result set |
| **Enter descend** | **no** | `Results` is a leaf: `libnav::descend` returns `None`, and Enter plays instead |
| **`F12`** | rescans, then re-runs the query | Falls out of `populateLevel()` |

Mechanically this is **one line**: `libnav::rowIsPath` becomes
`l == Level::Tracks || l == Level::Results`. That predicate is the single gate LIB-S5 built, so
everything in the table above turns on together and correctly - and the unit test that pins its
polarity covers the new level too.

**Levels 1-2 stay hard-excluded on safety**, permanently and unchanged: artist and album identities
are tag text, and invalid UTF-8 throws out of `fs::path`.

**Query text never becomes a path.** It is matched against fields and stored in `State`; nothing
constructs a path from it. Matched tag fields are display-only. The paths come from the index, which
is OS-origin.

## 7. Tests

The whole matching engine is pure, so most of this slice is machine-provable - unusually good for a
UI slice.

**`tests/library_index_test.cpp`** (extended) for `search`:
- term AND / field OR: a two-term query matching across two different fields hits; one term absent
  misses
- each of the six fields matches in isolation, and the filename stem matches for a record with no
  tags at all
- case-insensitive for ASCII; **non-ASCII is byte-exact, asserted as the stated limit** so the
  behaviour is pinned rather than accidental
- empty query, whitespace-only query, query longer than any field
- **the cap and the total**: with `limit` smaller than the match count, exactly `limit` records come
  back and `total_out` reports the true total
- result order is artist/album/disc/track/path, asserted against deliberately shuffled input, with
  format-duplicates landing adjacent
- **no subscript escapes**: every returned record's path is present in the index
- a planted invalid-UTF-8 tag field is matched and returned without throwing
- **scale**: the 100k synthetic case with a wall-clock assertion generous enough not to be flaky on
  a loaded CI runner, so a future change that makes this quadratic fails here rather than on Dos's
  hardware

**`tests/library_level_test.cpp`** (extended):
- `rowIsPath(Results)` is true, and the polarity mutation still fails
- entering search from each level sets `return_to`; ascending from `Results` returns there
- `reset` clears `query` and `return_to`
- the result row label: full form, elision priority, and that the extension survives elision

**Not machine-tested:** the key, the input bar, rendering, and every message. Gate.

## 8. Gate - eyes-on, both platforms

1. **The point of the slice, checked first:** from the folder browser, `|`, a query, and results
   appear from across the whole collection - not from the current pane.
2. `|` from library levels 1, 2 and 3, each returning to that level on `[Back]`/Left; from the folder
   browser, two Lefts land back in the folder browser.
3. Enter on a result **plays it**, and it appears in `[Recently Played]`. `a`, `q`, `*` behave; `u`
   marks and the `*` shows; `x` converts; `;` opens chapters on an MP4-family result.
4. A query matching nothing says so **in the pane**. A one-character query shows the cap and the
   `N of M` count rather than pretending to be complete.
5. A query matching a track that exists in several formats shows them adjacent and **distinguishable
   by extension**.
6. **Non-ASCII, planted and deterministic:** a query containing accented characters, and a result
   whose artist/album/title carry them, on both `wingui` and `ncursesw`. Plus a planted raw-Latin-1
   tag field, which must be found and drawn without an exit.
7. `F12` from a results list rescans and the results reflect the new index.
8. `\` inside `[Library]` still searches the visible pane at every level, including a results list -
   the capability that was deliberately not taken away.
9. `library=0`: `|` does nothing, anywhere.
10. Every other section still enters, draws, exits and plays; `\` elsewhere unchanged; `/` in
    `[Radio]` and `[Podcasts]` unchanged.

Machine: ctest both toolchains, `--no-tests=error`, currently 46/46 Windows and 47/47 Linux.
**Measured search latency at real scale in the debrief**, cold and warm.
Brace-balance and scoped-diff audit before handoff. **Verification split as in LIB-S4/S5/S6.**

## 9. Files expected to change

`include/LibraryIndex.h` (`search` + `detail::icontains` - the authorised query addition, no format
change), `include/LibraryNav.h` (`Level::Results`, `query`, `return_to`, `rowIsPath`, the result-row
label), `src/UIManager.cpp` (the `|` key, `InputMode::LibrarySearch`, `showLibrarySearch`, the
`populateLevel` case, the header, the operation branches), `include/UIManager.h`,
`tests/library_index_test.cpp`, `tests/library_level_test.cpp`, `CHANGELOG.md`, and this note as
design-of-record.

**Not touched:** `LibraryScanner.*`, `BrowserPins.h`, `Config`, `PlaylistManager` (LIB-S6's
`addIndexedTrack` is reused as-is - a result row is exactly the case it was built for), `AudioManager`,
the audio thread, `ar_crc`, the CD and rip paths, scrobblers, plugins, `Version.h`. No index format
change. `Ctrl+T` untouched.

## 10. Judged against the intent

The brief asks to be judged on whether a user can find their track, not on whether the words were
followed. Against that:

- **Finding** - six fields including the filename, substring, multi-term. The untagged-rip case is
  covered, which browsing cannot do at all.
- **Acting** - Enter plays. Every level-3 operation works, by one predicate rather than by new wiring.
- **Reaching it** - `|` from anywhere, not only from inside the section.
- **Trusting it** - the count never lies about how many matched; the extension distinguishes
  duplicates; a non-match says so where the user is looking.
- **Two honest limits, stated not hidden:** non-ASCII case folding is byte-exact (same as `\` today),
  and results are capped at 500 with the total shown.

One thing I would flag as a real gap that this slice does **not** close: there is still no way to
search **within** an artist or album, only the whole collection and the visible pane. On this
collection the whole-collection search plus `\` covers it; on a 100k library "everything by this
artist matching X" would want scoping. **Not proposing it here** - it needs a scoping UI and the
brief's non-goals rule out a filter language - but recording it rather than leaving it to be
discovered.
