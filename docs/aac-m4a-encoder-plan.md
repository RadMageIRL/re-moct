# AAC/M4A rip — Phase 1 plan: M4aEncoder + three-path parity

Scope ID: `aac-m4a-encoder`. Stage: **plan-first** (this doc is the deliverable;
nothing built or committed yet). Branch `experimental/win-pdcurses`. Built on
`aac-m4a-phase0-probe` (FDK encoder + hand-rolled MP4 mux proven, no new dep, seeks
correctly, clean in RE-MOCT + FFmpeg + Apple/WMP per Dos).

All anchors below were resolved against the live tree. **Seven findings (F1-F7)
diverge from or refine the brief — read those first; two carry a decision for Dos.**

---

## Findings (divergences / refinements from the brief)

**F1 — the convert art WRITER needs an M4a case (the brief said it's already there).**
`ArtEmbed::extractEmbeddedArt` *reads* MP4 `covr` by extension, but the *writer*
`ArtEmbed::embedArt(path, RipFormat, blob)` switches on `RipFormat` and has cases
only for Mp3/Flac/Opus/WavPack — `Wav`/`default` returns false. Convert-to-M4A art
would silently no-op today. **Add `case RipFormat::M4a`** (TagLib `MP4::File` +
`covr`, the exact path the probe used). Small, but required.

**F2 — the rip tag/art/RG writer is a SEPARATE inline path (`CDRipper::tagFile`), not
ArtEmbed.** `tagFile` writes tags, art, AR, and ReplayGain inline per extension
(`is_mp3`/`is_flac`/`is_opus`/`is_wv`) — it does NOT call `embedArt`. So rip M4A needs
its own new `else if (is_m4a)` branch: MP4 title/artist/album/track/year, AR + plain
`REPLAYGAIN_*` as `----:com.apple.iTunes:` freeform atoms, and a `covr` atom from the
art bytes. Not mentioned in the brief; it is required for rip M4A to be tagged at all.

**F3 — MP4 ReplayGain reads back with ZERO decode-side change (proven, not assumed).**
`probes/aac-m4a/rgcheck.cpp` writes `----:com.apple.iTunes:REPLAYGAIN_TRACK_GAIN` and
reads it back through TagLib's generic `PropertyMap` as `REPLAYGAIN_TRACK_GAIN =
'-6.92 dB'` — exactly the key `LocalFileSource` (`tryRG`, LocalFileSource.cpp:81)
reads. So M4A playback RG works for free; the slice does NOT touch the decode path.
The brief's "Phase-1 verify" is satisfied structurally; `m4a_encoder_test` locks it.

**F4 — build fan-out: two factory edits pull `M4aEncoder.cpp` into five existing test
targets.** `makeEncoder` (EncoderFactory.cpp) and `makeRecEncoder`
(StreamRecorder.cpp) both gain an M4a case, so every target that links either TU now
references `M4aEncoder` and must also list `src/M4aEncoder.cpp`:
`convert_core_test` + `art_embed_test` (EncoderFactory), `stream_record_test` +
`copy_mux_test` + `xfade_handoff_test` (StreamRecorder), plus the `remoct` target.
FDK include/lib is already on all of them except `stream_record_test` (add
`${FDKAAC_INCLUDE}`/`${FDKAAC_LIB}` there). This is mechanical but must be complete or
the link breaks.

**F5 — the convert test matrix auto-expands to cover M4A both directions (free).**
`convert_core_test` builds "each input format -> each output format" over
`kRipFormats`. Once M4A is row 6, it becomes both a convert *source* (decoded via the
already-linked `AacDecoder`) and a *target* (via the new `M4aEncoder`). Lossy
round-trip tolerance is already how MP3/Opus are asserted there, so M4A slots into the
existing pattern — real coverage for the cost of adding the source TU (F4).

**F6 — DECISION: the rec third is the heaviest part, for the use case the brief itself
says Copy beats.** Rip and convert are the natural M4A homes. The rec path costs the
most (a 4th rec-panel row + key, a `tagCut` M4a branch, the start() validation guard,
`RecOptions` + rec Config keys, `makeRecEncoder`, and the three StreamRecorder test
link-sets from F4) — and the brief notes Copy is strictly better than M4A-rec for AAC
broadcasts. Two ways forward:
- **(a) Full three-path now** (as briefed) — one slice, complete parity, heavier diff.
- **(b) Rip + convert now, rec-M4A as a fast follow** — smaller, lower-risk slice; rec
  keeps Opus/Mp3/Copy (Copy already the right tool for AAC).
Recommendation: **(a)**, since the brief already weighed the Copy tradeoff and asked
for the note, not the removal — but flagging so it's an accepted cost, not a surprise.

**F7 — DECISION (only if F6=a): the rec panel key numbering.** Rec rows today are
Opus(1)/Mp3(2)/Copy(3), `rec_focus_` bounded 0..2, Copy special-rendered after the
`rows[]` encode array. M4A joins `rows[]` as the third *encode* format, which puts Copy
last: **Opus(1)/Mp3(2)/M4A(3)/Copy(4)**, `rec_focus_` 0..3. That renumbers Copy's key
3 -> 4 (a small muscle-memory change). Alternative: keep Copy(3), append M4A(4) — but
that separates M4A from the other encoders and reads oddly. Recommendation: regroup
(Copy -> 4); it's the sensible grouping and Copy-as-last-special-option is clear.

**Confirmed unchanged (by non-action):** `rip_encoder_seam_test` constructs
FLAC/MP3/WAV/WavPack directly and never links `EncoderFactory`, so it neither sees M4A
nor changes — byte-identity holds exactly as the brief states. The rip/convert digit
selectors are already `ch <= '0' + kRipFormatCount`, so digit `6` selects M4A for free.

---

## The encoder — `M4aEncoder` (finalize-time mux)

New `include/M4aEncoder.h` + `src/M4aEncoder.cpp`, implementing `IEncoder`. The
container's `moov` sample table needs the full AU list before any bytes are written, so
this encoder is structurally unlike the streaming ones:

- **`open(path, total)`** — open FDK (`aacEncOpen`, `AOT_AAC_LC`, 44100, stereo,
  `TT_MP4_RAW`); set the mode from the ctor (`AACENC_BITRATEMODE = level` for VBR, or
  `= 0` + `AACENC_BITRATE` for CBR); run the `aacEncEncode` init call; capture the ASC
  from `aacEncInfo().confBuf`. Store `path`; do NOT open the file yet (nothing to write
  until finalize). Keep a small PCM residual buffer.
- **`writeFrames(s16, n)`** — stage into a residual buffer and feed FDK in whole
  1024-frame units (mirroring `Mp3Encoder`'s deinterleave-staging discipline); collect
  each emitted access unit (bytes + size) into an in-memory `std::vector` AU list. No
  container writing here.
- **`finalize(ok)`** — if `ok`: flush FDK's tail (`numInSamples = -1` drain), assemble
  the file with **`Mp4Mux::muxM4a`** (lifted verbatim to `include/Mp4Mux.h`) from the
  ASC + AU list, and do the single `fwrite`. **Return whether that write completed** —
  a buffering encoder only learns of disk-full at flush (the WavPack/ENOSPC lesson), so
  a short final write returns false and the caller treats the track as failed and
  removes the partial. If `!ok`: release FDK, write nothing.
- **Ctor (vbr-first, per the Mp3Encoder lesson):**
  `explicit M4aEncoder(bool vbr = true, int vbr_level = 4, int cbr_bitrate_bps = 128000)`
  so a default construction is the sane default (VBR level 4).

**Memory cost (explicit, accepted):** the AU list buffers one whole track's AAC in RAM
until finalize — a 5-minute track at ~128k is ~5 MB, the same order `AacDecoder`
already slurps per file. Bounded, per-track, freed on finalize. No streaming-mux
gymnastics at this size.

**LP64 note:** `Mp4Mux.h` and the encoder use explicit `uint32_t/uint64_t/size_t`
only — no miniaudio types, so LP64 trap #2 does not arise in the encoder. It lives in
the *test*, which drives `ma_decoder_get_length_in_pcm_frames` — that call takes a
local `ma_uint64` (the probe's fix), never a `uint64_t*`.

---

## File-by-file diff shape

**New files**
- `include/Mp4Mux.h` — lifted verbatim from `probes/aac-m4a/Mp4Mux.h` (the proven writer).
- `include/M4aEncoder.h`, `src/M4aEncoder.cpp` — the encoder above.
- `tests/m4a_encoder_test.cpp` — see Tests.

**`include/RipFormats.h`** — append `M4a` to the enum (last, stable); append
`{ RipFormat::M4a, "M4A", ".m4a", false, true, "" }` to `kRipFormats` (row 6, key 6);
add to `RipOptions`: `bool aac_vbr = true; int aac_vbr_level = 4; int aac_cbr_bitrate = 128000;`

**`include/EncoderQuality.h`** — add `cycleAacVbr(int level, int dir)` wrapping 1..5;
extend `encoderQualityLabel` with a trailing `int aac_vbr_level = 4` param and a
`case RipFormat::M4a` (`alt_mode ? "<bps/1000> kbps  CBR" : "VBR " + level`); add the
`RipFormat::M4a` axis-hint case (`alt ? bitrate ladder : "quality (VBR 1-5)"`).

**`src/EncoderFactory.cpp`** — include `M4aEncoder.h`; add
`case RipFormat::M4a: return std::make_unique<M4aEncoder>(opt.aac_vbr, opt.aac_vbr_level, opt.aac_cbr_bitrate);`

**`include/Config.h` / `src/Config.cpp`** — rip keys `aac_vbr` / `aac_vbr_level`
(clamp 1-5) / `aac_cbr_bitrate` (clamp 6k-510k); rec keys `rec_aac_vbr` /
`rec_aac_vbr_level` / `rec_aac_cbr_bitrate`; `rec_format` accepts `"m4a"`. Parse +
persist mirroring the existing `mp3_*` / `rec_mp3_*` blocks (Config.cpp:211-226, 352-362).

**`src/CDRipper.cpp`** — `tagFile`: add `is_m4a` + the `else if (is_m4a)` MP4 branch
(F2): tags + AR (`----` freeform) + plain `REPLAYGAIN_*` (`----` freeform) + `covr`.
Add the TagLib MP4 includes.

**`src/ArtEmbed.cpp`** — `embedArt`: add `case RipFormat::M4a` (F1); MP4 headers already
included (ArtEmbed.cpp:14-17).

**`src/UIManager.cpp`** — rip + convert Left/Right switches (5571, 5688) and `[M]`
switches (5588, 5714) gain an `M4a` case (VBR -> `cycleAacVbr`; CBR -> `cycleBitrate`;
`[M]` -> toggle `aac_vbr`). The four draw/hint `alt`+bitrate ternaries (rip 2020/2037,
convert 8172/8183) extend to a 3-way including M4A and pass `aac_vbr_level` to the
label. Rec (only if F6=a): add the M4A row to `rows[]` (2130), grow `rec_focus_` to
0..3 (5428-5430), add the key (F7 decision), extend the rec quality-edit/`[M]` handlers
(5436-5449) and the rec hint mapping (2167), extend `rec_fmt_` init (234), and add
`opt.aac_* = config_.rec_aac_*` to the `^E` RecOptions population (5483).

**`include/StreamRecorder.h` / `src/StreamRecorder.cpp`** (only if F6=a) — `RecOptions`
gains `aac_vbr`/`aac_vbr_level`/`aac_cbr_bitrate`; `start()` validation (94) allows
`M4a`; `makeRecEncoder` (394) adds the `M4a` case; `tagCut` (264) adds an M4A branch
(MP4 tags + per-cut plain RG + `covr`). Add `M4aEncoder.h` + TagLib MP4 includes.

**`CMakeLists.txt` / `tests/CMakeLists.txt`** — `REMOCT_SOURCES += src/M4aEncoder.cpp`;
add `src/M4aEncoder.cpp` to `convert_core_test`, `art_embed_test`, `stream_record_test`
(+ FDK include/lib), `copy_mux_test`, `xfade_handoff_test` (F4); new `m4a_encoder_test`.

---

## Tests (in-slice)

- **`m4a_encoder_test`** (new) — the probe, productized: encode a known PCM fixture via
  `M4aEncoder` (VBR and CBR), open the result through the shipping `AacDecoder`
  (miniaudio), assert correct length + **seek 25/50/75% tracks a frequency-stepped
  signal** (the sample-table gate), and tag/`covr`/RG round-trip incl. the `----`
  ReplayGain read via PropertyMap (F3). Also the **ENOSPC** case: `finalize` returns
  false on a mux-write to a full sink and the caller treats the track as failed. Link
  set mirrors `copy_mux_test` (decode stack + FDK + TagLib).
- **`encoder_quality_test`** (extend) — `cycleAacVbr` wraps 1..5; `encoderQualityLabel`
  renders M4A VBR + CBR; the AAC axis swaps on `[M]`; Config round-trip persists the
  six aac keys and proves **rip and rec AAC are independent storage** (setting rec AAC
  never moves rip AAC).
- **Coverage inherited free (F5):** `convert_core_test` exercises M4A as convert
  source + target; `art_embed_test` covers convert M4A `covr` carryover.
- **Unchanged by non-action:** `rip_encoder_seam_test` + `stream_record_test`'s
  Mp3/Opus byte contracts (M4aEncoder is additive; it touches no existing encoder).

## Gates
- ctest green both toolchains (UCRT64 + WSL2 Debian); fresh-link verify.
- **Joan Osborne AR v2 gate RUNS** — the `RipFormat` table + `makeEncoder` factory are
  touched (even though the CD read/verify path is not). 2-track cancel-at-3 on the
  GHD3N, canonical CRCs a377572a / a0c5b3c2 conf 200.
- Dep-walk shows **no new DLL** (FDK already shipped).
- Dos hardware gate: rip a CD to M4A (both VBR + CBR), convert a file to M4A, play in
  RE-MOCT + external/Apple; rec-to-M4A if F6=a. Per-row editor eyes-on both themes/builds.

## Non-goals (unchanged)
No HE-AAC, no raw `.aac` target, no profile knob (AAC-LC hardcoded); no ALAC/Vorbis
encode; no CD read/verify or `ar_crc.*` change; no new runtime dependency; no reorder
of the `RipFormat` enum (append-only, M4A row 6).

## Open decisions for Dos
1. **F6** — full three-path now (recommended, as briefed) vs rip+convert now / rec-M4A follow-up.
2. **F7** (if F6=a) — rec key numbering: regroup to Opus1/Mp32/M4A3/Copy4 (recommended,
   renumbers Copy) vs keep Copy3 / M4A4.
