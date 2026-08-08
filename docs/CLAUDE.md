# RE-MOCT - Claude Code briefing

This file loads every session. It's the index, not the encyclopedia. Deep detail
lives in `docs/` (see the pointer block below) - read those on demand, not eagerly.

## What this is
RE-MOCT (**M**usic **O**n **C**onsole **T**erminal) is a C++20 Windows terminal
audio player, CD ripper, and internet-radio client. Sole dev/owner: Dos
(RadMageIRL). Repo: `github.com/RadMageIRL/re-moct`. Dev machine: **7of9**.

**Identity / thesis:** Classic mode is a faithful MOC homage. Hit **Ctrl+T**
(Awesome mode, `config_.awesome_mode`) and it becomes *RE-MOCT* - the remix
(comet progress bar, sub-cell visualizer, breathing animations). The mode toggle
is the whole point; keep Classic minimal and faithful, put flair in Awesome.

## Build & run
- Toolchain: **MSYS2 UCRT64**, **GCC 16.1** (upgraded from 15.2 on 2026-08-07;
  TagLib stayed 2.2.1 - MSYS2 has not packaged upstream 2.3.1), CMake + Ninja.
- **Windows configure - THESE FLAGS, OR IT IS A DIFFERENT PRODUCT:**
  `cmake -S . -B build -G Ninja -DREMOCT_PDCURSES=ON -DREMOCT_STATIC_PROBE=ON`,
  and `CMAKE_BUILD_TYPE` stays **EMPTY** (no `-DNDEBUG`; asserts live, and that is
  what made the CJK resize crash findable). Binary: `build\bin\remoct.exe`.
- **READ THE CONFIGURE BANNER.** It must say `curses: PDCursesMod wingui
  (vendored, static) - Option C` and `STATIC PROBE: preferring .a archives`, and
  the exe must need exactly two UCRT64 DLLs (`libebur128`, `libfdk-aac-2`). A bare
  `cmake -S . -B build` silently configures an **ncursesw** build that links 19
  DLLs and does not run. It compiles and tests green. **A green build of the wrong
  product is indistinguishable from a green build unless the banner is read.**
- Linux: `cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release` under WSL
  Debian. `REMOCT_PDCURSES` is `WIN32`-gated, so `ncursesw` in the banner is
  correct there. No `/dev/sr*`, so no CD work on Linux.
- `ctest` needs `export PATH="/c/msys64/ucrt64/bin:$PATH"` **in the same shell**,
  or ~22 tests fail with `0xc0000139`. Gates: Windows **56/56**, Linux **57/57**.
- Audio/encode: miniaudio (`ma_device_*`), FDK-AAC, libFLAC, LAME, libebur128, TagLib.
- Net: all HTTP via the `core::IHttp` seam (`include/core/IHttp.h`; WinINet impl
  `src/platform/win/HttpWinInet.cpp`) - except StreamSource's live read loop (raw
  WinINet by design, permanently).
- Services: MusicBrainz, Discogs, AccurateRip, CTDB, Cover Art Archive, iTunes, Deezer,
  iHeart (HLS via `revma.ihrhls.com`), radio-browser.info, ICY/SHOUTcast,
  Last.fm, ListenBrainz, Discord (named-pipe IPC, asset key `remoct_logo`).

## How we work (collaboration discipline)
- Claude reads the tree, edits **surgically and additively**, and hands back
  **complete compilable drop-in files OR tight scoped diffs** - never patch
  documents or find/replace instructions.
- **Claude builds and gates directly** on 7of9 (Windows) and WSL Debian (Linux) -
  both toolchains, `EXIT=0`, ctest both, warning diff against
  `docs/warn-sweep-plan.md`, every slice. This replaced the old
  edit→Dos-builds→report loop; do not fall back to it. Dos still does the live
  hardware test, which is the only thing that can exercise a real disc.
- Brace-balance + scoped-diff audits before handing anything back, and **check
  every caller** of a signature you change rather than the ones you remember.
- **Confirm before touching concurrency-sensitive paths** (crossfade, streaming
  machinery, ring buffer). Lay out tradeoffs explicitly; Dos engages at peer level
  and wants pushback when warranted. No speculation - confirm mechanism before code.
- **Probe-first:** validate new parsing/protocol logic with a standalone test tool
  before integrating (e.g. `iheart_http_dump.cpp`, `radio_probe.cpp`).

## Always-on technical rules
- **Wide API for all Unicode.** This ncursesw build reports `COLORS=8` everywhere
  (256-color blocked). Narrow draw funcs don't decode UTF-8 - every glyph must use
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
track-stamped to prevent cross-track leak - no post-change lockout, by choice) +
prime-after-seek (decode-discard ~0.18s before target to warm the bit reservoir).
Audiobook suite (`.m4b`, chapters, `[Books]` nav). Discord Rich Presence stage 2
(async album art). `theme.conf` theming. CoverArt module. iHeart metadata state
machine + ring-buffer re-pin fix. Device-switching fix. Column-aware UTF-8 pipeline.
The disc-number campaign (below). `Ctrl+F` metadata search is **common** now.

## 1.6.1: which disc this is - the vocabulary to keep straight
`DiscPick` (`include/MBLookup.h`) is the one answer: `{disc, total, matches, user}`
with `ambiguous() == !user && total > 1 && matches != 1`. `pickDiscForTrackCount`
is a thin wrapper over `pickDisc` and its contract is unchanged. **Ambiguous means
disc 1 was ASSUMED**, covering both no-match and a tie; a user's choice
(`withUserDisc`) clears it, because a person's answer is determined whatever the
counts say. Everything that reports - `discLogLine`, `discSourceLabel`,
`discAmbiguityNote`, `discColumn`, `formatCandidateRow`, `wrapToWidth` - is a pure
function over it in that same header, so the log, the sidecar, the modal and the
cmdline cannot drift. `UIManager::resolvedPick()` is the only thing consumers call.

**`toc_track_count` is the TRACK count. `total_discs` / `DiscPick::total` is the
MEDIA count.** They were both `disc_total` until 2026-08-08, in one file, meaning
opposite things. In `disc.json`, `disc.disc_total` is media and the older
`selection.disc_total` is tracks - published in 1.5.0, deliberately NOT renamed.

`disc.json` is **schema 4**: the `disc` block carries `disc_number`, `disc_total`
and `disc_source` ∈ `unique_track_count` | `ambiguous_fallback` | `user`.
Every taggable output carries the disc number, `1/1` included (`include/DiscTag.h`).

## LOCKED - the conversation does not happen
`docs/LOCKED-CODE.md` is binding. **Every proposal touching the CD path opens with
this line, verbatim, before anything else:**

> This proposal touches / does not touch AR_PREGAP, ar_crc.*, or the read addressing.

**If the answer is "touches": stop there and say so. Do not propose it.** No brief,
no probe attached, no scoped exception. The rule forbids the discussion, not just
the change. Locked: `ar_crc.*`, `AR_PREGAP=150`, the read addressing, the disc-ID
math (`computeCDDB`/`fetchARData`/`tocOffsets`/CTDB), the audio thread,
`CursesSeam.h`, dynamic `libfdk-aac-2.dll`, `abi_version=1`, `Ctrl+T`.

**CD addressing (`docs/CD-ADDRESSING-LOCKED.md`):** TOC reports ATIME, reads address
in LBA, `LBA = frame - 150`. `start_frame` is NEVER a read address; `lba()` is the
only thing that goes to a read. `kMsfLeadIn` and `AR_PREGAP` both equal 150 and are
never substituted for each other. Fixed in `f759781`; never re-litigated.

## Where the project is
**Phases 0-4 COMPLETE** (2026-07-04): every platform call behind a seam on both
platforms, Linux port done, streaming source is a real loadable plugin
(`remoct_stream.{so,dll}`) proven byte-identical to compiled-in. Detail in
`docs/roadmap.md` / `docs/architecture.md` / the phase-4 handoff. **"Fix iHeart and
ship without rebuilding the host" is literally true.**

**Released:** 1.5.0 and 1.6.0 (2026-07-27), both merged to `main` and tagged.
1.5.0 = per-track rip selection + the 17-slice `[Library]` section + the CD
read-addressing fix. 1.6.0 = HTOA (hidden track before track 1).

**1.6.1 is UNRELEASED on `experimental/win-pdcurses`.** `Version.h` and
`CMakeLists.txt` say 1.6.1; **`docs/index.html` deliberately still says 1.6.0** and
reconciles at ceremony. Contents: the non-ASCII display fold, a CJK crash fix, a
window-move repaint fix, and the disc-number campaign (disc number in tags, the
silent tie made loud, the disc surfaced before the rip, and the release/disc
picker with `F5` re-open). See the newest `docs/session-handoff-*.md`.

## 1.6.1: the display fold - read before touching display text
`foldForDisplay` (`include/StringUtils.h`, was `sanitizeForDisplay`). Per codepoint:
**reject** malformed UTF-8 as `?`, **normalize** the typography table, else **pass
through verbatim**. **Byte length is never a criterion** - that was the bug: every
3- and 4-byte sequence became one `?`, so CJK drew as `????` while 2-byte accented
Latin came through.

**RAW IN, FOLD AT THE DRAW.** Identity and outbound paths carry raw text - the
library index, `TrackInfo`, `now_playing_`, the CD `MBRelease`. Folding on the way
in scrobbled `???? - ????` to Last.fm and wrote folded text back into tags from the
editor. The fold belongs at draw sites only.

**Windows font:** RE-MOCT picks its own (`wingui_font`); bundled JetBrains Mono has
no CJK. Boxes are a missing-glyph result, not a fold failure.

## 1.6.1: vendored PDCursesMod carries a patch
`lib/pdcursesmod/pdcurses/refresh.c` - a fullwidth glyph's two cells split across a
`MAX_PACKET_LEN` (89) chunk boundary aborted the process. **That patch is the ONLY
thing preventing the abort; RE-MOCT-side scheduling never was. RE-APPLY IT AFTER ANY
RE-PIN.** Filed upstream as Bill-Gray/PDCursesMod **#386**; drop it only once that
lands. `VENDOR.md` entry 4 is the record.

**wingui gotchas that cost a night:** `PDC_doupdate()` is a `PeekMessage`/
`DispatchMessage` pump, so `doupdate()` is not a leaf and anything drawn from a
window handler can be re-entered. `WM_TIMER` is synthesised only when the queue
drains, so a held drag starves it to **zero** - which is why a MOVE repaints from
`WM_MOVING` (app-side subclass, `remoctWndProc`) while a RESIZE stays synchronous on
`WM_SIZE`. Asserts are live (`CMAKE_BUILD_TYPE` empty, no `-DNDEBUG`) and that is
what made the crash findable - do not silence them.

## Earlier flights (detail in the matching handoffs)
**2026-07-16:** .opus/.wv/.ogg playback via custom miniaudio backends; RIP OVERHAUL
(`IEncoder` seam, FLAC/MP3/WAV/Opus/WavPack, modal digits 1-5, one verified read).
**2026-07-17:** stream-record complete (^E, `[REC]`), MP3 tag read+write,
log-semantics, radio-art staleness, THE ABI OPENED ONCE (`254baca`, additive, no
version bump), batch ReplayGain (^O).

## Deep knowledge - read the matching file when a task touches it
- Roadmap, phases, parked items, decisions → `docs/roadmap.md`
- Plugin/Source interface, platform abstraction, Linux port, GitHub strategy → `docs/architecture.md`
- Hard-won lessons (AccurateRip, wide-API, ring buffer, MP3 seek, cover art) → `docs/lessons.md`
- Streaming internals (iHeart desync, metadata machine, ICY, rabbit-hole capture) → `docs/streaming.md`
- What may not be discussed, and the CD addressing rule → `docs/LOCKED-CODE.md`, `docs/CD-ADDRESSING-LOCKED.md`
- Vendored PDCursesMod: local patches, re-pin procedure → `lib/pdcursesmod/VENDOR.md`

Keep this file under ~200 lines (Claude Code truncates the tail silently). Put new
detail in `docs/`, not here; update the pointer block if you add a doc.
