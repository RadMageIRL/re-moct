# Session handoff — 2026-07-17 — stream-record R1 shipped; R2 planned

## Shipped and pushed (`experimental/win-pdcurses`, CI GREEN on `c333428`)

- `18c3104` — docs: the 2026-07-16 flight debrief (handoff + CLAUDE.md
  block) that was pending from last night.
- `c333428` — **feat(record): the stream-capture engine (slice R1)**. The
  commit message is the full record; headlines:
  - `StreamRecorder` (include/StreamRecorder.h + src/StreamRecorder.cpp):
    three lanes — audio-thread tap = one atomic check + one bounded memcpy
    into a 4 MiB SPSC ring (drop-whole-block + counter on overflow, never
    blocks); dedicated worker drains/converts/encodes (Mp3Encoder +
    OpusRipEncoder direct), rolls cuts on the split flag, tags (RG TXXX on
    MP3, R128 Q7.8 via R128Gain.h on Opus, track gain only) with per-cut
    ebur128 RG; UI surface = start/stop/onTitle (internal dedup) + atomic
    state accessors.
  - Tap in AudioManager's stream branch AFTER readFrames, BEFORE
    volume/EQ/mute (the BPM undecorated-signal doctrine). Recorder stop
    precedes `stream_plugin_.close()` at play()/stop()/openCD()/
    startStreamConnectLocked() + dtor; the connect-fail close sites need no
    hook (that stream never played).
  - `CDRipper::musicRoot()` extracted verbatim (+ `recordingsDir()` =
    `<music>/re-moct/recordings`); `sanitizePath` body → StringUtils.h
    `sanitizePathComponent` (delegating wrapper stays).
  - `stream_record_test` froze the contract headlessly (frame-exact splits,
    both tag dialects read back, banded R128 value, timestamp fallback,
    split-off, overflow honesty, parse edges).

## Gates (all machine gates run this session)

- Windows ctest **23/23**, Linux **24/24** (xfade target now carries the
  recorder TUs — AudioManager owns the engine).
- Trimmed CD gate (GHD3N, full TOC, [A], cancel@3): T1 `a377572a` / T2
  `a0c5b3c2` both AR v2 conf 200; T1/T2 FLAC audio MD5 + pcm_crc32
  **byte-identical to preflight1** → musicRoot extraction inert.
- Live gates through the REAL AudioManager (harness `srec_live.exe`, ends
  via the production stop() teardown): SomaFM ICY/MP3 150 s (real on-air
  split at 132 s) and **KKJO AAC 300 s (Dos's metadata-rich test stream:
  `https://stream.abacast.net/direct/eagleradio-kkjofmaac-ibc4`)** — real
  split at 193 s, dropped=0 across 12.8M frames, cut length matches the
  event to the second. All cuts decode through LocalFileSource (durations
  exact, seek OK); Opus R128 round-trips (-4.89/-6.65 dB).

## Findings (recorded in lessons.md, full text there)

1. **Pause loses the broadcast** — producers halt on paused_, readFrames
   silence-pads (probe: rms 0, segment_gets frozen). Record-through-pause =
   silence gap + lost airtime; unfixable host-side. R2 surfaces the copy;
   keep-draining-while-paused is a recorded plugin conversation.
2. **Split must not depend on audio flow** — the idle-ring roll stall,
   found by the contract test before any UI existed.
3. **Linkage norms shape API homes** — CDRipper TU never links into tests →
   sanitize body lives in StringUtils.h; recorder takes dirs from callers.
4. Pre-existing, NOT R1: MP3 RG TXXX is not parsed on decode (rip baseline
   subM/01.mp3 probes rg_db=0.00 identically). Its own small slice later;
   affects rips and recordings equally.

## Outstanding — Dos's holds

- **Listening passes:** the gate cuts are in
  `C:\Users\david\Music\re-moct\recordings\` (`KKJO FM\`, `SomaFM Groove
  Salad\`) — play them in RE-MOCT; plus the playback-glitch listen during a
  live capture (rides naturally with R2's UI gate).
- **R2 plan review:** `docs/stream-record-r2-plan.md` (UNCOMMITTED, awaiting
  greenlight) — panel design, ^E re-confirmed free, badge in drawTitleBar's
  modes string, deinvertArtist hoist (articles-only caveat flagged), config
  keys rec_format/rec_split/rec_dir, the now-drivable UI-responsiveness
  gate. This handoff + the lessons/CLAUDE.md edits are also uncommitted —
  they ride with the next greenlight.

## Harness additions (C:\Users\david\smoke\ + build/bin)

- `riprobe-srec.exe` — riprobe rebuilt against the R1 tree (current-tree
  rule). Recipe reconstructed (was unscripted): the xfade-shaped g++ line —
  all AudioManager/CDRipper/decoder/encoder TUs, `-Iinclude -Ilib
  -I/c/msys64/ucrt64/include/opus`, libs FLAC/mp3lame/ebur128/fdk-aac/
  opusfile/opusenc/opus/vorbisfile/vorbis/ogg/wavpack/tag/wininet/winmm/
  ole32/**shell32/uuid** (the last two for SHGetKnownFolderPath/
  FOLDERID_Music in musicRoot).
- `probe-srec.exe` — decode probe rebuilt (LocalFileSource + decoder TUs).
- `build/bin/srec_live.exe` — the R1 live-capture harness (real
  AudioManager: connect → device → tap → recorder → production stop()).
  Usage: `srec_live <url> <secs> [station]`. Rebuild against the tree
  before reusing.
- `srec_gate/` in smoke = this session's trimmed-gate output (kept as
  evidence beside the preflight baselines).

## Next

R2 IMPLEMENT on greenlight: `UIOverlay::RecPanel`, ^E, drawRecPanel
(settings + recording views, pause-gap copy), [REC] badge, tick-loop
onTitle wiring, config keys, deinvertArtist hoist + recorder adoption,
CHANGELOG user-facing entry. Then the recorded later slices (ad-aware
split, copy/remux, art, trim, log semantics, MP3-RG read-back).
