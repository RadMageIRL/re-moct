# Session handoff — 2026-07-02 (rev 4: slice 7 landed — notifications seam in; CD-IOCTL is the last Phase 1 seam)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(The prior handoff `session-handoff-2026-07-01.md` covers Phase 0 + the
negative-offset bug and is still valid history. This file's earlier revisions
covered HTTP 6/8 → 8/8 (slice 4), the slice-5 boundary, then slice 6 —
superseded by this revision, which adds slice 7.)

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
  dependency injection. Slices 6 and 7 set the precedent: single-consumer seams take
  their interface by CONSTRUCTOR INJECTION (DiscordRP holds an `IIpc*`; UIManager holds
  an `INotify*`; `core::ipc()`/`core::notifier()` are link-time bridges only — no
  setIpc()/setNotify() exists).
- **The live StreamSource audio read loop stays OUT of any seam, permanently.**
  `producerWorker`/`producerWorkerAAC` → `rawRead`'s `InternetReadFile(hConn_)` → SPSC
  ring is raw WinINet by design. Audit invariant: `InternetReadFile` appears in
  StreamSource ONLY in `rawRead`; `InternetOpenUrl` only in `connect()`'s ICY branch.
- **The escape/UTF-16LE/base64 `-EncodedCommand` block in NotifyWinToast.cpp is
  frozen** — it fixed a real quote-injection bug (metadata from network stream tags
  breaking out of the old `-Command "..."` form). Do not "clean it up".

## What this session accomplished
**Slice 7** (code `dde7041` + docs = this commit): the notifications seam — the THIRD
seam built INTO the slice-5 boundary, and the smallest:
- `include/core/INotify.h`: ONE primitive, `notify(title, body)` — two strings, no
  icon/duration (the baseline toast never set either). Contract is the baseline's:
  fire-and-forget, non-blocking, best-effort, NO error reporting, any thread.
- `src/platform/win/NotifyWinToast.cpp` (`platform::win::WinToastNotify`): `git mv`
  from src/Toast.cpp — **rename held at 64%**, injection-fix history preserved. The
  PowerShell transport (esc → WinRT toast script → UTF-16LE/base64 `-EncodedCommand`
  → detached CreateProcess, 5 s bounded reap) moved byte-identical; the body lives as
  a file-static at ORIGINAL indentation with the class as a thin adapter — that's what
  held the rename (see the new lessons.md entry: re-indenting a moved body into a
  class kills similarity detection).
- CONTENT stayed consumer-side (the IHttp/IIpc split): Toast.h is now a header-only,
  platform-free content adapter `showTrackToast(title, artist, album, INotify&)` —
  baseline mapping verbatim (artist PREPENDS title; empty album → "RE-MOCT" body),
  including the status-shape inversion (("Streaming", station, "") → "station -
  Streaming" — baseline, not fixed). `'RE-MOCT'` CreateToastNotifier id stays
  impl-side (platform attribution; Linux twin = `notify-send -a`).
- **Ownership: constructor injection** — UIManager is the ONE consumer class (33 call
  sites, all in UIManager.cpp, all UI-thread): `UIManager(..., core::INotify* =
  nullptr)` defaulting to `core::notifier()`. A private 3-arg member wrapper hides the
  4-arg adapter → all 33 call sites compiled textually unchanged.
- Out of scope, deliberately untouched: only the track-change toast honors
  `config_.toast_enabled` (status toasts fire unconditionally) — baseline behavior.

## Verification (slice 7, two-layer, all green)
- **ctest 9/9**: prior eight + `notify_toast_test` (portable — no PowerShell, no
  process spawn): FakeNotify through the REAL Toast.h adapter — mapping branches,
  the dominant (msg,"","") status shape, the Streaming inversion,
  adapter-must-NOT-escape (escaping is the transport's job), call ordering.
- **Live gate (passed on this machine):** toasts fire from the real triggers (track
  change with toast_enabled on; status toggles); probe toasts through the real seam
  confirmed in Action Center — quote-injection case ('/" in metadata, the past bug's
  exact trigger) intact, accents (Björk/Jóga) render; a synchronous run of the
  identical script exited 0 (Show() succeeded, i.e. the toast actually rendered).

## Rest of Phase 1 — what remains
- **CD access: Windows IOCTL → SG_IO on Linux — the LAST platform seam.** Survey
  first: StreamSource/CDSource/CDRipper (where the IOCTLs live, who calls them, what
  moves). Interface → `include/core/`, Windows impl → `src/platform/win/`; pick
  accessor-vs-injection by consumer count.
- Parked (see `roadmap.md`): wiring `stop_` into IHeart polls; stalled-connect prompt
  interrupt; concurrency debt; Track Info album-tag decode artifact.

## Current state
- **Branch:** `restructure`. Slice 7 = code `dde7041` + docs (this commit) — LOCAL,
  push when ready. Origin is in sync up through the slice-6 pair (`89285d8`/`ca38511`).
- **Tests:** 9/9 green via `ctest` (iheart_sm, ar_crc, notify_toast, http_seam,
  scrobble_request, group_c_request, http_cancel, iheart_request, discord_ipc).
- **Build:** clean, `remoct.exe` at `build\bin\remoct.exe`.
- **Layout:** `include/core/{IHttp,IIpc,INotify}.h` +
  `src/platform/win/{HttpWinInet,IpcWinPipe,NotifyWinToast}.cpp`; Toast.h is the
  consumer-side content adapter in `include/`; everything else flat until its seam
  migrates. `src/platform/linux/` still absent (Phase 3).
- **This `E:\code\remoct` clone has the full MSYS2 UCRT toolchain + deps** — build +
  ctest run locally; interactive/real-world checks run here too when they don't need
  hardware (the slice-6 Discord gate and the slice-7 toast gate both ran on this
  machine); a rip needs 7of9's drive.
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
