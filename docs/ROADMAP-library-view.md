# ROADMAP - Library view (core module, config-toggled)

Status: **IN PROGRESS - this is CURRENT WORK, shipping as 1.5.0.**
Written 2026-07-25 as a banked candidate; promoted to current work on 2026-07-26.
HTOA does not preempt it, and HTOA's release number is undetermined.

**Current state is the shipped-slice table near the end of this document, not this
header.** Slices 1 through 9 are built; the remaining work and its numbering are
tabled there. The stale progress note that used to sit here said "slices 1, 2 and 3",
which was true for about six hours.

Two questions this document raised were **closed by Dos's decisions of 2026-07-25**: the
fork in section 8 is resolved to (b) with the hedge, and Enter on a library track row
appends to the playlist (section 5). Both are marked in place.

**One slice was added that this plan did not anticipate.** Slice 3's hardware gate failed
on row placement: `[Library]` was pushed into the pane correctly and then the SORT moved
it to the bottom, because the comparator kept its own hand-maintained copy of the pinned
order and nobody had added the new row to it. The fix was one pin; the lesson was that the
order lived in two lists that had to agree. `include/BrowserPins.h` now holds it once, both
readers consume it, and `browser_pins_test` asserts the rendered sequence. Sequenced before
slice 4 deliberately - slice 4 adds section surface on top of that structure.

Design of record for the capability question: recon debrief `library/recon-abi-capability`.
Its verdict is settled and is not re-derived here: the plugin ABI is an audio-source
interface with no content-provider concept, so library view is a **core module behind a
config toggle**. Identity is preserved the Classic/Awesome way - a core toggle, not plugin
isolation.

Reference design is cmus (scan once, tag-read, persist an index, revalidate by mtime,
present the hierarchy as a sequence of flat lists). The implementation is ours: our index
format, our cache, our section grammar. We are not porting cmus.

---

## 1. Scope and non-goals

**In scope.** A `[Library]` sidebar section that browses the collection by artist, then
album, then track, independent of folder layout. Off by default. A folder-player user who
never enables it sees one extra sidebar row and pays no scan cost.

**Non-goals - these hold the line against creep.** Every one of these is a thing a library
feature naturally grows toward, and each is excluded deliberately:

- **Not a replacement for the folder browser.** The directory pane stays primary. Library
  is an alternative view over the same files, never the only way to reach them.
- **No watch daemon.** No inotify, no ReadDirectoryChangesW, no background rescan timer.
  Revalidation happens on explicit rescan and on section entry, nowhere else. (The
  podcast campaign fenced auto-refresh for the same reason, and slice 5 proved that
  scanning on section entry is itself a regression risk - see §3.)
- **No network metadata.** The index is built from local tags only. No MusicBrainz,
  Discogs, or cover-art lookups during scan. Art in the library view reuses the existing
  on-demand paths or shows nothing.
- **No tag editing from the library view.** The tag editor already exists on the folder
  side; library rows are read-only. Editing would mean write-back plus index invalidation,
  which is its own campaign.
- **No smart playlists, no filters, no saved queries.** Artist/album/track and the two
  free stat views (§7) are the whole surface.
- **No cross-device or remote libraries.** One local music root.
- **No MPD-style database server, no IPC, no daemon.** The index is a file this process
  owns.

---

## 2. Slice 1 - metadata index and on-disk cache format

The riskiest new piece, and the one to prove first. It is **provable standalone before any
UI wiring**, the same discipline as the podcast RSS parser: build the pure thing, test it
against real inputs, integrate only once it holds.

**Shape.** A pure, header-inline module (`include/LibraryIndex.h` or similar) that owns:
serialise an index to a byte buffer, parse one back, and answer the hierarchy queries the
UI needs (distinct artists; albums for an artist; tracks for an album). No filesystem, no
curses, no audio - so it links into a test with no device and no seam, exactly as
`PodcastFeed.h` and `PodcastChapters.h` do.

This also satisfies the repo norm that a test never links a heavyweight TU: keep the logic
header-inline and pure, and the test links nothing else.

**Record fields (minimum).** Absolute path, artist, album, album-artist, title, track
number, disc number, year, genre, duration, file mtime, file size. mtime and size are the
revalidation key (§3), not decoration.

**Format.** Versioned and line-oriented, mirroring how the project already persists things
(`remoct.conf`, `podcast=`, `pod_ep=`): a header line carrying a format version and the
music root, then one tab-separated record per track, with tab and newline escaped in field
values. Debuggable by eye, cheap to parse, and a version mismatch means "discard and
rescan" rather than a migration. A 20k-track index in this format is a few megabytes,
which is not a problem worth a binary format's opacity.

Cache location: beside `remoct.conf` in the config directory (`Config::configPath()`'s
directory - the same idiom `theme.conf` already uses), not in the music root. The music
root is the user's, and we do not litter it.

**The trap that will bite this slice - CORRECTED 2026-07-26 (LIB-S2).** This paragraph
used to say `fs::path` throws for any byte CP1252 cannot map, i.e. for non-ASCII. That was
reasoned, not measured, and it is wrong. Measured on both toolchains:

**libstdc++ on Windows treats a narrow `fs::path` string as UTF-8, so the trigger is
INVALID UTF-8, not non-ASCII.** Valid UTF-8 of any kind - accents, smart quotes, CJK,
4-byte emoji - constructs fine, round-trips byte-exact, and finds real files. A raw
Latin-1/CP1252 byte, a lone continuation byte or a truncated sequence throws
`Illegal byte sequence`, from `fs::path()` **and** from `fs::exists(s, ec)` (the
conversion runs before the error code applies). Linux never throws; bytes pass through.

The old wording could not explain why `UIManager.cpp`'s `fs::directory_iterator(current_dir_)`
has always worked over a collection with 137 non-ASCII paths. The corrected rule explains
that **and** the slice-5 crash: slice 5 built a path out of **feed title text**, which
carries raw Latin-1 bytes.

**Therefore: paths that come from the OS are safe; paths built from feed or tag text are
not.** Index paths are still strings start to finish - built by string append, compared and
persisted as strings - and where a real filesystem call is unavoidable, construct
`fs::path` from `utf8_to_wide(...)` so no narrow-to-wide conversion can run. But an
OS-origin walk may use `std::filesystem` directly, which is what LIB-S2 does. Full detail
and the measurement table are in `docs/library-index-plan.md` section 6.

**Proving it.** A unit test over hostile input - truncated file, wrong version header,
embedded tabs and newlines, non-ASCII throughout, missing fields, absurd durations,
duplicate paths - plus a real scan of the actual music collection compared against a
known track count. The podcast parser's fixture approach applies directly.

---

## 3. Slice 2 - background scanner and incremental revalidation

**Threading.** Off the UI thread, using the idiom this codebase has already proven five
times over (podcast fetch, podcast search, feed art, episode art, chapters):
`std::atomic<bool> active_` / `std::atomic<bool> done_` / a `std::mutex` guarding the
result / a want-key for latest-wins staleness / a `std::thread` joined in the destructor,
picked up per-frame by a `poll*()` called from the main loop. Anchors:
`podcast_fetch_active_`, `podcast_fetch_done_`, `podcast_fetch_mtx_` in `UIManager.h`.
Nothing here needs a new concurrency pattern, and inventing one would be the mistake.

**Cancellation is mandatory, not optional.** A scan of a large collection must be
abortable, or quitting mid-scan hangs the process on the destructor join. The podcast
download worker's cancel flag is the precedent.

**Incremental revalidation.** A file whose path, mtime, and size all match its index record
is not re-read. Everything else is: new files get tag-read, changed files get re-read,
records whose files have vanished get dropped. That comparison is the entire reason mtime
and size are in the record.

**When revalidation runs - the slice-5 landmine.** Scanning on section entry is exactly
the regression that broke the podcast feed-load path: work added to a load path that had
been fine, on a collection large enough that the cost only appeared at real scale. So:
revalidation runs on **explicit user rescan** and **first enable**, never implicitly on
every `[Library]` entry. If a cheap entry-time freshness check is ever wanted, it is
point-of-use only and bounded - the podcast chapters slice's "check at point of use, never
scan on enter" rule, which was written after paying for the alternative.

**Tag reading.** Core already links TagLib across roughly ten translation units; the
scanner calls it directly. There is no seam to design and no second TagLib to link - that
question only existed on the plugin route, which is closed.

---

## 4. Slices 3-4 - the `[Library]` section, levels 1 to 3

**The mechanism already exists and is confirmed live.** `dir_entries_` and `dir_display_`
(`UIManager.h:250-251`) are parallel `std::vector<std::string>`: identity and display are
already separate. Every existing virtual section populates them in lockstep -
`dir_entries_.push_back(<real path or url>)` alongside
`dir_display_.push_back(sanitizeForDisplay(<formatted label>))` - across `[FAVs]`,
`[Radio]`, `[Books]`, the drive list, and radio search results. A library row showing
"Artist - Title" while holding an absolute path is the idiom, not an extension of it.

**Drill-down is a proven pattern, extended by one level.** `[Podcasts]` already does
feeds then episodes, via a second mode flag (`in_podcast_feed_`) and dedicated repopulate
functions (`showPodcastFeedList`, `showPodcastIndexResults`). Library is that at three
levels: artists, then that artist's albums, then that album's tracks. Same shape, one more
step, plus the `[Back]` sentinel each level already pushes into both vectors identically.

**What each level actually is.** A flat list produced by an index query. The UI holds the
current level and the selection path that led to it (artist, then album); Enter descends,
`[Back]` and Left ascend. No tree rendering, no hierarchical widget - the hierarchy lives
in the index, and the pane only ever draws a flat list.

**The wiring tax is the real cost**, and it is where the fork in §8 bites. Every existing
section flag costs branch sites across sorting, header text, `is_dir` and pseudo-entry
handling, `browserEntryPath`, the exclusion chains, refresh and enter-reset paths, and the
draw loop. Measured on the live tree: `in_podcasts_` 29 sites, `in_drive_list_` 29,
`in_radio_` 25, `in_favs_` 21, `in_recent_` 19, `in_books_` 15.

**The reset trap.** `refreshDir()` and `enterDriveList()` reset the section flags, and the
podcast slice-2 brief missed exactly this - the fix was caught by reasoning about the Left
arrow, not by the gate. Any new mode flag must be added to both reset sites or a refresh
leaks a broken section state.

---

## 5. Slice 5 - playback and queue integration

**Confirmed solved; this slice wires, it does not design.** `browserEntryPath` resolves an
absolute entry straight through, which is why `[FAVs]` - a formatted display row holding an
absolute path - plays on Enter with no special handling. A library track row is the same
thing. No new playback plumbing, no audio-thread involvement, and nothing in the rip or CD
path is touched.

What this slice does cover: Enter plays, queue keys work on a library row, marking and
converting behave or are excluded deliberately, and the focus-aware `\` search works at
each level (it reads `focus_` and searches `dir_display_`, so it should work by
construction - confirm rather than build).

**DECIDED (Dos, 2026-07-25): Enter on a library track row APPENDS TO THE PLAYLIST**, the
folder-browser precedent, not standalone playback (the podcast precedent). Rationale of
record: it works best with making and loading playlists. This was an open UX question when
this roadmap was drafted; it is closed, and slice briefs should not re-open it.

---

## 6. Slice 6 - toggle, rescan key, first-run experience

- **Config key**, defaulting **off**. Off means the section is absent and no scan ever
  runs. The `crossfade` key's ruling is the precedent: default to the behaviour that
  changes nothing for the existing user.
- **Music root.** Reuse `CDRipper::musicRoot()` (the known-folder lookup the rip and
  podcast paths already use) as the default, with a config key to override. Memoize it -
  it is a COM call, and calling it per row per frame was the suspected cause of a
  scroll-time crash during the podcast campaign.
- **Rescan key**, scoped to the section, with progress surfaced the way the rip and podcast
  download progress already are (`rip_status_` / `podcast_dl_status_` on the command line),
  not a modal.
- **First run.** Enabling the toggle triggers the first scan, and it must be visibly
  in-progress and cancellable, never a frozen UI. An empty index with a scan running is a
  normal state the section must render honestly ("scanning, N tracks so far"), not an
  error.

---

## 7. Slice 7 - genre, album-artist, compilations, and scale

**Compilations are the correctness problem**, not a nicety. Grouping purely by artist tag
shatters a compilation into one album per track. Album-artist is the standard fix, with a
fallback chain: album-artist if present, else artist, with "Various Artists" recognised.
Any album whose tracks disagree on artist but agree on album-artist is one album. This
needs deciding against real files in the collection, not in the abstract.

**Genre** is a third top-level entry point over the same index - cheap once the index
exists, and worth keeping behind the same toggle.

**Two views come nearly free** from data already persisted: `TrackStats { play_count,
last_played }` keyed by path (`Config.h`, `stat=<path>|<count>|<epoch>`) gives most-played
and recently-played with no new storage. Recently-added falls out of the mtime already in
each index record. These are index queries, not features.

**Scale.** The numbers to hold: a 20k-track first scan is bounded by tag-read throughput
and must stay cancellable and off the UI thread; drawing is already O(visible rows) and
unaffected; the index must not be re-serialised on every mutation. Measure a real scan
before promising a number.

---

## 8. THE FORK - RESOLVED (Dos, 2026-07-25): (b) seventh flag, with the hedge

Dos ruled for **(b)**, including the hedge, on the rationale set out below. The refactor in
(a) is not scheduled; the trigger for revisiting it stands as written. The rest of this
section is kept as the reasoning of record, not as an open question.

A three-level library section lands in the same branch-site territory as the existing six
(~15-29 sites each, ~138 total today). Two ways forward.

### (a) Refactor first

One preliminary slice collapsing the six hand-coded section flags into a single
virtual-list mechanism - a section descriptor owning its own populate, header, enter, and
exclusion behaviour - then library becomes a registration rather than a seventh scatter.

- **Upside:** paid once; every future section is cheap; cross-cutting changes (the kind the
  focus-aware `\` search was) touch one place instead of six.
- **Cost and risk:** it rewrites the plumbing of six shipped, hardware-gated features -
  radio, podcasts, books, favourites, recent, drives - for **zero user-visible gain**. Every
  one of them needs re-gating on both platforms, and several are network- or
  hardware-dependent and cannot be fully machine-tested. The regression surface is the
  entire existing sidebar.

### (b) Seventh flag

Add library the established way, no refactor.

- **Upside:** additive; touches no working section; each site is a known pattern already
  written six times. Regression risk is confined to new code.
- **Cost:** roughly 25-40 new branch sites, and the maintenance drag grows rather than
  shrinks.

### Recommendation: **(b), with a hedge**

Three reasons.

1. **The refactor's payoff is speculative and library is probably the last big section.**
   Radio, podcasts, books, favourites, recent, drives, library - that is the full set a
   player of this shape needs. "Paid once, every future section cheap" only pays if more
   sections follow, and none are on the roadmap.
2. **It violates a lesson this project already paid for.** Slice 5 of the podcast campaign
   added speculative hardening to a working path and introduced a feed-load regression;
   the recovery was to revert to the proven behaviour. Rewriting six working sections to
   make a seventh tidier is that mistake with a larger blast radius.
3. **Refactoring after the three-level case is better informed than before it.** Library is
   the first section with three levels. Building it teaches what a general mechanism
   actually has to express; doing the refactor first means designing that mechanism
   against six two-level sections and guessing at the third.

**The hedge, which costs almost nothing:** build library's three levels behind one small
internal level-descriptor used *only by library* - current level, its populate function,
its header text, its enter and back behaviour. It keeps a three-level section from costing
three times a two-level one, and it becomes a working prototype of the general mechanism
if (a) is ever revisited. It does not touch a single existing section.

**Trigger that would flip this to (a):** a decision to add further virtual sections after
library, or a cross-cutting change that has to touch all seven and proves painful. Either
makes the refactor's payoff real rather than hypothetical. Worth revisiting then, with the
three-level case in hand.

**How the slice plan shifts under (a):** insert a refactor slice at position zero and add a
full re-gate of all six existing sections on both platforms, including the hardware-gated
ones. Total becomes 8-9 slices, and the first of them ships no user-visible change - which
also means it cannot be validated by a user-facing gate, only by "nothing broke."

---

## 9. Slice sequencing

Riskiest and most-provable-in-isolation first. Planned at 7 slices under path (b).

**IT CAME OUT AT 10, and the three extra were all discovered rather than planned** - the
estimate was not wrong about the work, it was wrong about what the work would turn up:

- **3b** pinned-row consolidation, inserted when slice 3's gate failed on a second list of
  section names that had to agree with the first
- **AA** append-whole-album, raised out of slice 5 rather than absorbed, because it is the
  first operation touching many playlist rows at once
- **the 7/8 split** - the original slice 7 became collection-wide search (a capability the
  original scope MISSED entirely: `\` searched the current pane, which is right for a
  folder browser and wrong for a library), and the original slice 7's contents became
  slices 8 and 9

The honest read: a seven-slice estimate for a campaign of this shape was about 30% light,
and every overrun came from something the plan could not see from the outside.

| # | Slice | Why here |
|---|---|---|
| 1 | Index format + hierarchy queries (pure, header-inline) | Riskiest, and fully provable with no UI, no device, no seam |
| 2 | Background scanner + mtime revalidation + cancel | Needs slice 1's format; proven async idiom; still no UI |
| 3 | `[Library]` section, level 1 (artists) | First user-visible slice; pays the section-flag tax once |
| 3b | Pinned-row consolidation (unplanned - see the header) | The section-flag tax proved fragile; consolidate before adding more |
| 4 | Levels 2-3 (albums, tracks) + `[Back]`/Left | Extends slice 3; the `in_podcast_feed_` pattern at depth |

**What slice 3 actually cost, measured rather than estimated.** The plan called the
seventh-flag tax "a known pattern written six times". It is not: of roughly 25 branch
sites, **six were crash-or-corruption**, because existing code builds `fs::path` out of
`dir_entries_` values and a library row is an artist string from a tag, which may be raw
Latin-1. The worst is in the DRAW LOOP, where it would throw every frame. The exclusion
chains are correctness work. This does not reverse the fork - path (a) would have put six
shipped sections at that risk instead of confining it to new code - but slice 4 should
budget for it rather than expecting bookkeeping.

**Slice 3 inherited one open question from slice 1** (design of record:
`docs/library-index-plan.md` section 8). `tracksForAlbum` returns indices into the index,
and those are valid only against the index instance that produced them.

**ANSWERED in slice 3, and the answer is that the question dissolves.** The UI holds
identity STRINGS and never indices. A query result is consumed inside the one populate call
that asked for it and never survives a frame, so there is nothing to invalidate and no
generation counter is needed. The cursor is restored by NAME through
`libidx::restoreCursor`, so a scan completing underneath a live selection re-seats on the
same artist rather than on whatever row that subscript now denotes.

Slice 4 must keep this true at depth: a track row holding an index into the album's track
vector would reintroduce precisely the problem three slices avoided.
| 5 | Playback, queue, `\` search at each level | Wiring over a confirmed-solved path |
| 6 | Toggle, music-root config, rescan key, first-run UX | Makes it shippable and ignorable |
| 7 | Album-artist/compilations, genre, stat views, scale pass | Correctness and polish against the real collection |

Comparable in total to the podcast campaign (6 slices), but **front-loaded**: slices 1-2
carry most of the difficulty and produce nothing a user can see, which is a scheduling fact
worth stating up front rather than discovering at the midpoint.

**Gate character.** Slices 1-2 are machine-gateable in full (pure logic plus a headless
scan over the real collection). Slices 3-7 are eyes-on, as every UI slice in this project
is. The first three slices can therefore run faster than the podcast campaign's did; the
back half cannot.

---

## Sequencing and status

**SUPERSEDED 2026-07-26. Library view is CURRENT WORK and ships as 1.5.0.** It is not a
1.6.0 candidate, it is not post-HTOA, and it is not unscheduled. HTOA and CTDB repair are
**parked with an undetermined release number**; they do not preempt this campaign.

The paragraph this replaces said the opposite of all three, and it said so in
design-of-record. It is recorded here rather than deleted because the correction is the
point: a wrong premise in a roadmap costs something in every slice that reads it, and this
document was read at the start of slices 1, 2 and 3.

**Shipped so far, all pushed:**

| slice | commit | state |
|---|---|---|
| 1 - index format + queries | `c6287ee` | CI green |
| 2 - background scanner | `5152b46` | CI green |
| 3 - `[Library]` level 1 | `30b8d40` | CI green, eyes-on passing |
| 3b - pinned-row consolidation | `30b8d40` | CI green, eyes-on passing |
| 4 - levels 2-3 + the one ascent path | `0eb7cd6` | CI green, eyes-on passing |
| 5 - playback, queue, `\` search | `bf79789` | CI green, eyes-on passing |
| AA - append whole album | `fcadcf9` | CI green, eyes-on passing |
| 6 - toggle, music root, F12 rescan, Esc cancel | `b955fe6` | CI green, eyes-on passing |
| 7 - collection-wide search (`\|`) | `bba2c8e` | CI green, eyes-on passing |
| 8 - compilations + `artists()` scale fix | `fed8658` | CI green, eyes-on OPEN |
| 9 - browse pane scroll-to-cursor | `23d0f52` | CI green, eyes-on OPEN |
| 10 - genre, stat views, scale, the stat-join fix | `db9f482` | CI green, eyes-on OPEN |
| 11 - multi-root library | this commit | gates green, eyes-on OPEN |

**RENUMBERED 2026-07-26.** A correctness defect found on hardware took the number
LIB-S9, so what this document previously called slice 9 is now **LIB-S10**. The
campaign came out at 10 planned slices and is now running past that, for the same
reason it ran past 7: the overruns are things the plan could not see from outside.

**Remaining:**

| # | Slice | State |
|---|---|---|
| **S12** | cleanup: drop the four redundant `ensureDirCursorVisible()` handler calls | raised by S9 - once the draw-time invariant exists they are redundant but not wrong, and deleting working hardware-gated call sites was out of scope under additive-only. See `docs/library-slice9-plan.md` §3. |
| **S13** | normalise `track_stats` keys in `Config`, merge the split pairs | **raised by S10 §0.** S10 folds case on the READ side, which fixes both views and the info pane. This fixes the STORE: `recordPlay` keys on whatever path the playlist entry held, so one file accumulates two tallies. Mutates persisted user data, so it wants its own gate and rollback story. S10's read-side fold is not wasted by it - old configs still need the fold until they migrate. |
| **S14** | tag-EDIT a browser row | **narrowed by S10.** The display half shipped in S10: `infoPaneSubject()` makes `i` follow the browser cursor everywhere, and `e` refuses a browser subject rather than writing the wrong file. S14 is the write half - what `TagEditability`, `PlayingLocked` and `saveTagEdits`'s playlist-sync loop mean for a file that is not in the playlist. |

**Then the 1.5.0 ceremony**, which is Dos's call and is not scheduled.

`Version.h` reads 1.5.0; the CHANGELOG heading is `## [1.5.0] - Unreleased`.

**Anchor provenance.** The anchors in this document were verified at tip `6d40021` on
2026-07-25. Re-verified against tip `30b8d40` on 2026-07-26, for slice 4's design note
only: `dir_entries_`/`dir_display_`, the populate idiom, `browserEntryPath`, `LibLevel`
and the `lib_*` members, the `libidx` query surface, and every `in_library_` site. The
async worker idiom, `TrackStats`, and the per-section branch-site counts were **not**
re-read on 2026-07-26 and still date from `6d40021`. Re-verify before building - this
document ages the same way every other one does.
