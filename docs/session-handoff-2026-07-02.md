# Session handoff — 2026-07-02 (rev 2: slice 5 landed — core/platform boundary established)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(The prior handoff `session-handoff-2026-07-01.md` covers Phase 0 + the
negative-offset bug and is still valid history. This file's earlier revisions
covered groups a/b/c at 6/8, then slice 4 closing HTTP at 8/8 — superseded by
this revision, which adds slice 5.)

## Pinned constraints (do NOT re-litigate)
- **The 150-sector offset is a physical property of the disc** — a design aspect of
  AccurateRip, worked out across the HydrogenAudio forum posts at
  https://hydrogenaudio.org/index.php/topic,97603.0.html. Correct by design. Never
  "fix" the 150. (Also in `lessons.md`, the `ar_crc.h` comment, the negative-offset commit.)
- **HTTP request bodies go over the wire as raw UTF-8 bytes — never widen them.**
- **Parity for an HTTP migration lives at the call sites, not the seam** — and diff the
  read *loops*, not just the flags (the slice-4 `read_error` lesson).
- **`core::http()` / `core::setHttp()` are TRANSITIONAL globals** — the endgame is
  dependency injection (host hands plugins an `IHttp`, per `architecture.md`).
- **The live StreamSource audio read loop stays OUT of the seam, permanently.**
  `producerWorker`/`producerWorkerAAC` → `rawRead`'s `InternetReadFile(hConn_)` → SPSC
  ring is raw WinINet by design. Audit invariant: `InternetReadFile` appears in
  StreamSource ONLY in `rawRead`; `InternetOpenUrl` only in `connect()`'s ICY branch.

## What this session accomplished — slice 5: core/platform directory boundary
Commit `1977539` (code) + this docs commit. Pure relocation of the finished HTTP
seam — deliberately reorg-first on known-good code so the boundary is established
once and the remaining seams build into it instead of relocating later:

- `git mv include/IHttp.h include/core/IHttp.h` and
  `git mv src/HttpWinInet.cpp src/platform/win/HttpWinInet.cpp` — both true renames
  (95%/98% similarity), per-file history/blame preserved.
- **Include approach decided: statements changed** — `#include "core/IHttp.h"` at all
  15 referencing sites (IHeartRadio.h, StreamSource.h; CDRipper, CoverArt,
  IHeartRadio, LastFm, ListenBrainz, MBLookup, RadioBrowser .cpp; the impl; 5 test
  .cpps). Chosen over appending `include/core` to search paths: the layer is visible
  at every include site (the `core/`-purity audit is a grep over include lines),
  future core headers (IIpc, INotify, ICdIo) follow the pattern with zero extra
  CMake, and `include/` already on every target's path means no include-dir edits.
- CMake: `HttpWinInet.cpp` path updated in the main target (`if(WIN32)` block) + the
  4 Windows-only test targets (scrobble/group_c/http_cancel/iheart_request).
- Change surface: 18 files, 29+/29− — provably move-plus-paths only (the only
  non-include edits are the path comments inside the two moved files + CMake).
- **`src/platform/linux/` NOT created** — it's the home for the libcurl / Unix
  socket / libnotify / SG_IO impls when Phase 3 arrives. Only `win/` exists.
- Scope discipline held: ONLY the finished HTTP seam moved. All other files stay
  flat until their seam is migrated — each future seam lands its interface in
  `include/core/` and its Windows impl in `src/platform/win/` at migration time.

## Verification (slice 5 — "nothing changed but paths")
- Clean reconfigure + rebuild after `rm -rf build` (stale cache would lie): 50/50.
- **ctest 7/7 green** — same suite, same results (http_cancel_test ~2.6 s as before).
- `remoct.exe` launches and plays a local file (sanity floor; no rip/stream gate
  needed for a provably move-only diff).
- `git status` showed both files as `R` renames, never delete+add.

## Rest of Phase 1 — next fork (pick a seam; the structure is ready)
- **IPC:** Discord named-pipe → abstract interface (Linux = Unix socket).
- **Notifications:** Toast/PowerShell → abstract (Linux = libnotify/notify-send).
- **CD access:** Windows IOCTL → SG_IO on Linux.
- Each builds into the slice-5 structure: interface → `include/core/`, Windows
  impl → `src/platform/win/`, consumers include `"core/<Interface>.h"`.
- Parked (see `roadmap.md`): wiring `stop_` into IHeart polls (behavior improvement,
  separate decision); stalled-connect prompt interrupt (hardening); concurrency debt.

## Current state
- **Branch:** `restructure`; slice 5 = code `1977539` + this docs commit. Slice 4
  (`4c72b09` + docs) beneath it. Local until pushed.
- **Tests:** 7/7 green via `ctest` (iheart_sm, ar_crc, http_seam, scrobble_request,
  group_c_request, http_cancel, iheart_request).
- **Build:** clean, `remoct.exe` at `build\bin\remoct.exe`.
- **Layout now:** `include/core/IHttp.h` + `src/platform/win/HttpWinInet.cpp`;
  everything else still flat in `include/` + `src/` (by design — moves happen
  per-seam at migration time).
- **This `E:\code\remoct` clone has the full MSYS2 UCRT toolchain + deps** — full build +
  ctest run locally; only interactive/real-world checks (rip / scrobble / stream) need 7of9.
- The `sniffiheartradio/` probe keeps its own standalone WinINet copies of IHeartRadio —
  intentionally divergent, unaffected.

## Build / test commands (MSYS2 UCRT64)
```
cmake -S . -B build -G Ninja && cmake --build build          # build (also builds tests)
ctest --test-dir build --output-on-failure                    # run tests (build FIRST)
```
- Trust `ctest` output, not the main build's link-line count, as the test signal.

## Operating discipline for next session
- **Read `CLAUDE.md` + `roadmap.md` + `lessons.md` + `architecture.md` + THIS handoff
  FIRST**, before proposing anything. Verify against the actual code (`ls`/grep before
  asserting).
- **Design-first, no code until the boundary is confirmed.** Survey (verified,
  file:line), propose the seam/parity, flag the risks, then stop for sign-off.
- **Verify → build → ctest → real-world → commit.** No commit until the real-world gate
  passes. Commit code and docs separately. No `Co-Authored-By` trailer.

## Standing rule (going forward)
At the END of every working session, **update `roadmap.md` + `lessons.md` and refresh the
handoff doc as the last step before stopping** — so the durable docs are always the source
of truth and the next session never depends on session memory.
