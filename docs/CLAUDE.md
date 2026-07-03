# RE-MOCT — Claude Code briefing

This file loads every session. It's the index, not the encyclopedia. Deep detail
lives in `docs/` (see the pointer block below) — read those on demand, not eagerly.

## What this is
RE-MOCT (**M**usic **O**n **C**onsole **T**erminal) is a C++20 Windows terminal
audio player, CD ripper, and internet-radio client. Sole dev/owner: Dos
(RadMageIRL). Repo: `github.com/RadMageIRL/re-moct`. Dev machine: **7of9**.

**Identity / thesis:** Classic mode is a faithful MOC homage. Hit **Ctrl+T**
(Awesome mode, `config_.awesome_mode`) and it becomes *RE-MOCT* — the remix
(comet progress bar, sub-cell visualizer, breathing animations). The mode toggle
is the whole point; keep Classic minimal and faithful, put flair in Awesome.

## Build & run
- Toolchain: **MSYS2 UCRT64**, GCC 15.2, CMake + Ninja, ncursesw.
- Binary: `build\bin\remoct.exe`. Build from the repo root.
- Audio/encode: miniaudio (`ma_device_*`), FDK-AAC, libFLAC, LAME, libebur128, TagLib.
- Net: all HTTP via the `core::IHttp` seam (`include/core/IHttp.h`; WinINet impl
  `src/platform/win/HttpWinInet.cpp`) — except StreamSource's live read loop (raw
  WinINet by design, permanently).
- Services: MusicBrainz, Discogs, AccurateRip, CTDB, Cover Art Archive, iTunes, Deezer,
  iHeart (HLS via `revma.ihrhls.com`), radio-browser.info, ICY/SHOUTcast,
  Last.fm, ListenBrainz, Discord (named-pipe IPC, asset key `remoct_logo`).

## How we work (collaboration discipline)
- Claude reads the tree, edits **surgically and additively**, and hands back
  **complete compilable drop-in files OR tight scoped diffs** — never patch
  documents or find/replace instructions.
- **Claude cannot compile.** Loop: Claude edits → Dos builds on 7of9 → reports →
  re-syncs. Verify with brace-balance + scoped-diff audits and standalone probes.
- Always build on the files Dos actually uploaded this turn (stale baselines
  silently drop prior work). Run grep/brace audits before handoff.
- **Confirm before touching concurrency-sensitive paths** (crossfade, streaming
  machinery, ring buffer). Lay out tradeoffs explicitly; Dos engages at peer level
  and wants pushback when warranted. No speculation — confirm mechanism before code.
- **Probe-first:** validate new parsing/protocol logic with a standalone test tool
  before integrating (e.g. `iheart_http_dump.cpp`, `radio_probe.cpp`).

## Always-on technical rules
- **Wide API for all Unicode.** This ncursesw build reports `COLORS=8` everywhere
  (256-color blocked). Narrow draw funcs don't decode UTF-8 — every glyph must use
  the wide API (`setcchar`/`mvwadd_wch`/`mvwaddnwstr`), `NCURSES_WIDECHAR` defined
  before the ncurses include. Measure/cut text by display **columns**, not bytes
  (StringUtils helpers).
- **AccurateRip's 150-sector physical preamble is correct by design** (confirmed on
  HydrogenAudio). Do not re-litigate it.
- **Ring-buffer state transitions are subtle.** `prebuffered_` + `ringClear()`
  interact in both producer re-pin paths; changes need explicit justification. The
  data callback guards on `seeking_`.
- **Free-key scan** before assigning a new key: check `case` *and* `if (ch == ...)`
  binding forms.
- Color pairs are slots 1–14 (`CP_*` enum in UIManager.h); viz pairs are fg==bg
  solid fills, `CP_VIZ_TIP` (14) is peak-fg-on-default-bg for sub-cell glyphs.

## Current state (working & verified)
Awesome-mode comet progress bar (proportional gradient tail, breathing 1↔2↔3-cell
head, crossfade head-completion via `isCrossfading()`); Classic-mode original
`[#---]` bar. Sub-cell visualizer (lower-block glyphs, `CP_VIZ_TIP`). MP3 seek
smoothing: coalescing (`requestSeek`/`flushPendingSeek`, ~100ms cooldown,
track-stamped to prevent cross-track leak — no post-change lockout, by choice) +
prime-after-seek (decode-discard ~0.18s before target to warm the bit reservoir).
Audiobook suite (`.m4b`, chapters, `[Books]` nav). Discord Rich Presence stage 2
(async album art). `theme.conf` theming. CoverArt module. iHeart metadata state
machine + ring-buffer re-pin fix. Device-switching fix. Column-aware UTF-8 pipeline.

## Next substantive step
**Phases 0/1/2 COMPLETE** (seams 8/8 + boundary; `core::ISource` proven against
all four sources; slice C declined — see roadmap Decisions log).
**Phase 3 (Linux port) IN PROGRESS** — readiness survey + slicing approved
(`docs/phase3-readiness.md`): strictly a PARITY port. **Slice 0 DONE**
(`27735f5`): CI matrix live (debian:trixie container + windows msys2, both
required green); seam stubs in `src/platform/linux/` (`platform::lnx`); WSL2
Trixie inner loop on 7of9. **CD gate venue PROVEN: usbipd→WSL2** (GHD3N busid
4-1 stays bound; real SG_IO READ CD verified on Relish). **Slice 1 DONE:
the portable core compiles, links, and PLAYS on Linux** — whole-file `_WIN32`
gates off 21 files; `include/PortUtil.h` (each helper's Windows expansion =
the baseline call verbatim); StreamSource's sacred ICY loop moved inside
`#ifdef _WIN32` byte-verbatim (Linux connect() refuses Continuous until the
slice-3 twin; HLS fully portable). **Slice 2 DONE: libcurl `core::IHttp`
(`src/platform/linux/HttpCurl.cpp` — CURLSH share-handle sessions, XFERINFO
cancel, LOW_SPEED stall-guard timeout) + vendored MD5 both platforms
(`lib/md5.{h,c}` byte-verbatim Openwall; LastFm one signing path); five HTTP
tests portable; Windows 14/14, Linux 10/10; live gates = MB probe (real
Relish TOC resolved), RadioBrowser in the TUI (KWIN 97.7), digital iHeart
HLS PLAYED on Linux (audible, RMS-proven, nowPlaying live).**
**Next: slice 3 (ICY raw-loop Linux twin — design-first, sacred territory;
lean shape: curl CONNECT_ONLY + curl_easy_recv keeps the pull-read shape).**
The live read loop stays raw WinINet permanently on Windows. See
`docs/roadmap.md` for the six-slice plan + per-slice gates.

## Deep knowledge — read the matching file when a task touches it
- Roadmap, phases, parked items, decisions → `docs/roadmap.md`
- Plugin/Source interface, platform abstraction, Linux port, GitHub strategy → `docs/architecture.md`
- Hard-won lessons (AccurateRip, wide-API, ring buffer, MP3 seek, cover art) → `docs/lessons.md`
- Streaming internals (iHeart desync, metadata machine, ICY, rabbit-hole capture) → `docs/streaming.md`

Keep this file under ~200 lines (Claude Code truncates the tail silently). Put new
detail in `docs/`, not here; update the pointer block if you add a doc.
