# Session handoff - 2026-07-01 (Phase 0 + negative-offset bug)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.

## Pinned constraint (do not re-litigate)
The **150-sector offset is a physical property of the disc** - a design aspect of
AccurateRip, worked out across the HydrogenAudio forum posts at
https://hydrogenaudio.org/index.php/topic,97603.0.html. It is correct by design.
Never "fix" the 150. (Also recorded in `lessons.md`, the `ar_crc.h` comment, and the
negative-offset fix commit.)

## What this session accomplished
- **Repo wired to GitHub.** `E:\code\remoct` went from loose files (manual
  drag-drop air-gap copy) to a real clone on branch **`restructure`**, tracking
  `origin/restructure`. Reconciled against `origin/dev` with `git reset --mixed`
  (kept local files), restored dev's missing probe/aux files so local ⊇ dev, kept
  the new docs.
- **Nested-repo handled.** Parent `E:\code` is a junk remote-less git repo; added
  `remoct/` to `E:\code\.gitignore` so it stops tracking the real repo. `remoct`'s
  own `.git` is its root.
- **Knowledge base committed** - `CLAUDE.md` + `docs/*.md`, `.gitignore`, README.
- **Phase 0 COMPLETE** (both extractions landed, tested, committed on `restructure`):
  - iHeart now-playing decision logic → pure `IHeartNowPlayingSM.{h,cpp}` (threadless,
    no network, no windows.h). `StreamSource` is now a thin adapter calling it.
    Verified: `iheart_sm_test` green + live digital-stream check on 7of9.
  - AccurateRip CRC math → pure `ar_crc.{h,cpp}` (`TrackCrc`, `frame450Crcs`).
    Verified: `ar_crc_test` green + real Joan Osborne rip 12/12 AR v2 conf=200.
- **Parked bug RETIRED** (commit `1a2235a`): negative drive-offset preamble OOB read
  + wrong CRC phase. Root cause: truncating `/` and `%` on a negative `total_skip`
  gave a negative `sub_skip` → (a) `pbuf.data()+sub_skip*2` underflowed the buffer
  (OOB read), (b) main path gated `> 0` silently dropped it → wrong CRC phase. Fix:
  pure helpers in `ar_crc.h` - `normalizeSkip()` (floored decomposition, `sub_skip`
  always `[0,588)`, byte-identical to old math for non-negative input) and
  `arPreambleReadable()` (signed bounds guard replacing a `(DWORD)` cast that wrapped
  negatives to ~4e9). Wired into `CDRipper` preamble + main paths, dropped the `> 0`
  gate, switched LBA math to signed. 150 preamble untouched.

## Current state
- **Branch:** `restructure` (off `dev`), pushed to `origin/restructure`.
- **Tests:** `iheart_sm_test` + `ar_crc_test`, both green via `ctest`.
- **Build:** clean, `remoct.exe` at `build\bin\remoct.exe`.
- **Open caveat:** the negative-offset fix is proven in **logic** (synthetic tests)
  and **non-regressing** on the +6 drive (Joan Osborne re-rip, byte-identical CRCs).
  It is NOT validated on real negative-offset **hardware** - Dos has none. That final
  validation stays an open **HydrogenAudio cross-check item**.

## Build / test commands (7of9, MSYS2 UCRT64)
Build (also builds the test targets):
```
C:\msys64\usr\bin\bash.exe -l -c 'export PATH=/ucrt64/bin:$PATH && cd /e/code/remoct && rm -rf build && mkdir build && cd build && cmake .. -G Ninja && ninja'
```
Run tests:
```
C:\msys64\usr\bin\bash.exe -l -c 'export PATH=/ucrt64/bin:$PATH && cd /e/code/remoct && ctest --test-dir build --output-on-failure'
```
- **Build FIRST, then ctest** ("No tests were found" = ctest ran before a build).
- If a fresh test target is added and ctest can't find it, prefer the explicit
  `cmake -S . -B build -G Ninja && cmake --build build` form - it pins the source dir
  and forces reconfigure, avoiding stale-cache / wrong-root ambiguity.
- The main build's target count (e.g. `26/26`, `30/30`) is NOT the test signal -
  tests compile in the `tests/` subdir; the real proof is `ctest` finding + running
  them. Trust `ctest` output, not the link-line count.

## Operating lessons for the local agent (this session)
- **Verify before commit - always.** The agent repeatedly tried to commit before the
  build/tests were confirmed. Hold the line: build → ctest → (for CD work) re-rip a
  known disc → THEN commit. A commit should be a known-good checkpoint.
- **Two-layer verification.** `ctest` proves the *logic* off-disc; running the real
  `remoct.exe` (rip a known disc / play a stream) proves *real-world + regression*.
  For CD math, the Joan Osborne 12/12 conf=200 re-rip is the regression gate.
- **Trust the build/output over the agent's claims.** There was a file-sync episode
  where the agent asserted edits/wiring were present but the build contradicted it.
  Ground truth = what compiles and what `ctest`/the rip say.
- **VS Code agent > PowerShell agent** for this work - visible diffs, edits land
  reliably, less "which file where" confusion. Same model, better harness.
- **Design-first, no code.** Have the agent propose the approach (root cause, fix,
  test plan) and confirm the boundary before it writes - catches wrong turns cheaply.
- **Split by verification method.** iHeart (offline test) and ar_crc (disc rip) were
  committed as separate batches because they verify differently. Keep that pattern.
- **Header-only helpers are fine.** `normalizeSkip`/`arPreambleReadable` live inline
  in `ar_crc.h` (no `ar_crc.cpp` change needed) - that's correct, not an omission.
  A clean build/link is proof nothing's missing.

## Where the roadmap stands
- Phase 0 ✅ done. Negative-offset bug ✅ retired (logic proven; hardware validation
  open per above). Update `roadmap.md` to move that entry out of "Parked" (small
  docs commit).
- **Next: Phase 1 - platform abstraction / cleanup** (the `src/core` +
  `src/platform/{win,linux}` split; HTTP consolidation 8 WinINet → one `http_get`;
  IPC / notify / CD-IOCTL seams). This is **large and invasive** - start it in a
  FRESH, unhurried session, in VS Code, and break it into small pieces (e.g. HTTP
  consolidation first) rather than one marathon.
- Later: Phase 2 (internal Source interface), Phase 3 (Linux port), Phase 4 (iHeart
  as the first loadable plugin). Spotify remains dropped.
- Lighter alternative bites if wanted: scrobble dedup, ICY ingest sanitization.

## Working-tree note
Old dated `binary-dev/*.exe` build artifacts and `re-moct.png` were pending deletion;
`.gitignore` should cover `binary-dev/*.exe` / `*.exe` so artifacts don't get
re-added. Keep `binary-dev/lb_probe.cpp` (source); ignore its `.exe`.
