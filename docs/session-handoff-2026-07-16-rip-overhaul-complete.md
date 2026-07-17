# Session handoff — 2026-07-16 ~23:15 — decode formats + THE RIP OVERHAUL, COMPLETE

One session, eight slices, all committed and pushed to
`experimental/win-pdcurses`. Every slice ran the cadence: PLAN (probe the
live tree, report divergences) → greenlight → IMPLEMENT → machine gates +
hardware gate → STOP → debrief → explicit commit greenlight. All CI green
through `146638b`; the final push `2e53de7` was still mid-CI at handoff —
CHECK ITS VERDICT FIRST THING (both matrix jobs; expect green — local ctest
was 22/22 Win, 23/23 Linux, and no CI-visible surface changed beyond what
built here).

## The eight slices (chronological, each commit message is its own record)

| commit | slice | headline |
|---|---|---|
| `ebfaf1c` | decode-opus-wv | Opus+WavPack playback; found+fixed the R128-read silence bug; TagLib opusfile.h collision (CI grep-guard born here) |
| `4002f9b` | decode-ogg-vorbis | Vorbis playback (repaired the dead .ogg gate entry); Opus decode → f32 |
| `e071b0f` | rip-encoder-seam | IEncoder seam under ripTrack; **Joan Osborne gate: 12/12 AR v2 conf 200, 24/24 files byte-identical pre/post**; frozen-inline oracle born |
| `b61592b` | rip-format-select | modal format block (digits toggle), RipOptions plumbing, flac_level/mp3 keys; cue/m3u8 master rule |
| `f5ea409` | rip-wav-encoder | WAV out; strict-writeFrames fold-in + /dev/full test; taggable/note row properties |
| `146638b` | rip-opus-encoder | Opus out via libopusenc; **R128Gain.h = both dialect directions, one home**; end-to-end: R128 −3052 ↔ −6.92 dB == FLAC's RG |
| `2e53de7` | rip-wavpack-encoder | WavPack out, zero new deps; **IEncoder::finalize → bool**; the three-layer ENOSPC laundering trap closed |

**End state reached:** rip modal = FLAC/MP3/WAV/Opus/WavPack, digits 1-5,
any combination, one verified disc read. Playback now also decodes
.opus/.wv/.ogg. Version: **1.3.0 UNRELEASED** (CHANGELOG accumulating; Dos
confirmed folding everything into it).

## Outstanding — Dos's holds (not code)

- **Eyes-on modal**: full five rows (WavPack digit 5, `*`, `normal`).
- **Listening passes**: Opus 128k-VBR default is a taste call
  (A/B `C:\Users\david\smoke\subO\01.opus` vs `subF\01.flac`); WavPack/WAV
  optional ear checks.
- **CI verdict on `2e53de7`** (pending at handoff).

## Recorded next slices (design-of-record in each plan doc)

- **Log semantics** (own slice, small): mark lossless outputs (FLAC/WAV/
  WavPack) as verifiable masters vs derived lossy (MP3/Opus) in the rip log.
  The `lossless` row property is already in place — this is a log-writer edit.
- Deferred at various points: "save selection as default" from the modal;
  hybrid .wvc; per-slice §9 notes in the plan docs.

## Harness inventory (KEEP — C:\Users\david\smoke\)

- `riprobe.cpp` / `riprobe-wv.exe` (newest): **headless rip driver** —
  `riprobe <drive> <out> [A|C|Y|B] [formats f/m/w/o/k] [ntracks] [cancel_at_track] [cancel_after_s]`.
  cancel_at_track=3 with full TOC = the trimmed 2-track [A] gate (tracks 1-2
  get REAL network AR verdicts; tagging skipped on cancel → compare at audio
  level). Older exes (riprobe-seam/-fmtsel/-opus) = per-slice builds.
- `probe.cpp` / `probe-cur.exe`: **decode probe** over LocalFileSource —
  prints ch/sr/duration/rg_db/RMS/seek/tail. THE tool for auditions + RG
  read-back checks.
- **`preflight1/`, `preflight2/`**: the slice-1 12-track [A] baselines
  (byte-identity anchors — tag-SHA comparisons still reference
  `subF/01.flac`, `subM/01.mp3`). Do not delete.
- Gate artifacts: gateW/gateO/gateK, idK, subO/subK/subF/subM, defF2/defM2.
- `wvfail.c`: the libwavpack failure-propagation probe (documented finding).
- `modalshot.ps1`/`step.ps1`: wingui screenshot driver (worked for browsing;
  Ctrl+Y never arrived as curses keycode — modal checks are human).

Relish is (was) in the GHD3N at G: (offset +6). AR disc: 12 tracks, T1@LBA
182, track CRCs a377572a / a0c5b3c2 (conf 200) — the canonical gate values.

## Hard-won environment facts (this machine, this workflow)

- **WSL Debian**: reach via `wsl -e bash` from PowerShell; repo at
  /mnt/e/code/remoct; inner-loop tree ~/rb (rsync from repo, build ~/rb/build);
  **/tmp does NOT persist between wsl invocations** — use ~; write scripts to
  a file + `sed -i 's/\r$//'` (CRLF kills bash); `wsl -u root` works for apt.
- **Git Bash** (the Bash tool): repo at /e/code/remoct; NO /mnt; prefix
  builds with `export PATH="/c/msys64/ucrt64/bin:$PATH"`.
- **Long rips**: Start-Process detached + background watcher loop (bash
  run_in_background). NEVER `bash -lc` via Start-Process (login shell hangs
  silently — burned once).
- **GitHub API 503s happen**; watchers must retry with `2>/dev/null || sleep`.
- Deps installed in WSL this session: libopus/opusfile/opusenc/wavpack/
  vorbis -dev + sox/opus-tools/wavpack/vorbis-tools/opustags/lame/flac.
- Windows release bundle note: dynamic build now needs libopusenc-0.dll
  (Dos's collector script, outside the repo, already seeded).

## The lessons that must not be re-learned (full text in docs/lessons.md)

1. **Byte-identity refactors**: freeze the old code verbatim in a test as a
   reference arm (rip_encoder_seam_test) — the reference IS the contract.
2. **The laundering trap has THREE layers**: lenient write, block-callback
   indirection, and **stdio buffering** (found BY the ENOSPC test failing on
   its first run — reasoning alone missed it). Every buffering encoder's
   finalize does fflush-and-check; IEncoder::finalize returns bool.
3. **Dialect conversions live in ONE home** (R128Gain.h) — encode/decode
   share the same two functions; drift is unrepresentable.
4. **TagLib ships colliding basenames** (opusfile.h, vorbisfile.h, wavpack
   basename): codec includes always prefixed; CI grep-guard enforces.
5. **tagFile survived five slices with additive-only edits** — the
   else-if-per-format shape + whole-file SHA re-proof each time.
