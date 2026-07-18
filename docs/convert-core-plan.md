# Convert (single file / flat folder / marked set) - design of record

Scope ID: convert-core. Branch: experimental/win-pdcurses.
Stage: plan (this doc) -> review -> implement -> debrief -> Dos hardware gate -> commit.

Convert existing audio files to another format by reusing the decode path
(playback's ma_decoder) -> f32 PCM -> f32-to-s16 -> the IEncoder seam
(makeEncoder) -> output file, with source tags carried over. Three input
selectors (single file / flat folder / marked set), one shared convert core, one
batch worker modeled on the batch-R128 engine. v1 output is 44100/2 (the forced
decoder rate). Recurse, native-rate, and scale collision policy are explicitly
deferred.

## Anchors re-resolved on the live checkout (all confirmed)

| Anchor | Brief | Live | Status |
|---|---|---|---|
| makeEncoder | CDRipper.cpp:874 | 874 (FLAC/MP3/WAV/Opus/WavPack) | matches |
| IEncoder contract | IEncoder.h:29/34/46 | open/writeFrames(const int16_t*)/finalize | matches |
| open_decoder forces 44100/2/f32 | LocalFileSource.cpp:144-151 | 141-165 (`ma_format_f32, 2, 44100`) | matches |
| ma_decoder read | :174 | prime_decoder + readFrames drain | matches |
| readFrames signature | - | `uint32_t readFrames(float* dst, uint32_t)` (LocalFileSource.h:59) | f32 out |
| f32->s16 util | brief | `ma_pcm_f32_to_s16(dst,src,count,dither)` (miniaudio.h:5689) | present |
| GainScan worker | UIManager.cpp:6213, GainScan.h | poll 1330-1346, trigger 6317, engine drains a private LocalFileSource | matches |
| tagFile | CDRipper.cpp:671 | takes MBRelease/ARTrackResult/RGResult/RipMode | NOT reusable (finding F1) |
| browser entries | - | `std::vector<std::string> dir_entries_` (UIManager.h:200), `current_dir_`, `dir_cursor_` | matches |

### Findings (decide on review)

- **F1 - tagFile is NOT reusable for convert tag carryover.** CDRipper::tagFile
  (671) is built around CD-rip data (MBRelease, ARTrackResult, RGResult,
  RipMode) - it writes rip/AR/RG tags, not a source file's existing tags.
  Convert needs its OWN carryover: read the source's TagLib PropertyMap (the
  uniform surface GainScan already uses: `ref.file()->properties()`) and write
  it to the output's PropertyMap. Text tags (artist/title/album/track/genre/
  date) copy uniformly through PropertyMap. **Cover art does NOT ride
  PropertyMap** - it is per-format embed (FLAC picture block, MP3 APIC, MP4
  covr, Opus METADATA_BLOCK_PICTURE). See F2.

- **F2 - cover-art carryover is per-format and heavier than text tags.** The rip
  path embeds art per-format (CDRipper's "Cover Art (Front)" write, ~:862, plus
  the recorder's tagCut art path). Options: (a) v1 carries TEXT tags via
  PropertyMap uniformly and does best-effort art via a small per-format embed
  helper factored from the existing rip/recorder art writers; (b) v1 carries
  text only, art as a fast-follow. **Recommend (a)** if the existing art-embed
  code lifts cleanly into a shared helper; if it balloons (4 format-specific
  writers + a reader), fall back to (b) and flag - do not half-embed. Confirm on
  review.

- **F3 - "reuse the format+quality modal" = reuse the quality EDITOR, not the
  RipConfirm modal shape.** RipConfirm (drawRipConfirm) is MULTI-select
  (checkboxes) + a rip-mode pick (A/C/Y/B) - convert picks ONE output format and
  no rip mode. The reusable pieces are EncoderQuality.h (labels + cycle/snap) and
  the per-row focus/Left-Right/[M] interaction just shipped. The convert target
  picker is therefore closer to the [Rec] panel's SINGLE-select format + per-row
  quality editor. Propose a dedicated ConvertConfirm overlay that reuses the
  EncoderQuality machinery and the rec-panel single-select+edit pattern - not a
  literal RipConfirm reuse.

- **F4 - keys: x (convert), u (mark), U (clear marks).** Ctrl+V is OUT (Dos
  correction: terminal emulators bind Ctrl+V to paste and consume it before the
  PTY delivers a keycode - the same hazard class ruled it out). Rather than reach
  for a Ctrl code at all, convert uses a **plain letter**, which categorically
  dodges the whole hazard class: emulators intercept Ctrl-combos, never bare
  letter keys, so a letter always arrives as its ASCII code on both ncurses and
  pdcurses (the entire existing keymap is plain-letter getch on both toolchains).
  Full pane-scoped free-key scan of the browser/global handler (case AND
  if(ch==) forms): the only free letters are lowercase `x`, `y`, and uppercase
  `D`, `Y` (`u`/`U` now taken by marks; `m`=mute, `c`/`C`=clear-playlist,
  `v`/`V`+`i`/`I`=info pane, Space=pause). Lowercase `x` appears ONLY in the
  RecPanel handler (`x`=stop, gated behind the rec overlay) and a sub-pane switch
  - never in the browser path - so it is free there. Uppercase `X` stays the
  Devices-pane toggle; `x`!=`X` is the codebase's own per-key split convention
  (e!=E, f!=F, u!=U).
  Proposal: **`x` = convert** (mnemonic eXport/transcode; opens the scope
  chooser), **`u` = mark/unmark focused entry**, **`U` = clear all marks**.
  Keypress-test note: a plain letter needs no Ctrl-hazard test (that class does
  not apply to letters); the definitive in-app press confirmation on both wingui
  and the ncurses/Linux tty rides Dos's eyes-on hardware gate, as all key/modal
  testing does. Confirm the scheme.

- **F5 - the 44100 downsample note (brief's plan question).** open_decoder forces
  44100, so any source resamples to 44.1k on convert. Recommend **convert-with-
  note** generally (blocking lossy re-encodes would be user-hostile), plus an
  explicit warning when the SOURCE native rate > 44100 (read via TagLib
  `audioProperties()->sampleRate()` before decode, independent of the forced
  decoder rate) - so a 96 kHz hi-res FLAC -> lossy shows "source is 96 kHz;
  output will be 44.1 kHz" and proceeds only on confirm. Native-rate convert is a
  future dependency on the deferred bit-perfect slice. Confirm the warn-not-
  refuse policy.

## The convert core (shared by all three selectors)

New engine, modeled 1:1 on GainScan (worker thread, atomics, cancel,
takeFinished, currentFile; UIManager-free so a headless test pins it):

`include/ConvertJob.h` + `src/ConvertJob.cpp`:
```
struct ConvertPair { std::string src, dst; };   // dst pre-resolved by the selector
class ConvertJob {
  // Two entry points, one worker:
  bool startFiles (std::vector<ConvertPair> pairs, RipFormat fmt, RipOptions opt);
  bool startFolder(const std::string& dir, RipFormat fmt, RipOptions opt);  // enumerates flat
  void cancel();
  // atomics: total/index/converted/skipped/errors + finished_ + current_file_
};
```
Per-file core (the shared function `convertOne(src, dst, fmt, opt)`):
1. Guard `src == dst` (canonical-path compare) -> skip-with-note (never overwrite
   the source, e.g. flac->flac in place).
2. Guard `fs::exists(dst)` -> skip-with-note (v1 collision policy; never clobber).
3. Decode: a private `LocalFileSource src; src.open(path)` (the GainScan/BPM twin
   - 44100/2/f32, playback-identical). Drain `readFrames(buf.data(), 4096)` to
   EOF, cancel-checked each block (abandon in-flight cleanly, GainScan precedent).
4. f32 -> s16: `ma_pcm_f32_to_s16(s16buf, f32buf, (uint64)got*2, ma_dither_mode_none)`
   - deterministic, no dither (a plain round/clamp; the CD path feeds native s16,
   so there is no rip dither to match - none is the honest choice).
5. Encode: `auto enc = makeEncoder(fmt, opt); enc->open(dst, est_frames);` then
   `enc->writeFrames(s16buf, got)` per block; `enc->finalize(ok)`. est_frames from
   the source duration if known, else 0 (FLAC STREAMINFO hint; 0 is legal).
6. Tags: carry source PropertyMap -> dst PropertyMap (F1) + best-effort art (F2).
7. On any failure: finalize(false) and remove the partial dst (the IEncoder
   caller contract) -> errors_++.

makeEncoder is `static` in CDRipper.cpp today - expose it (a declaration in a
shared header, e.g. RipFormats.h or a new EncoderFactory.h) so ConvertJob links
it without duplicating the switch. Report: the one-line un-static + header decl.

## Input selectors (UIManager)

- **Single file:** the focused entry `dir_entries_[dir_cursor_]` joined to
  `current_dir_` (reuse the existing entry->path join the play-on-Enter path
  uses). Only if it is a supported audio file. dst = same dir, same basename, new
  extension. -> `startFiles({{src,dst}}, fmt, opt)`.
- **Flat folder:** `startFolder(current_dir_, ...)` enumerates ONE level
  (`fs::directory_iterator`, not recursive - the GainScan enumerate shape),
  supported-audio only, dst = same dir + new ext each. Bounded (user sees the
  folder), so NO count-confirm (that is recurse-only).
- **Marked set:** build pairs from `marked_` (below); each dst = same dir + new
  ext. -> `startFiles(pairs, ...)`. Clear `marked_` on completion.

Supported-input predicate = the decodable set (flac/mp3/wav/aac/m4a/mp4/opus/wv),
mirroring GainScan's `taggableExt` + wav. One shared helper (avoid drift).

## Marked-set state (new)

`include/UIManager.h`: `std::set<std::string> marked_;` (absolute paths -
path-keyed so a mark survives a re-sort, dir refresh, and leaving/re-entering a
dir; recommend marks persist across dir navigation within the session). Factor a
tiny testable wrapper (or free functions) for add/remove/toggle/clear/contains/
list so the membership logic is unit-tested headlessly.
- `u` toggles the focused entry's absolute path in `marked_`.
- `U` clears all marks.
- Browser render (the dir-pane draw): a marked glyph on marked rows + a visible
  marked count (pane header or status line). Confirm glyph choice on review
  (wide-API safe, e.g. a filled marker).

## Keys / overlay flow (F3, F4)

`x` (global handler, a new `case 'x':`) -> scope chooser overlay:
`[1] this file   [2] this folder (N files)   [3] marked set (M files)` (option 3
present only when `marked_` is non-empty) -> the ConvertConfirm modal (single-
select output format + the per-row quality editor reusing EncoderQuality.h) with
the 44.1 kHz note (F5) -> confirm -> `ConvertJob::start*` -> status-line progress
polled ~1 Hz (the ^O pattern: `convert_job_.running()` -> "Converting i/N file")
-> completion toast ("N converted, K skipped, E errors"). Cancellable by pressing
`x` again while running (the ^O cancel precedent). Audio thread untouched -
convert runs while playback continues.

New overlay enum value (UIOverlay::ConvertScope / ConvertConfirm) + a
`ConvertJob convert_job_` member + the tick poll/toast block (mirror gain_scan_
at UIManager.cpp:1330-1346).

## Collision policy (v1, bounded)

dst exists -> skip-with-note. src==dst -> skip-with-note (never clobber the
source). Overwrite/rename deferred to the recurse slice.

## Tests (in-slice; ctest both toolchains)

- `convert_core_test` (headless, the GainScan/stream_record precedent): a small
  fixture per input type (reuse rip test fixtures + gen.sh-style tones) ->
  convert to each output format -> assert the output opens, decodes >0 frames,
  and text tags carried. Links ConvertJob + the encoders + decoders (the
  copy_mux_test link set is the template).
- **f32<->s16 handoff:** WAV -> WAV convert is bit-exact on the audio (s16 ->
  f32 -> s16 with no dither round-trips exactly for in-range samples) - pins the
  conversion step deterministically. (The brief's "== rip output for same source"
  is not literally testable since rip is CD-only/native-s16; this is the
  equivalent invariant.)
- **Non-recursive enumeration:** a fixture dir with a nested subdir asserts the
  subdir's files are NOT in the flat-folder pair list (the flat-only guard).
- **Marked set:** toggle adds/removes; path-keyed marks survive a simulated
  re-sort/refresh; the convert-marked pair list == the marked paths; clear works.
- **src==dst guard** rejects in-place same-format overwrite; **skip-existing**
  leaves the prior dst untouched.
- Register each new target in tests/CMakeLists.txt; fresh-link verify.

## Touch list (no source changed until greenlit)

New: include/ConvertJob.h, src/ConvertJob.cpp, (maybe) include/EncoderFactory.h
(makeEncoder decl), a tag-carryover + art helper, tests/convert_core_test.cpp.
Modified: src/CDRipper.cpp (un-static makeEncoder + decl), src/UIManager.cpp
(`x` convert + scope chooser + ConvertConfirm draw/handler + u/U mark keys +
browser glyph + tick poll), include/UIManager.h (marked_, convert_job_, overlay enum),
CMakeLists.txt (ConvertJob.cpp into the remoct target), tests/CMakeLists.txt,
CHANGELOG.md (at implement time).

## Non-goals (unchanged from brief)

No recursive/tree convert, no count-confirm gate, no scale collision policy (all
deferred to the recurse slice). No native-rate/hi-res convert (44100 output;
depends on the bit-perfect slice). No change to the playback decoder, the audio
thread, or ar_crc.*/the CD read path (convert reads existing files only - Joan
Osborne gate N/A). No new encoder; reuse makeEncoder + the RipOptions quality
knobs as-is.
