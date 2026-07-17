# Session handoff — 2026-07-17 ~12:45 — the full-day flight (12 slices + 2 fixes)

One session, everything on `experimental/win-pdcurses`, every push CI-green
through `c62ca85` (the last two commits' runs pending at handoff — check
first thing: `7dc403b` batch-r128 and the docs commit after it; expect
green, locals were 26/26 Win + 27/27 Linux).

## Shipped today (chronological; each commit message is its own record)

| commit | slice | headline |
|---|---|---|
| `c333428` | stream-record R1 | the capture engine (SPSC ring tap, worker, split/tag/RG); pause finding |
| `52f6cf0`+`9f8c393` | stream-record R2 | [Rec] panel, ^E, [REC] badge; Dos confirmed all eyes-on gates |
| `e7c3fc8` | mp3-rg-read | MP3 RG finally applies; blob-in-description root cause PROBE-PROVEN |
| `8a36b4c` | mp3-rg-write | proper desc/value TXXX (7 frames + recorder's 2); subM3 = new tag contract |
| `76798ad` | log-semantics | Formats/Master lines; "AR verifies the READ, lossless RETAINS it" |
| `8f06fab`+`cb8baf1` | rec-cover-art | per-cut covers via the pane machinery made pane-independent |
| `8038368` | split-trim + ad-aware | frame-exact hold (1200ms default) + one-classifier ad routing/discard |
| `758cf3b` | radio-art-refresh-fix | the missing floor-reset (shared radioArtFloor) + time-bounded neg cache |
| `254baca` | **abi-cluster slice A** | THE ABI OPENED: struct growth + keep-draining; gapless gate 0.00s/35s pause |
| `c62ca85` | slice-B plan (docs) | copy/remux resolved: M4A decisively (AacDecoder already does ADTS+MP4!) |
| `7dc403b` | batch-r128 | GainScan folder scan + THE HEAL + parity exact; help ×5 + [REC] pulse riders |

## Where things stand

- **Backlog = abi-cluster slice B only** (copy/remux), plan approved+committed
  (`docs/abi-cluster-slice-b-plan.md`): implement the three declared-NULL fns
  (tee ring plugin-side), FrameSync.h parser, the ADTS→M4A mux writer (the ONE
  new piece — round-trip oracle = the app's own AacDecoder), MP3 raw-append
  writer, recorder pull-callback copy mode, panel third format. Dos's locked
  decisions ride: AAC copies TAGGED (M4A, not bare .aac), in-app playback
  (already exists), 2 MiB tee drop-oldest + counter.
- **v1.3.0 still UNRELEASED** — CHANGELOG accumulating (now: decode formats,
  rip overhaul, stream recording + covers + hold + ad-aware, MP3 tag pair,
  log semantics, gapless pause, batch ReplayGain).
- **Dos's open seals** (live/eyes-on, at leisure): slice-A gapless LISTEN
  (pause mid-recording → play the cut); art-fix live cases (recording on
  zc4366, rotation on KKJO); batch-scan live run + the by-ear rip-vs-batch
  spot-check + the pre-fix-MP3s-readable-in-foobar heal check; trim by-ear
  dial-in; ad-aware two-station run; cover-art pane-closed run; subM3 in an
  external player; the [REC] pulse glance.

## The findings that must not be re-learned (full text in lessons.md)

1. **LP64 (long)(uint32-diff) inverts wrap compares on Linux** — use
   (int32_t). FLAGGED unfixed: np_pub_q_'s release check uses (long).
2. **TagLib File objects open RW by default** — a live handle silently
   blanks a concurrent FileRef read on Windows (sharing violation swallowed
   → metadata skipped). Scope tightly.
3. **Additive ABI growth**: no version bump; struct_size REACH-check before
   reading appended pointers; per-fn null = per-feature capability. One
   truth in the plugin / two views in the host; gapless proven structurally
   then empirically.
4. Earlier today (also in lessons.md): pause loses the broadcast for any
   host-side tap; split triggers must not depend on audio flowing; linkage
   norms shape API homes; the blob-TXXX root cause.

## Environment/harness notes (memory also updated)

- Test streams: KKJO ICY/AAC `stream.abacast.net/direct/eagleradio-kkjofmaac-ibc4`;
  real iHeart HLS `stream.revma.ihrhls.com/zc4366/hls.m3u8`; SomaFM = slow control.
- Harness exes rebuilt-per-slice in smoke/: riprobe-txxx, probe-rgfix,
  rgprobe, id3strip (syncsafe-correct), srec_live (build/bin — now with
  pause_at/pause_len + the silence-window scan). The g++ recipe is in the
  07-17 morning handoff + memory (shell32/uuid for musicRoot).
- **smoke/subM3/ = the MP3 tag contract; subM = the legacy-blob specimen;
  rg_parity/ = scratch parity copies (regenerate at will). Do not delete
  subM3/subM/subF/subO/preflight*.**
- gain_scan_test's artifact-parity leg: env `GAIN_SCAN_ARTIFACTS=<dir with
  01.flac+01.opus copies>`; CI runs the synthetic suite only.

## Next flight

Slice B implement (the brief exists in the plan doc), then v1.3.0 release
prep is plausibly next (CHANGELOG is rich; Dos's call). The recorded
later-items ledger: album gain for batch-r128, GainScan recursion, np_pub_q_
LP64 look, keep-draining copy interplay verified in B, .wvc hybrid,
per-slice §9 notes.
