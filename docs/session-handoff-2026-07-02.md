# Session handoff — 2026-07-02 (rev 5: slice 8 landed — CD-I/O seam in; PHASE 1 SEAMS COMPLETE)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(The prior handoff `session-handoff-2026-07-01.md` covers Phase 0 + the
negative-offset bug and is still valid history. This file's earlier revisions
covered HTTP 6/8 → 8/8 (slice 4), the slice-5 boundary, slice 6, then slice 7 —
superseded by this revision, which adds slice 8, the LAST platform seam.)

## Pinned constraints (do NOT re-litigate)
- **The 150-sector offset is a physical property of the disc** — a design aspect of
  AccurateRip, worked out across the HydrogenAudio forum posts at
  https://hydrogenaudio.org/index.php/topic,97603.0.html. Correct by design. Never
  "fix" the 150. (Also in `lessons.md`, the `ar_crc.h` comment, the negative-offset commit.)
- **HTTP request bodies go over the wire as raw UTF-8 bytes — never widen them.**
- **Parity for a transport migration lives at the call sites, not the seam** — diff the
  read *loops*, not just the flags; and PROBE fire-and-forget baseline calls before
  migrating them (the slice-8 SET_SPEED was a lifelong ERROR_BAD_LENGTH no-op —
  dropped, not migrated; fixing it is parked).
- **Single-consumer(-class) seams take their interface by CONSTRUCTOR INJECTION** —
  DiscordRP holds an `IIpc*`, UIManager an `INotify*`, CDSource + CDRipper an
  `ICdIo*`; `core::ipc()/notifier()/cdio()` are link-time bridges only (no setters).
  `core::http()/setHttp()` remains the one transitional global (endgame DI).
- **The live StreamSource audio read loop stays OUT of any seam, permanently.**
  Audit invariant: `InternetReadFile` appears in StreamSource ONLY in `rawRead`;
  `InternetOpenUrl` only in `connect()`'s ICY branch.
- **The escape/UTF-16LE/base64 `-EncodedCommand` block in NotifyWinToast.cpp is frozen.**
- **CD seam audit invariant (new):** `DeviceIoControl`/`ntddcdrm.h` appear ONLY in
  `src/platform/win/CdIoWin.cpp`; CDSource.cpp keeps `windows.h` for `Sleep()` only
  (the DiscordRP precedent); CDSource.h is platform-clean (no windows.h) — nothing
  outside `src/platform/win/` touches a CD IOCTL.

## What this session accomplished
**Slice 8** (code `14aebec` + docs = this commit + a separate dead-code removal):
the CD-I/O seam — the FOURTH seam built INTO the slice-5 boundary and the LAST
Phase 1 platform seam. Survey → design → probe → sign-off → implement, in that order.
- `include/core/ICdIo.h`: factory `open(drive)` → `ICdDevice` (dtor closes;
  concurrent opens of one drive are CONTRACT), `readToc` (raw MSF — msf_to_lba and
  the "do NOT subtract 150" comment stay consumer-side verbatim),
  `lastSessionFirstTrack`, `readRaw(lba, sectors, want_c2, buf, size, got)`,
  `setSpeed`, `mediaPresent`, `model`. `want_c2` is EXPLICIT (buffer-size-implies-C2
  was the baseline's one Windows-only assumption; SG_IO needs CDB bits); `got` is
  surfaced (C2 probe + de-interleave key on it).
- `src/platform/win/CdIoWin.cpp`: every method = one baseline DeviceIoControl moved
  parameter-identical (lba*2048 DiskOffset quirk now impl-side; STORAGE_QUERY model
  parse moved byte-identical from CDSource::queryDriveModel).
- Consumers survive nearly verbatim: CDRipper's readSectors/retry/flush/frame450/
  preamble/Enhanced-CD logic all unchanged except `HANDLE`→`ICdDevice&` at the one
  transport line each. Worker takes the device as a moved `unique_ptr`.
- **F1 probe finding:** CDSource's "reset speed to max" sent a hand-rolled 6-byte
  struct — cdrom.sys rejects it with ERROR_BAD_LENGTH (probe-confirmed). DROPPED for
  parity (a failing fire-and-forget call and no call are identical); canonical
  adoption parked in roadmap.md. CDRipper's canonical-struct sites migrated
  byte-identical.
- **Header detox in-slice:** CDSource.h sheds windows.h/winioctl/ntddcdrm; CDTrack
  DWORD→uint32_t; MBLookup TOC-offset params follow (type-only — the diff shows
  types, zero logic). Rip-open error message lost its GetLastError code (was
  cross-thread-stale anyway; seam surfaces no OS codes — slice-4 residual precedent).
- Unified open flags (ripper's shape): CDSource residuals argued inert + recorded
  in lessons.md (share-mode widened; SEQUENTIAL_SCAN dropped).

## Verification (slice 8, two-layer, all green)
- **ctest 10/10**: prior nine + `cd_toc_test` (FakeCdIo via ctor injection through
  the REAL CDSource: Relish-shaped T1@182 pregap, Enhanced-CD data track +
  session-2 leadout, malformed-TOC clamps, model→offset, media checks).
- **THE hardware gate (this machine IS 7of9; drive G:, HL-DT-ST GHD3N +6):**
  identity-confirmed Relish (all 12 TOC LBAs matched baseline), then a full [A]
  rip through the seam via a temp headless harness driving the REAL public
  pipeline (AudioManager::openCD → CDRipper::start): **12/12 AR v2 conf 200;
  per-track crc_v1/crc_v2, all 12 pcm_crc32, frame450 global/local, the T1
  first-16-samples dump, and "C2 support: no" ALL diff-identical to the
  pre-migration baseline log** (`Joan Osborne - Relish (1995) - Copy/logs/
  rip_20260621_203243.log`); .bin cache saved. Gate output kept at
  `Music/re-moct/_slice8_gate/`. CD playback + seekTo(60) through the seam
  confirmed (position advance 1s/s); the rip ran with CDSource's device still
  open — the two-concurrent-opens contract exercised live. (Rip speed read
  4-5x on inner tracks — CAV ramp, not a regression; CRCs are the gate.)

## Rest of Phase 1 / next steps
- **Phase 1 platform seams are COMPLETE** (HTTP, boundary, IPC, notify, CD-I/O).
- Parked (see `roadmap.md`): canonical SET_SPEED in CDSource::open (behavior
  improvement, needs its own decision + listen test); corrupt-TOC session-2 OOB
  (latent baseline, preserved); wiring `stop_` into IHeart polls; stalled-connect
  prompt interrupt; concurrency debt where cheap; Track Info album-tag decode.
- **Then Phase 2: internal Source interface** (compile-time C++ ABC; refactor
  file/iHeart/ICY/CD sources onto it). See `roadmap.md` + `architecture.md`.

## Current state
- **Branch:** `restructure`. Slice 8 = code `14aebec` + docs (this commit) + F5
  dead-code removal (separate follow-up commit) — LOCAL, push when ready. Origin
  in sync through the slice-7 pair (`dde7041`/`3c70574`).
- **Tests:** 10/10 green via `ctest` (iheart_sm, ar_crc, notify_toast, http_seam,
  scrobble_request, group_c_request, http_cancel, iheart_request, discord_ipc,
  cd_toc).
- **Build:** clean, `remoct.exe` at `build\bin\remoct.exe`.
- **Layout:** `include/core/{IHttp,IIpc,INotify,ICdIo}.h` +
  `src/platform/win/{HttpWinInet,IpcWinPipe,NotifyWinToast,CdIoWin}.cpp`; Toast.h
  is the consumer-side content adapter in `include/`; everything else flat until
  Phase 2/3 moves it. `src/platform/linux/` still absent (Phase 3).
- **This machine IS 7of9** (hostname-confirmed this session) — full MSYS2 UCRT
  toolchain, deps, AND the optical drive; build + ctest + hardware gates all run
  locally.
- Gotcha: the harness shell's cwd can silently reset to `E:\code` (outer junk repo)
  — `cd /e/code/remoct` explicitly in git one-liners. Running built exes needs
  `/c/msys64/ucrt64/bin` on PATH (exit 127 = missing DLLs, not a crash).

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
- **Design-first, no code until the boundary/plan is confirmed.** Survey (verified,
  file:line), propose, flag risks, stop for sign-off. Probe-first for anything
  touching a device or protocol.
- **Verify → build → ctest → real-world → commit.** No commit until the real-world
  gate passes. Commit code and docs separately. No `Co-Authored-By` trailer.

## Standing rule (going forward)
At the END of every working session, **update `roadmap.md` + `lessons.md` and refresh the
handoff doc as the last step before stopping** — so the durable docs are always the source
of truth and the next session never depends on session memory.
