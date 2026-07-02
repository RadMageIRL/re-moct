# Session handoff — 2026-07-02 (rev 3: slices 5 + 6 landed — boundary established, IPC seam in)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(The prior handoff `session-handoff-2026-07-01.md` covers Phase 0 + the
negative-offset bug and is still valid history. This file's earlier revisions
covered HTTP 6/8, then slice 4 closing HTTP at 8/8, then slice 5 — superseded by
this revision, which adds slice 6.)

## Pinned constraints (do NOT re-litigate)
- **The 150-sector offset is a physical property of the disc** — a design aspect of
  AccurateRip, worked out across the HydrogenAudio forum posts at
  https://hydrogenaudio.org/index.php/topic,97603.0.html. Correct by design. Never
  "fix" the 150. (Also in `lessons.md`, the `ar_crc.h` comment, the negative-offset commit.)
- **HTTP request bodies go over the wire as raw UTF-8 bytes — never widen them.**
- **Parity for a transport migration lives at the call sites, not the seam** — diff the
  read *loops*, not just the flags, and preserve asymmetric timeout shapes (the slice-4
  `read_error` and slice-6 bounded-header/unbounded-body lessons).
- **`core::http()` / `core::setHttp()` are TRANSITIONAL globals** — the endgame is
  dependency injection. Slice 6 set the DI precedent: single-consumer seams take their
  interface by CONSTRUCTOR INJECTION (DiscordRP holds an `IIpc*`; `core::ipc()` is only
  the link-time bridge, no `setIpc()` exists).
- **The live StreamSource audio read loop stays OUT of any seam, permanently.**
  `producerWorker`/`producerWorkerAAC` → `rawRead`'s `InternetReadFile(hConn_)` → SPSC
  ring is raw WinINet by design. Audit invariant: `InternetReadFile` appears in
  StreamSource ONLY in `rawRead`; `InternetOpenUrl` only in `connect()`'s ICY branch.

## What this session accomplished
**Slice 5** (code `1977539` + docs `098354e`, PUSHED): the core/platform boundary —
`include/core/IHttp.h` + `src/platform/win/HttpWinInet.cpp` via `git mv` (true renames),
`#include "core/IHttp.h"` at all 15 sites (statements-changed decided over search-path),
5 CMake path updates. Move-plus-paths-only diff; clean rebuild 50/50; ctest 7/7;
launch+play confirmed. See roadmap Done + rev 2 of this file for full detail.

**Slice 6** (code `89285d8`, docs = this commit, local): the IPC seam — the FIRST seam
built INTO the boundary, and the proof it's clean:
- `include/core/IIpc.h`: `IIpcChannel` (a local bidirectional byte channel) + `IIpc`
  (channel factory, logical endpoint name → platform path). Three primitives shaped by
  the one real consumer: `send` (whole buffer, ONE OS write — Discord's framing breaks
  on split writes), `waitReadable(min_bytes, timeout_ms)` (bounded peek-poll; the win
  impl keeps the baseline's 10 ms granularity), `recvSome` (one blocking OS read).
  The bounded-header/UNbounded-body asymmetry is deliberate parity — a tidier
  `recvExact(timeout)` was rejected (would invent semantics the baseline doesn't have).
- `src/platform/win/IpcWinPipe.cpp`: `platform::win::WinPipeIpc` — faithful extraction
  of DiscordRP's pipe code (CreateFileW/WriteFile/PeekNamedPipe+Sleep/ReadFile/
  CloseHandle). Teardown = close only, NO CLOSE frame (baseline: faithful, not polite).
- Protocol stayed in DiscordRP: framing, discord-ipc-0..9 probe (Discord-protocol
  knowledge), handshake, 1 MB squatter cap, CLOSE handling, lazy reconnect, payload/
  nonce/jsonEscape. Same protocol/transport split as IHttp.
- **Ownership: constructor injection** — `DiscordRP(app_id, core::IIpc* = nullptr)`,
  default `core::ipc()`. No `setIpc()` global (one consumer didn't justify the
  http()/setHttp() pattern; the ctor param IS the DI endgame shape).
- DiscordRP.cpp pipe-API-free (`windows.h` only for `GetCurrentProcessId` — the
  IHeartRadio `GetTempPathA` shape); DiscordRP.h drops `windows.h` entirely, so
  UIManager.h stops inheriting it from this path. UIManager's
  `DiscordRP discord_{app_id}` unchanged.

## Verification (slice 6, two-layer, all green)
- **ctest 8/8**: prior seven + `discord_ipc_test` (FakeIpc via ctor injection driving
  the REAL DiscordRP: handshake bytes exact, probe order 0..9 / first-accept /
  all-refuse no-op, SET_ACTIVITY framing + pid + timestamps/assets omission,
  jsonEscape (quote/backslash/newline/tab/control→\u00xx), activity-null clear,
  CLOSE→reject-without-activity-frame, reconnect-after-death re-probes+re-handshakes).
  Fixture lesson (recorded in `lessons.md`): the fake's captured frames must live in
  state owned by the fake FACTORY (shared_ptr), not on the channel — DiscordRP
  destroying the channel on CLOSE is the behavior under test, and first draft dangled.
- **Live Discord gate (all four passed on this machine):** RP shows title/artist +
  album art; track change updates; Discord quit+restart → next update reconnects
  (lazy re-probe + re-handshake); toggle off clears the activity.

## Rest of Phase 1 — remaining seams (build into the structure)
- **Notifications:** Toast/PowerShell → `core::INotify`-ish (Linux = libnotify/notify-send).
- **CD access:** Windows IOCTL → SG_IO on Linux (StreamSource/CDSource/CDRipper survey first).
- Each: interface → `include/core/`, Windows impl → `src/platform/win/`, consumer
  includes `"core/<Interface>.h"`; pick accessor-vs-injection by consumer count.
- Parked (see `roadmap.md`): wiring `stop_` into IHeart polls; stalled-connect prompt
  interrupt; concurrency debt; Track Info album-tag decode artifact.

## Current state
- **Branch:** `restructure`. Slice 6 = code `89285d8` + docs (this commit) — LOCAL,
  push when ready. Slice 5 pair (`1977539`/`098354e`) already pushed; origin in sync
  up to there.
- **Tests:** 8/8 green via `ctest` (iheart_sm, ar_crc, http_seam, scrobble_request,
  group_c_request, http_cancel, iheart_request, discord_ipc).
- **Build:** clean, `remoct.exe` at `build\bin\remoct.exe`.
- **Layout:** `include/core/{IHttp,IIpc}.h` + `src/platform/win/{HttpWinInet,IpcWinPipe}.cpp`;
  everything else flat until its seam migrates. `src/platform/linux/` still absent (Phase 3).
- **This `E:\code\remoct` clone has the full MSYS2 UCRT toolchain + deps** — build + ctest
  run locally; interactive/real-world checks run here too when they don't need hardware
  (the slice-6 Discord gate ran on this machine); a rip needs 7of9's drive.
- Gotcha: the harness shell's cwd can silently reset to `E:\code` (outer junk repo) —
  `cd /e/code/remoct` explicitly in git one-liners.

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
