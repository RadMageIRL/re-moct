# Session handoff — 2026-07-02 (Phase 1 HTTP seam COMPLETE: 8/8, slice 4 landed)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(The prior handoff `session-handoff-2026-07-01.md` covers Phase 0 + the
negative-offset bug and is still valid history. This file's earlier revision
covered groups a/b/c at 6/8 and the fork; the fork is now resolved — superseded
by this revision.)

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

## What this session accomplished — slice 4: HTTP 8/8, consolidation CLOSED
Commit `4c72b09` (code), this commit (docs). The two audio-thread sites migrated
behind `core::IHttp` via the capabilities designed and approved this session:

- **Cancel token:** `HttpRequest::cancel` (`const std::atomic<bool>*`, polled before
  open + before each chunk read — baseline `stop_` granularity) + `HttpResponse::
  cancelled` (ok=false, partial body cleared via pure `finalizeCancelled()`). A
  consumer's own stop is distinguishable from network death (reconnect/backoff safe).
- **Persistent sessions:** `IHttpSession` + `IHttp::openSession(HttpSessionConfig)`;
  default forwarding impl keeps every FakeHttp test unchanged; `WinInetSession` holds
  one `InternetOpen` handle + KEEP_CONNECTION per fetch. Lifetimes match baseline
  exactly: hls session dies in `disconnect()` (fresh per re-handshake/re-pin), IHeart's
  lives with the object. This is the Phase 2/4 shape (Source/plugin holds a session;
  host hands `IHttp` as factory) — the fork rationale, proven.
- **Site 7 — `StreamSource::hlsHttpGet`:** signature/call-sites unchanged; 8 MB
  cap-and-keep, `pragma_no_cache`, `cancel=&stop_`, `final_url` fallback, ≥400 gate and
  read-error fail replicated consumer-side. Only `disconnect`/`hlsEnsureSession`/
  `hlsHttpGet` touched (3 diff hunks; zero live-loop symbols in the diff).
- **Site 8 — IHeart metadata:** 5 s session, same UA, Accept header, 4 MB cap,
  **nullptr cancel** (parity — boundedness stays the deliberate timeout).
  `IHeartRadio.cpp` is now **WinINet-free** (still `windows.h` for GetTempPathA).
- **New fields default inert** — the six previously-shipped sites are byte-identical
  (`pragma_no_cache=false` held; scrobble/group-c request tests still green through the
  refactored impl).
- `read_error` response flag = implementation-time survey correction; accepted
  residuals + fixture lesson recorded in `lessons.md`.

## Verification (two-layer, all green)
- **ctest 7/7**: the prior five + `http_cancel_test` (localhost socket fixture:
  mid-body abort **373 ms** vs a ~300 s body / 8 s timeout; keep-alive = **1**
  connection for 2 session fetches; pre-cancelled = zero network) +
  `iheart_request_test` (real consumer + injected fake; session config + request shape
  + all three endpoints). Fixture lesson: reset the session BEFORE joining server
  threads (pooled connection outlives fetches) + cap socket waits — 110 s → 2.6 s.
- **Real-world on 7of9 (all six gates passed):** digital iHeart resolves + plays clean;
  **mid-segment stop aborts <1 s** (the regression this design must not introduce);
  stop-during-station-switch clean; now-playing + Discord art intact; ad-onset re-pin a
  non-event; ICY/SHOUTcast behaviorally unregressed.

## Rest of Phase 1 — the next fork (pick a seam or the reorg)
- **IPC:** Discord named-pipe → abstract interface (Linux = Unix socket).
- **Notifications:** Toast/PowerShell → abstract (Linux = libnotify/notify-send).
- **CD access:** Windows IOCTL → SG_IO on Linux.
- **The `src/core` + `src/platform/{win,linux}` directory reorg** — `IHttp.h` →
  `include/core/`, `HttpWinInet.cpp` → `src/platform/win/` (use `git mv`, one logical
  move per commit; grep-built leak map first; keep buildable).
- Parked (see `roadmap.md`): wiring `stop_` into IHeart polls (behavior improvement,
  separate decision); stalled-connect prompt interrupt (hardening); concurrency debt.

## Current state
- **Branch:** `restructure`; code commit `4c72b09` + docs commit local — push when ready.
- **Tests:** 7/7 green via `ctest` (5 prior + `http_cancel_test`, `iheart_request_test`).
- **Build:** clean, `remoct.exe` at `build\bin\remoct.exe`.
- **This `E:\code\remoct` clone has the full MSYS2 UCRT toolchain + deps** — full build +
  ctest run locally; only interactive/real-world checks (rip / scrobble / stream) need 7of9.
- The `sniffiheartradio/` probe keeps its own standalone WinINet copies of IHeartRadio —
  intentionally divergent, unaffected by the migration.

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
