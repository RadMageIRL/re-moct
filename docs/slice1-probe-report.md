# Slice 1 - Probe Report

**Type:** instrumentation + static verification. No production behaviour changed.
**Verified against:** `experimental/win-pdcurses` (our tree; diverged from the brief's
2026-07-07 snapshot - line anchors re-verified locally, divergences noted).
**Status:** static verification COMPLETE. Runtime measurements COLLECTED on
2026-07-08 (Windows wingui + Linux ncursesw + the GHD3N USB drive); see the
"Collected data" section. Remaining gaps: Windows-ncursesw caps, the Linux CD
latency run (needs usbipd attach), and the audible-spin-up confirmation.

---

## Collected data (2026-07-08)

### Probe A - caps (both truecolor -> same clamped budgets)

| build | COLORS | COLOR_PAIRS | has_colors | can_change_color | TERM | c_budget | p_budget |
|---|---|---|---|---|---|---|---|
| Linux ncursesw | 256 | 65536 | 1 | 1 | xterm-256color | 192 | 236 |
| Windows wingui | 16777472 | 1048576 | 1 | 1 | (wingui/n-a) | 192 | 236 |
| Windows ncursesw | (not captured) | | | | | 192* | 236* |

`*` The code clamps `min(COLORS,256)` / `min(COLOR_PAIRS,256)`, so ANY truecolor
build (`COLORS>=256 && can_change_color`) yields c_budget=192 / p_budget=236 - the
render->count is **build-independent**. A non-truecolor build would take the
nearest-of-16 path (fewer distinct colours) => LESS pair pressure, not more. So the
truecolor budget is the worst case and it is identical on wingui and Linux.

### Probe A - render costs (offline harness over 116 real album covers)

The in-TUI A2 counter is live (validated: Color Me Badd = 213 pairs in both the TUI
and the harness). For breadth, an offline harness replicated the EXACT extract ->
cover::render(22x11) -> allocArtColorPairs counting over one cover per album:

| metric | value |
|---|---|
| albums with embedded art | 116 |
| art box (fixed max) | 22x11 = **242 cells** (never larger - see finding below) |
| distinct pairs range | 213 .. 236 |
| covers hitting p_budget (236) | 8 |
| **covers overflowing -> pair_fallback > 0** | **6** (Beck *One Foot in the Grave* fb=3, Garbage *Supervixen* fb=5, Sugar Ray *RPM* fb=3, Yes *Survival* fb=2, + 2 dup copies) |
| colour budget (192) | saturated on nearly all covers (colour_fallback ~266-291) |

**Probe A3 verdict: the pair-budget fallback FIRES on real covers (6 of 116, ~5%).
Slice 8's premise HOLDS** - and the exhausted path returns `P0` (pair 20), NOT the
nearest pair (confirmed in §7), so those overflow cells are visibly miscoloured on
busy covers. The colour budget (192) also saturates on most covers, but that path
degrades gracefully to nearest-existing.

**Finding (divergence from the brief's A2 step 2):** the art box does NOT grow with a
larger terminal - `box_cols = clamp(22,4,cols-26)`, `box_rows = clamp(11,2,rows-4)`
cap at **22x11**. A bigger terminal yields the same 242-cell box; only a *smaller*
terminal shrinks it. So "resize larger -> more cells" is not reproducible; 242 is the
hard max, and 6 covers still overflow the pair budget at that size.

### Probe B - CD idle latency (GHD3N USB DVD, Windows)

| run | platform | tray | median us | max us | call#1 us | audible spin-up? |
|---|---|---|---|---|---|---|
| 1 | Windows (IOCTL) | loaded | 1459 | 6187 | 984 | **YES (heard)** |
| 2 | Windows | empty | ~2000 (3-sample sanity) | ~2043 | ~2002 | n/a (no disc) |
| 3 | Linux (SG_IO) | loaded | 1878 | 4414 | 1891 | likely (same drive; not listened) |
| 4 | Linux | empty | not run (needs tray eject) | | | |

**Probe B verdict: FAIL.** Timing alone would PASS (median 1.46/1.88ms, max
6.19/4.41ms, call#1 not an outlier on either platform) - BUT the GHD3N **audibly
spun up** during the Windows loaded run. This is exactly the case the brief flagged:
`IOCTL_CDROM_CHECK_VERIFY` / SG_IO `TEST UNIT READY` return in ~2ms (before the motor
reaches speed), so the timing does NOT show the spin, yet the disc physically spins.
Per §B3, "the drive audibly spins" is a FAIL condition regardless of latency. Linux
was the same physical drive and almost certainly behaves the same (not listened to
during Run #3).

**=> Slice 3 must RESHAPE (do NOT keep `dev_` open and poll it).** Keeping the handle
open and polling `mediaPresent()` every 2s while stopped would spin the idle drive up
on every poll - audible and wearing. The §B3 FAIL path: keep the TOC, playlist rows,
and MusicBrainz metadata cached; **close the handle on `stop()`; reopen lazily on
Enter / Ctrl+R / Ctrl+Y.** Same user-visible behaviour, different code. (The vestigial
poll from §B1 is a separate Slice-3 fix.)

---

## Probe B1 - CD idle lifecycle (all 6 CONFIRMED)

| # | Assertion | Verdict | Actual (our tree) |
|---|---|---|---|
| 1 | `checkMedia()` short-circuits `if (!dev_) return false;` before touching hardware | CONFIRMED | `CDSource.h:92` (exact); guard `:94`, `dev_->mediaPresent()` `:95` |
| 2 | Every `cd_mode_` clear site also closes the device | CONFIRMED | 5 `store(false)` sites: `AudioManager.cpp:132` play, `374` stop, `1132` openCD-fail, `1145` closeCD, `1181` stream. Each closes `cd_source_` on the line before / same line. (brief's 131/373/1131/1144/1181 = +1 snapshot skew) |
| 3 | `dev_ != nullptr && !cd_mode_` is unreachable | CONFIRMED | `openCD` sets `cd_mode_=true` only after `open()` succeeds (`:1113`->`1114`); both failure paths (`:1113` return, `:1131-1132`) leave `dev_` closed |
| 4 | The poll reaches `checkMedia()` only with `dev_ == nullptr` | CONFIRMED | `UIManager.cpp:1005` (exact); guard `!cd_drive_letter_.empty() && !cdMode() && Stopped` |
| 5 | The poll is a 6s countdown that purges CD rows + clears `cd_drive_letter_` | CONFIRMED | `25*80ms=2s` per check (`:1008`); `cd_fail_count_>=3` (`:1012`) -> `removeIf` + `cd_drive_letter_.clear()` (`:1014-1021`). 3*2s = 6s. Never performs a real media check |
| 6 | Working eject detector is the reader thread, playback-only | CONFIRMED | `CDSource.cpp:224` (exact) `dev_ && dev_->mediaPresent()` after a read failure; sets `media_removed_` `:231`. Eject-while-stopped undetectable |

Twin syscalls (exact): `CdIoWin.cpp:98` -> `IOCTL_CDROM_CHECK_VERIFY`;
`CdIoSgIo.cpp:116` -> `TEST UNIT READY 0x00`. Factory accessor is `core::cdio()`
(`ICdIo.h:107`); the brief's `core::ICdDeviceFactory::open` name is stale.

## Probe C - Classic colour path re-inits the palette

**YES.** Ctrl+T (`case 20`) -> `initColours()` at `UIManager.cpp:4754`, which re-calls
`start_color()` (`:473`) and `use_default_colors()` (`:474`). A theme switch resets
the full palette (art slots 64+ / pairs 20+ included). => The Ctrl+T art-pair
invalidation Slice 2 assumes it can drop **cannot be dropped**.

## §7 disconfirmation - anchors + one real divergence

- `allocArtColorPairs`: **C0=64 / P0=20 confirmed** (`:2927/2929`; function at `:2921`,
  not 2919). `c_budget = min(COLORS,256)-64`, `p_budget = min(COLOR_PAIRS,256)-20`.
  The exhausted-pair fallback **returns `P0`, not the nearest pair** - the comment
  ("reuse the closest fg's pair") is inaccurate. Confirmed for Slice 8.
- `applyAwesomeTheme` at `:518` (exact); truecolour slots from `next=16` (`:556`).
- **DIVERGENCE:** brief says applyAwesomeTheme "uses pairs 1-13" - our tree uses
  **pairs 1-17** (`CP_VIZ_{LOW,MID,HIGH}_B` = 15/16/17, added for the segmented LED
  spectrum). Still `< 20 (P0)` and slots `16..~30 < 64 (C0)`, so **the disjointness
  conclusion HOLDS** - but the count is stale and the free gap before P0 is now 18-19.

## Probe A3 - falsification status

The arithmetic is correct as stated: `p_budget = 236` on a 256-pair build; a 22x11
box is 242 cells. Whether a photographic cover actually exceeds 236 distinct
`(fg,bg)` pairs - and thus whether the fallback ever fires - is exactly what the A2
counters answer at runtime. **Open until the A2 data is collected.**

---

## Instrumentation & tool (built, verified, NOT yet run)

| Item | Location | Notes |
|---|---|---|
| Probe A1 caps log | `UIManager.cpp` `initColours`, both paths | `#ifdef REMOCT_PROBE`; logs `A1 caps ...` |
| Probe A2 render counters | `UIManager.cpp` `allocArtColorPairs` | `#ifdef REMOCT_PROBE`; counters only, allocation logic untouched; logs `A2 alloc ...` |
| `-DREMOCT_PROBE=ON` toggle | `CMakeLists.txt` (default OFF) | shipped build carries zero probe code |
| `cd_idle_probe` | `tools/src/cd_idle_probe.cpp` | standalone; uses real `core::cdio()` |

**Build matrix (all green):** Win OFF 20/20 - Win ON compiles - Linux OFF 21/21 -
Linux ON compiles - `cd_idle_probe` compiles on both platforms.
**Fence checks:** `cd_loaded_` / `ensurePlaylistCursorVisible` / `nowPlayingRow` ->
none. `art_pairs_` -> only pre-existing `info_/radio_` members. UIManager diff is
entirely inside `#ifdef REMOCT_PROBE`; the only non-fenced, non-`tools/` change is the
CMake toggle (default OFF). Nothing in the brief's §1 fence was touched.

---

## Run instructions (next session / data collection)

### Probe A - curses budget + cover render costs
Build the instrumented binary and drive the four renders; read the `Log` file for
`A1 caps ...` and `A2 alloc ...` lines. Do it on **both** Windows backends
(ncursesw + wingui) and Linux.

```bash
# Windows ncursesw (default):
cmake -S . -B build-probe -G Ninja -DCMAKE_BUILD_TYPE=Release -DREMOCT_PROBE=ON
# Windows wingui: add -DREMOCT_PDCURSES=ON
# Linux: same as ncursesw
cmake --build build-probe
```
Then run and trigger, one at a time:
1. `i` on a tagged local track with a photographic cover.
2. Resize the terminal/window LARGER, reopen `i` (bigger art box = more cells).
3. A radio station with a resolvable cover.
4. A station with NO resolvable art (logo floor).

Fill (x2 builds):

| render | grid | cells | truecolor | colours | pairs | c_budget | p_budget | colour_fallback | pair_fallback | ret |
|---|---|---|---|---|---|---|---|---|---|---|
| local cover | | | | | | | | | | |
| local cover (larger) | | | | | | | | | | |
| radio cover | | | | | | | | | | |
| logo floor | | | | | | | | | | |

And the caps line per build:

| build | COLORS | COLOR_PAIRS | has_colors | can_change_color | TERM |
|---|---|---|---|---|---|
| Win ncursesw | | | | | |
| Win wingui | | | | | |
| Linux ncursesw | | | | | |

**Verdict to write:** does `pair_fallback` ever exceed 0? If it is 0 across all four
renders on both builds, the Slice 8 premise (fallback fires, returns P0) collapses -
say so.

### Probe B - CD idle latency
Build the standalone tool (from repo root), then run on 7of9 (Win) and Linux, tray
loaded and empty:

```bash
# Windows (MSYS2 UCRT64):
g++ -std=c++20 -Iinclude tools/src/cd_idle_probe.cpp src/platform/win/CdIoWin.cpp -o cd_idle_probe.exe
# Linux:
g++ -std=c++20 -Iinclude tools/src/cd_idle_probe.cpp src/platform/linux/CdIoSgIo.cpp -o cd_idle_probe

cd_idle_probe.exe D      # Windows;  Linux: ./cd_idle_probe sr0
```
It idles 90s (spin-down), then polls `mediaPresent()` 30x at 2s. **Listen for
spin-up** - it may not show in the timing. Fill:

| run | platform | tray | median us | max us | call#1 us | audible spin-up? |
|---|---|---|---|---|---|---|
| 1 | Windows (7of9) | loaded | | | | |
| 2 | Windows | empty | | | | |
| 3 | Linux (SG_IO) | loaded | | | | |
| 4 | Linux | empty | | | | |

**§B3 verdict:** PASS if median < 10ms, max < 250ms, call#1 not an outlier, no
audible spin-up (Slice 3 keeps `dev_` open across `stop()` and polls). Otherwise
FAIL (Slice 3 closes the handle on `stop()`, reopens lazily). Note empty-tray
behaviour either way.

---

## Slice-close checklist (when this closes)
- Remove the `#ifdef REMOCT_PROBE` blocks in `UIManager.cpp` and the `REMOCT_PROBE`
  option in `CMakeLists.txt`.
- Decide whether `tools/src/cd_idle_probe.cpp` stays (it is a reusable diagnostic,
  like the other `tools/`) or is removed.
- Remove this report or fold its verdicts into the Slice 3 / 8 briefs.

## Headline
The brief's B1 and C mechanisms all hold (with the pairs-1-17 divergence noted), so
the premises for Slices 2/3/8 stand - **except A3 and B2, which only runtime data
can settle**, and both are now instrumented and ready.
