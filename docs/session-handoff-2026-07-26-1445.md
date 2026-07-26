# Session handoff - 2026-07-26 14:45

Branch `experimental/win-pdcurses`. Everything from this session is committed and pushed.

**Six slices shipped this session: LIB-S9 through LIB-S14.** All CI-green, and **all
hardware-gated by Dos, including the five that were open at the previous handoff.**

`[1.5.0]` is the open cycle. `Version.h` reads 1.5.0, CHANGELOG heading is
`## [1.5.0] - Unreleased`, and it now carries fourteen slices of user-facing entries.

---

## NEXT SESSION STARTS HERE

**The library campaign is DONE through S14, and nothing is briefed.** Two items remain,
both raised and neither designed:

- **LIB-S15** - silent dupe denial from library to playlist.
- **LIB-S16** - library view navigation.

**Then the 1.5.0 ceremony, which is Dos's call and is not scheduled.** The release-flow
steps are in [[remoct-workflow]]; the short version is finalise the CHANGELOG date on
experimental, ff dev, `--no-ff` merge into main, annotated bare tag, push all three plus
the tag, and Dos does the clean-box build and the GitHub release himself.

**Read the CHANGELOG end to end before the ceremony.** Fourteen slices is a lot of
user-facing text written a slice at a time, and nobody has read it as one document.

---

## What shipped this session

| commit | slice |
|---|---|
| `3b40f50` + this one | S13 `Config` stat-key normalisation, S14 tag editing on browser rows |
| `cc56e47` | S12 - remove the per-handler scroll math |
| `f127f78` | S11 - multi-root library |
| `db9f482` | S10 - genre, stat views, scale, the stat-join fix |
| `23d0f52` | S9 - browse pane scroll-to-cursor |

Gates at the tip: **Windows 48/48, Linux 49/49.**

---

## THE THING WORTH CARRYING FORWARD

**The disconfirmation clause fired on FOUR of six slices, and one of those had already
survived a round trip.**

| slice | the stated premise | what the tree said |
|---|---|---|
| S9 | the browse pane's scroll fix was never written | it existed, implemented **step 2 of three**, and ran from four handlers but never from the draw |
| S10 | stats join on path | a byte-exact join matched **20 of 295** real entries; 17 files had their counts **split** |
| S11 | a list of roots is a format change invalidating every index | repeated `root` lines are compatible **both ways**; no version bump, no rescan |
| S14 | `TagEditability`, `PlayingLocked` and the sync loop assume a playlist entry | **none of the three did** |

**S14 is the one to remember.** That claim originated in a LIB-S10 debrief I wrote, which
inferred those three dependencies instead of reading them. It went into a brief as a
specification and came back as the reason the slice was split out as large. Neither side
read the code until the clause forced it, and the slice turned out to be two lines and a
resolver.

**An inference in a debrief becomes a fact in the next brief.** Write debriefs so that
inferences are labelled as inferences.

## The other lessons, shorter

- **A probe that models a different situation measures a different program.** S11's first
  probe printed `records after a scan with it missing: 0` with `removed = 619` - the exact
  disaster the slice prevents. The probe was wrong: it pointed at a *different* root path,
  simulating a root being **replaced** rather than a drive going **offline**. Both cases
  are in the probe output now so the distinction is visible.
- **Verify the mutation LANDED.** S13's first attempt at a mutation reported 0 failures and
  **had not applied** - a `perl` multi-line regex that silently matched nothing. Recorded
  as "the test survives removing the guard" it would have been a false confidence. Third
  time this step has paid.
- **Check what the fixture PRODUCED.** S10's 100k scale fixture set no `album_artist`, so
  every synthetic album was correctly flagged a compilation and `artists()` returned **one
  row** - S8's failure reproduced in the slice that documented it. The sanity assertion
  written in the same commit is what caught it.
- **The Linux gate catches real bugs, not just portability nits.** S11's `normaliseRoot`
  stripped `\` as a separator on both platforms; **on Linux a backslash is an ordinary
  filename character**, so a directory called `weird\` would have been renamed to nothing.
- **Only the pane the user is looking at may be repopulated.** S11 failed its hardware gate
  because `startLibraryScan` ended with an unconditional `populateLevel()` - correct for
  every caller until `@` let a scan start from the folder browser, where it wiped the
  listing. A background scan reports on the bottom-left line.

## Facts about this tree, updated

- **`libidx::detail::foldPathKey` is THE path-identity rule and now has four users**: the
  S10 play-stat join, S11's root comparison and `isPathUnder`, S13's key normalisation, and
  S14's playlist sync and index lookup. Case- and separator-folded on Windows, **identity on
  Linux**. Do not write a fifth rule.
- **`infoPaneSubject()` is THE info-pane subject resolver.** Before S10 the pane and the `e`
  handler each computed it with a verbatim copy of the same three-branch dance, which is how
  they could mean different files. One resolver, two readers.
- **`panescroll::ensureVisible` is THE cursor/scroll rule**, enforced at the top of both
  panes' draws. S12 deleted the last six per-handler copies. `ensureDirCursorVisible()` has
  exactly one caller.
- **Free ASCII punctuation, re-swept at S11:** `"` `#` `$` `&` `'` `(` `)` `:` `^`.
  Taken since: `|` (S7 collection search), `%` (S10 stat views), `@` (S11 add library
  folder). **`?` is NOT free** - it is the Help pane, via a pre-switch `if`, and two
  successive surveys said otherwise. Sweep `case` labels **and** pre-switch `if`s.
- **`remoct.conf` now carries repeated `library_root=` lines**, and Dos's real config has
  two: `C:\Users\david\Music` and `D:\Music`.
- **The stat keys in `remoct.conf` are normalised as of S13**, and a `remoct.conf.statbak`
  is written once beside it the first time that migration runs.

## Measured numbers, so nobody re-derives them

| thing | real collection | 100k synthetic |
|---|---|---|
| two-root first scan (2,775 records) | **14.8 s cold**, 649 ms warm | - |
| rescan, nothing changed | **220 ms** | - |
| index size, two roots | **0.48 MB** | - |
| `genres()` | 0.08 ms, **27 rows** | 53.5 ms |
| `mostPlayed()` | 0.86 ms, 219 of 219 | 46.3 ms |
| `neverPlayed()` | 0.36 ms, **500 of 1,937 (89.8%)** | 27.8 ms |
| `buildPlayStats()` | 0.05 ms (295 raw -> 278 folded) | 4.7 ms |
| stat merge | 295 -> 278 entries, **3901 -> 3901 plays** | - |

`D:\Music` holds **619** audio files by `kAudioExts` - the brief said 618, from a debrief
that omitted `.mp4`.

## Known and accepted, not defects to rediscover

- `Pop-Rock`, `Post Punk` and `Post-Punk` are separate genres. Any rule merging them breaks
  `Hip-Hop`, which is 49 records and seven times as common.
- Genre folding splits on `/` and `;` only. `Folk, World, & Country` and `R&B` survive
  intact because commas and ampersands are not separators.
- Podcast episodes refuse tag editing. The download manager owns those files - `d` deletes,
  a re-download replaces, and identity is the episode id not the path.
- Editing tags does **not** rewrite the index file; the in-memory index is updated and a
  rescan persists it. `mtime`/`size` are left stale on purpose so the rescan re-reads that
  one file.
- No backup is taken of an edited music file. S13 backed up `remoct.conf` because that
  migration touched a file the user never asked us to touch; S14 is one file the user chose.
- Five `stat=` keys point into `C:\Users\david\smoke\files\` - LIB-S6 launch-smoke
  artifacts. They survive migration under the no-drop policy and are **not** plays Dos made.

## Process rules in force

- **Push is a separate permission from commit.** Dos said "commit and push" explicitly each
  time this session; do not infer it.
- **Design note first**, then greenlight, then build, then debrief, then commit on his word.
- **Plan docs commit WITH their slice** as design-of-record. A not-yet-greenlit note stays
  untracked - S13's did, for exactly one turn.
- **Verification split in every debrief**: what was RUN and green, versus what is the
  hardware gate's to confirm.
- **Mutation-test new tests, and verify the mutation landed** before trusting either
  outcome.
- **Do not report on Dos's gate backlog.** He tracks it; saying it back to him is noise.
