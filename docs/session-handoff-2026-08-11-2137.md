# Session handoff - 2026-08-11 21:37

Branch `experimental/win-pdcurses`. Continues `docs/session-handoff-2026-08-09-1933.md`, which was a
recon session that touched no tracked source. **This one ships code.**

**1.6.2 is version-bumped and committed on `experimental/win-pdcurses`. It is NOT merged to `dev` or
`main`, and NOT tagged.** Two items, both confirmed on hardware by Dos before the commit.

---

## NEXT SESSION STARTS HERE

1. **Nothing is running, nothing is half-done.** Both items are done, gated on both toolchains and
   live-tested. The only thing outstanding for 1.6.2 is the release ceremony (§6).
2. **READ THE CONFIGURE BANNER before trusting any build.** Still the cheapest mistake available.
3. **The three C2 decisions from the 08-09 handoff §4 are still open.** Nothing this session touched
   them. They are unchanged and still Dos's call.
4. **`docs/CLAUDE.md` is at 201/200 lines.** It was at 201 before this session and is at 201 after -
   the 1.6.2 paragraph was paid for by folding the standalone 1.6.1 paragraph into `**Released:**`.
   **There is no headroom. Anything added must displace something.**
5. `docs/LOCKED-CODE.md` before anything near the CD path, with the declaration line. Neither item
   this session went near it, and both reports said so up front.

---

## 1. What shipped

### 1.1 The Classic radio scanner stopped borrowing the visualizer palette

`UIManager::drawProgress`. The KITT sweep on the radio status line picked its colour per cell from
`CP_VIZ_PEAK/HIGH/MID/LOW` **in both modes**. Now Awesome keeps exactly that and Classic draws the
whole sweep in one `CP_TITLE`, shaded by the glyph ramp alone. Glyphs, head, tail length and timing
are byte-for-byte unchanged.

**The brief's hypothesis was wrong in an instructive way and the report said so before writing
code.** The hypothesis was "it uses a fixed pair / ignores the theme". It does neither: `CP_VIZ_*`
are re-inited per mode and `Ctrl+T` re-runs `initColours()`, so by the usual test - *does it consult
the theme?* - the widget looked correct. **The defect was the wrong palette ROLE, not the wrong
palette.** Full mechanism in `docs/lessons.md` "Colour-pair roles".

Two facts made the fix obvious rather than arbitrary, and both are worth keeping:

- **It was the only per-cell-coloured animation in that row.** Everything else in `drawProgress` is
  one `wattron` for the whole draw.
- **Awesome's own comet bar is monochrome** - its gradient is glyph density inside a single
  `COLOR_PAIR(CP_PROGRESS)`. So is `[#---]`. **The mode difference in that row has never been colour
  count**, so "one pair, shaded by glyph" is Classic's existing answer, not a new invention.

**It had no `awesome_mode` gate because the stream branch RETURNS above the mode branch.** A
`grep awesome_mode` shows `drawProgress` as gated - it is, for the two paths below the early return.
**When auditing "is this mode-aware", check what returns before the gate.**

`CP_PROGRESS` was rejected for Classic: it is white-on-**blue** there and these are solid blocks, so
it would have painted a slab across the idle gap - a bar where there is no bar.

**No 14th theme role was added.** `theme.conf` names 13; a palette entry for one widget is the wrong
trade when the technique was already in the file twice.

### 1.2 The most-played playlist track shimmers

New `UIManager::sparkleWinners()` + a per-row branch in `drawPlaylist`, and a new
`PlaylistManager::contentRevision()`.

**Playlist-scoped, and the reason is a fact rather than a preference:** `Config::track_stats` counts
any file played through the transport; `LibraryIndex` only covers configured `[Library]` roots after
a scan. A library-scoped winner would be absent from most playlists and would do nothing at all,
silently, for anyone who never set `[Library]` up.

**Two suppression rules, and they are the same rule at opposite ends of the range:**

- **Floor `max > 0`.** A fresh playlist is all zeros, so every row would tie. No higher threshold - a
  new user seeing nothing for weeks with no explanation is the worse failure.
- **Tie cap of 3.** An album played through eight times has every row tied at 8, and forty shimmering
  rows carry exactly as much information as none. **This is not a tiebreak** - it never picks a winner
  among tied tracks. Two or three tied rows all sparkle.

**Where it does not apply, and why none of it needed a special case except one:**

| | counts? | sparkles? | mechanism |
|---|---|---|---|
| Radio / streams | no | never | `recordPlay` is skipped for stream rows; count is always 0, the floor does the rest |
| CD rows | no | never | `recordPlay` returns early on `isCDTrackPath` - *"CD tracks are volatile"* |
| Podcasts | yes | **yes** | ordinary files on the normal transport path; no `isPodcastPath` exists and inventing one is scope for no gain |
| Audiobooks | yes | **NO - the one real predicate** | `config_.isSavedBook()` |

**The audiobook case is the one that would have lied.** `recordPlay` fires on every current-track
change, so *resuming* a book inflates it: a book picked up forty times would outrank a song someone
loves. `isSavedBook` is a scan over <=200 entries - fine once into the cached set, not fine per row
per frame, which is the second reason the set is cached at all.

**Theme-appropriate, and per Dos's ruling they are genuinely different idioms rather than one effect
tinted twice:**

- **Classic:** the whole row pulses `A_BOLD` <-> `A_NORMAL`. The CGA answer. **Deliberately not
  `A_BLINK`** - depending on PDCursesMod wingui's handling of it is not something to build on.
- **Awesome:** **per character.** Each character rides its own phase through the theme's viz hues, so
  the row glitters rather than breathes.

**Both at half the text-scroll rate** (`text_scroll_offset_ / 2`, ~600ms). Dos left Classic's rate to
me: I halved it too. At 1.7Hz the row pulse read as a flash; at 0.83Hz it reads as a breath.

---

## 2. The three things in item 1.2 that were nearly built wrong

Keep these. Each one is a plausible implementation that would have shipped a worse feature.

**2.1 - A `+1`-per-column phase is a WAVE, not a sparkle.** The cycle travels along the row and reads
as one effect moving. The fix is a stable pseudo-random scramble of the character index
(`sparkleCellPhase`, a bit-mix keyed on **playlist index** and column).

**2.2 - A hash that took the FRAME as input would be STATIC.** Every cell would re-roll every step
and read as noise on a broken signal. **Only the shared beat may move; the pattern underneath must be
fixed** - glitter is light catching a *fixed* surface, and the fixedness is what makes it read as a
surface at all. This is the half that is easy to get wrong, because "random per cell" sounds like the
whole requirement.

**2.3 - A ramp with every step lit is CONFETTI.** Every character sits on some hue at once and the
title stops being readable. `kTwinkle` rests at the row's own pair for three beats of eight, so at any
instant most characters are at the row colour and a scatter are lit.

Keyed on the **playlist index, not the screen row**, so two sparkling rows decorrelate and neither
pattern crawls when the pane scrolls.

---

## 3. Two mechanism facts that cost nothing to know and a lot to rediscover

**3.1 - `drawAnimatedPanes()` deliberately EXCLUDES the playlist and dir panes** (*"they don't
animate"*). So the 80ms marquee tick is NOT what repaints the playlist. **But `text_scroll_offset_`
advances every ~300ms and sets `redraw_needed_` unconditionally**, which drives a full `drawAll()` -
so the playlist pane already repaints ~3.3 times a second in both modes, playing or idle. **The
shimmer therefore costs zero new frames.** Anything wanting a faster animation in that pane must force
redraws at the 80ms tick, which buys a strobe rather than a shimmer.

**3.2 - A pair's BACKGROUND matters as much as its foreground the moment the widget changes what kind
of thing it draws.** Both items hit this, in opposite directions:

- Item 1: `CP_PROGRESS` is fine for text and paints a **slab** when the glyphs are solid blocks.
- Item 2: the solid `CP_VIZ_*` pairs are **fg==bg**, so text drawn in them is invisible. A hue cycle
  on a text row must use the `_B` variants and `CP_VIZ_TIP`, which exist precisely as the text-safe
  form of those hues.

A useful corollary fell out: the row's padding spaces take a twinkle pair too and are **unaffected**,
because every pair in the ramp shares `CP_DIM`'s base bg in Awesome and a space paints bg only. The
twinkle shows on glyphs and nowhere else - which is what "per character" should mean.

**Per-cell drawing and fullwidth glyphs:** `wmove` ONCE then `wadd_wch` sequentially, letting curses
advance by each glyph's own width. `column = cx + codepoint_index` is the column-vs-byte trap in a new
costume; the two diverge on the first wide glyph and a CJK title would scatter its own characters.

---

## 4. New API - read the contract before reusing it

**`PlaylistManager::contentRevision()`** - a membership fingerprint, bumped by the seven mutations
that change the SET of entries and **deliberately NOT by a reorder**. Sort, move, shuffle and a
display-title refresh leave it alone, because the set is the same set.

**That is the contract, not an oversight.** The caller it exists for caches folded PATHS, which a
reorder cannot invalidate. **Do not widen it** to mean "did anything change" - widening it silently
makes the sparkle recompute for an answer that cannot have changed. If a future caller needs "did the
rows move", that is a different question and wants a different counter. The header says all of this at
the declaration.

A dedup'd add is not a change: those paths return early, above the bump.

**One case ruled on unasked:** while a directory load is draining, the sparkle is suppressed and the
cache **re-arms** rather than caching the empty answer - otherwise the final drain would leave
"nothing sparkles" stuck until the next mutation.

---

## 5. Cost, measured by reading rather than assumed

- **The winner scan is cached** (`sparkle_dirty_` + `contentRevision()`, same shape as
  `play_stats_`). Uncached it would fold a path per entry, and `foldPathKey` allocates - 5000
  allocations per frame at 3.3fps on a long playlist, on the draw path.
- **The per-character draw is bounded by PANE WIDTH, not playlist length.** At most 3 rows sparkle
  (the tie cap) and only those scrolled into view; on a 200-column terminal that is ~300 cells per
  frame, ~1,000 `setcchar`/`wadd_wch` pairs per second. **A 5000-track playlist costs what a 20-track
  one does.** The true delta is smaller still, since `waddnwstr` already does per-character work
  internally.
- Every other row keeps its single `mvwaddnwstr`.

---

## 6. 1.6.2 CEREMONY - WHAT IS AND IS NOT DONE

**Done:** `include/Version.h`, `CMakeLists.txt` and **`docs/index.html`** all say 1.6.2 (index.html
was reconciled in the same commit this time, not deferred); `CHANGELOG.md` has a `[1.6.2] - 2026-08-11`
entry and its link ref; `docs/CLAUDE.md` says UNRELEASED with what it contains.

**NOT done - this is the whole outstanding list:**

1. Merge `experimental/win-pdcurses` -> `dev`.
2. Merge `dev` -> `main`.
3. Tag `1.6.2`.
4. Flip the `docs/CLAUDE.md` paragraph from UNRELEASED to released, with the tag and merge SHA.

**The 08-09 handoff recorded that `docs/CLAUDE.md` claimed "1.6.1 is UNRELEASED" for a full day after
it shipped. Item 4 is that mistake's counterweight - do it in the same sitting as items 1-3.**

---

## 7. Gates

**Windows** - banner read and quoted every configure:
`curses: PDCursesMod wingui (vendored, static) - Option C`,
`STATIC PROBE: preferring .a archives over .dll.a import stubs`, `CMAKE_BUILD_TYPE:STRING=` (empty).
Build `EXIT=0`, **ctest 56/56**. `ldd` shows exactly two UCRT64 DLLs: `libebur128.dll`,
`libfdk-aac-2.dll`. `remoct.exe --version` reports **`RE-MOCT v1.6.2-win`**.

**Linux** - WSL Debian, `ncursesw` in the banner (correct; `REMOCT_PDCURSES` is `WIN32`-gated).
Build `EXIT=0`, **ctest 57/57**.

**Warning diff vs `docs/warn-sweep-plan.md`: NO new warnings on either toolchain.** Same set, same
counts, line drift only. This session's drift, which is the **fourth** recorded:

| was (08-09 §10) | now |
|---|---|
| `UIManager.cpp:11691` (`wrows`) | **`11889`** |
| `UIManager.cpp:12855, 12952` (misleading-indent) | **`13053`, `13150`** |
| `UIManager.cpp:595, 598` (missing-field-init) | **unmoved** - every edit is below them |

`warn-sweep-plan.md` is untracked and not greenlit, so it was **not** edited. The drift is recorded
here instead.

**Live test:** Dos confirmed on hardware. Classic scanner reads as one colour; `Ctrl+T` leaves Awesome
unchanged; Awesome glitters per character and slower; Classic breathes as a row.

---

## 8. Noticed and deliberately NOT fixed

**The EQ palette borrow.** `drawEq` (`UIManager.cpp` ~`:5037-5046`) overloads `CP_VIZ_HIGH/LOW/MID/
PEAK` to mean selected / boost / cut / disabled - the same borrow as the scanner, found by the same
grep. Static rather than animated, so it was outside the brief that found it, and Dos ruled it a
non-goal. **Logged durably in `docs/lessons.md`, not only in a report.**

Its live hazard is the comment, not the code: `:5036` hard-codes the *default Classic* colours into
prose - *"sel=cyan-on-cyan, boost=green-on-green, disabled=white-on-white"* - which stops being true
the moment anyone edits `theme.conf` or presses `Ctrl+T`. **A future session reads that and believes
it.** If the EQ is ever touched, fix the comment before the code.

---

## 9. Still open, unchanged

- **The three C2 decisions** - `docs/session-handoff-2026-08-09-1933.md` §4. Untouched this session.
- **The unexplained pairing** - `The Sanctuary of Zi'Tah` + the FFXI artist. Ruled: leave open, do not
  chase, real only if it recurs.
- **The two GCC 16 dead stores** + `RipProgress::using_c2`, the third of the family that GCC cannot
  see because it is a struct field. Not greenlit.
- `selection.disc_total` means TRACKS while `disc.disc_total` means MEDIA - flagged, deliberately not
  renamed (published in 1.5.0).
- **Cover art candidates** - may want re-scoping rather than resuming.

---

## 10. Docs changed

| file | tracked | what |
|---|---|---|
| `docs/CLAUDE.md` | yes | colour-pair range **1-14 -> 1-18 + art pairs from 20** (a tracked doc stating a wrong range is what a future session believes); the role rule; the 1.6.2 UNRELEASED paragraph. **Still 201 lines** |
| `docs/lessons.md` | yes | new **"Colour-pair roles"** section: the scanner diagnosis, "check what returns *before* the gate", the bg-matters trap in both directions, the EQ borrow, and a sub-section on per-character animation (wave vs static vs glitter) |
| `CHANGELOG.md` | yes | `[1.6.2] - 2026-08-11` + link ref |
| `docs/index.html` | yes | 1.6.1 -> 1.6.2, all six stamps and the headline |
| `docs/warn-sweep-plan.md` | **no** | **untouched.** Not greenlit; the line drift is in §7 above |

---

## 11. Process notes from this stretch

1. **A widget that consults the theme can still draw the wrong vocabulary.** "Does it read the
   palette?" is the wrong test; "does it take the pair for its row's ROLE?" is the right one. The
   scanner passed the first and failed the second, which is why it survived review.
2. **When a brief's premise is wrong, say which part and then answer the real question.** The
   hypothesis here was "fixed pair"; the truth was "borrowed role". Reporting that first is what made
   the fix obvious rather than a taste call - and it is the second session running that this mattered.
3. **Check what RETURNS before a gate, not just whether the gate exists.** `grep awesome_mode` showed
   `drawProgress` as mode-aware. It was, for the paths the bug was not on.
4. **For any animation spread across cells, the OFFSET between cells is the effect.** Ramp = wave,
   per-frame hash = static, stable scramble = glitter. Same code, three different features.
5. **Cost claims get read out of the source, not assumed.** The brief assumed the marquee tick
   animates the playlist pane; it explicitly does not. The thing that made the feature free was a
   different unconditional 300ms redraw - and finding that is what made "zero new frames" a
   measurement instead of a hope.
