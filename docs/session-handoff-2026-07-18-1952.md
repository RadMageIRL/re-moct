# Session handoff - 2026-07-18 ~19:52 CDT - v1.3.0 released + iHeart ad-boundary capture probe

Branch `experimental/win-pdcurses`, tip **`48c8d8b`**. **v1.3.0 is RELEASED and tagged.**

## Headline: v1.3.0 shipped

- Tag **`1.3.0`** (bare, annotated, msg "RE-MOCT 1.3.0") on the main merge commit
  **`9fe4c11`** ("Merge dev into main: RE-MOCT 1.3.0"). `main` at `9fe4c11`, CI green
  both toolchains (Linux Debian + Windows UCRT64). Version.h reads 1.3.0 at the tag.
- Ceremony mirrored 1.2.0: release-date commit on experimental -> ff dev -> `--no-ff`
  merge dev->main -> annotated tag on the merge commit. Full record in
  [remoct-workflow memory] and the commit graph.
- **Outstanding (Dos's, post-tag):** create the GitHub Release from the `1.3.0` tag
  (CHANGELOG `[1.3.0]` section = notes) and attach the clean-box Windows binary.

## Shipped into 1.3.0 this flight (all CI-green, on experimental/dev/main)

| commit | what |
|---|---|
| `87ff1a2` | repin-refine: iHeart re-pin clip fix (prime-to-music-boundary) + F6 off/on/smart mode + lower-left mode indicator |
| `7793d6e` | repin-refine follow-up: indicator changed to confirm-on-change (~5s flash) + version reverted to 1.3.0 |
| `d8d2e82` | docs-nerdfont: per-platform font docs (Windows `wingui_font` vs Linux terminal font) + `wingui_font` now written to config for discoverability |
| `c4597c5` | release: CHANGELOG date finalization (2026-07-18) + `[1.3.0]` link |
| `e38c4a1` | docs: F6 re-pin mode documented in README (keybindings + streaming features) |
| `9fe4c11` | Merge dev into main: RE-MOCT 1.3.0 (tagged) |

### Open Dos on-air / eyes-on gates (repin-refine)
Section A (clip fix) is on-air-gated - machine gates were necessary-not-sufficient:
- Re-pin through a real morning-drive ad break: confirm playback lands at/near the
  incoming song's start (not 20-30s in) in good-alignment cases, and the safe
  live-edge-minus-2 fallback in the edge cases.
- F6 off / on / smart behave distinctly on-air (smart rides short breaks, re-pins only
  long pods via the 150s SMART_STALL_MS; on = old 35s; off = never).
- The yellow `<feed> - <repin>` indicator flashes ~5s then clears, both themes/builds.
Design of record: `docs/repin-refine-plan.md`.

## New this session (post-1.3.0, experimental only): iHeart ad-boundary capture probe

`48c8d8b` - extended `tools/src/iheart_http_dump.cpp` with a **dual-station
`trackHistory` capture mode** (`--capture`). Instrumentation ONLY - no player/plugin/
audio change; this captures the mechanic, a fix (if any) is a later slice.

**Why:** Z100 (zc1469) intermittently loses now-playing metadata for the song after
an ad break (a gap before it recovers); The Breeze (zc4366) is consistent across the
same boundary. Same player/code/guards, so the variable is the STATION's trackHistory
feed behavior. Station ids confirmed from Dos's URLs: Z100=zc1469, Breeze=zc4366,
Hits 106.1=zc4257.

**What it does:** polls trackHistory JSON for 2+ stations on a fixed ~5s timer, logs the
FULL `data[]` each tick as JSON-lines (unknown fields preserved) plus the player's
would-select state. The four guards are ported VERBATIM from
`plugins/stream/IHeartRadio.cpp::pollNowPlaying`: future-filter (FUTURE_GRACE=60),
newest-aired scan of the whole jumbled `data[]`, monotonic `accepted_max` guard,
staleness (GRACE=30). State per tick: `SONG / EMPTY / HELD / FUTURE-REJECTED / LIVE /
POLL-FAIL`. A `SONG -> gap -> SONG` boundary emits a `TRANSITION` record with the gap
duration (the headline number) + which guard held. Rolling logs (--rollmb, default 40MB),
stderr heartbeat ~each minute, POLL-FAIL-and-continue (never crashes).

Smoke-tested live: both stations 200, full feed captured, guards compute (both read LIVE
off-hours = staleness guard firing), JSONL well-formed, no crash. Not in any CMake build
(rebuild-per-need), so no CI impact. Needs `-Ilib` for vendored `json.hpp`.

### Hypotheses the capture must distinguish (see the brief)
1. post-ad song simply LATE to trackHistory (feed lag) - Z100 lags, Breeze doesn't -> EMPTY.
2. Z100 resurfaces a STALE pre-ad entry -> trips monotonic guard -> HELD.
3. Z100 posts an AD/placeholder entry Breeze doesn't (visible in the dumped data[]).
4. post-ad startTime lands in the FUTURE longer on Z100 -> FUTURE-REJECTED.
5. (audio-side, less likely) - logged enough to see, not the primary target.

### Run it (Phase 1 = off-hours CONTROL, now)
From `tools/src/`:
```
g++ -std=c++20 -I../../lib iheart_http_dump.cpp -o iheart_http_dump.exe -lwininet
./iheart_http_dump.exe --capture 21600 5 --stations=1469,4366 --outdir=cap
```
Z100 vs Breeze(control), 5s, ~6h, rolling JSONL into `cap/`. Add `4257` for Hits.
**Phase 2 (later, Dos live):** SAME tool in a morning/evening drive window (high ad
density) where Z100's gap manifests - the failing transitions to diff against Phase 1.
Analysis (diff drive-time failing vs off-hours clean) happens AFTER Phase 2.

## Working-tree / repo notes
- `docs/session-handoff-2026-07-18-0027.md` is UNTRACKED (a pre-existing personal
  handoff, not committed; did not ship in 1.3.0). This new handoff IS committed.
- `main` also carries direct GitHub web edits (README/index.html/Pages) not in dev, so
  `main` = dev + merge commits + web edits (NOT ff-able from dev; always `--no-ff`).
- Tool docs updated: `tools/README.md` (capture mode + `-Ilib` build line); the tool's
  own header comment carries full usage + the Phase 1/2 plan.

## Next plausible work
- Dos: GitHub Release + Windows binary for 1.3.0; the repin-refine on-air gates.
- iheart-adboundary-capture: run Phase 1 now, Phase 2 at drive time, then analyze the
  differential. A recovery fix (e.g. a grace-hold sized to the measured gap) is a
  SEPARATE later slice, designed against the captured numbers - do NOT pre-build it.
