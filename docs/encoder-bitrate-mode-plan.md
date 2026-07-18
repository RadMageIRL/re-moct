# Per-encoder bitrate + CBR/VBR (MP3 + Opus) - design of record

Scope ID: encoder-bitrate-mode. Branch: experimental/win-pdcurses.
Stage: plan (this doc) -> review -> implement -> debrief -> Dos hardware gate -> commit.

Per-format, independently editable bitrate/quality and a CBR/VBR mode toggle for
the two lossy encoders that exist (MP3, Opus). FLAC/WAV/WavPack untouched. Rip and
recording keep SEPARATE quality knobs (the old sharing is deliberately retired).

## Anchors re-resolved on the live checkout (all confirmed)

| Anchor | Brief | Live | Status |
|---|---|---|---|
| Config lossy fields | Config.h:47-51 | 47-51 (`mp3`, `opus_bitrate`) | matches |
| Config "reuses" comment | :55 | :53-56 | matches |
| Config load | Config.cpp:199-213 | `opus_bitrate` :199, `mp3` :205-210 | matches |
| Config save | :339-340 | :339-340 | matches |
| drawRipConfirm | UIManager.cpp:1904 | 1904 | matches |
| rip row render | :1992-2007 | 1992-2007 | matches |
| rip key handler | :5440-5462 | 5440-5462, `default: return` at 5462 | matches |
| rec panel rows[] | :2095-2096 | rows[] init 2088-2091, render 2093-2095 | minor drift |
| rec panel handler | :5326-5366 | 5326-5408, `return` catch-all at 5408 | matches |
| rec opt populate | :5386-5395 | 5385-5395 (`RecOptions`) | matches |
| rec radio select | :5348-5351 | 5348-5354 | matches |
| Mp3Encoder open | Mp3Encoder.cpp:14-27 | open 16-38, Xing rewrite 63-70 | matches |
| Mp3Encoder ctor | Mp3Encoder.h:19 | 19 (`explicit Mp3Encoder(int vbr_q=0)`) | matches |
| OpusRipEncoder ctl | OpusRipEncoder.cpp:42 | 42 (`OPUS_SET_BITRATE`) | matches |
| OpusRipEncoder ctor | OpusRipEncoder.h:34 | 34 (`explicit OpusRipEncoder(int bitrate=...)`) | matches |
| rip factory | CDRipper.cpp:877-879 | 874-883 `makeEncoder` | matches |
| RipOptions | RipFormats.h:47 | 47-53 | matches |

### Findings (divergences / brief gaps - decide on review)

- **F1 - a SECOND encoder factory exists.** The brief names only
  `makeEncoder` in CDRipper.cpp:874. The recording path has its own parallel
  factory at **StreamRecorder.cpp:395-396**, building the same
  `Mp3Encoder(opt.mp3_vbr_q)` / `OpusRipEncoder(opt.opus_bitrate)` from
  `RecOptions`. Both must thread the new ctor args. One extra file in the touch
  list; no design impact.

- **F2 - Mp3Encoder ctor argument order (recommend NOT the brief's order).**
  The brief suggests `Mp3Encoder(bool cbr, int vbr_q, int cbr_bitrate_bps)`.
  The seam oracle constructs `Mp3Encoder mp3;` (test :172) and
  `Mp3Encoder mp3(0);` (test :221, the explicit-default arm). Keeping **vbr_q
  first** - `Mp3Encoder(int vbr_q=0, bool cbr=false, int cbr_bitrate_bps=256000)`
  - leaves both existing constructions literally valid and VBR-V0, so the
  byte-identity arms need zero change and the "explicit V0 default" intent is
  preserved. The brief's order makes `Mp3Encoder(0)` mean "cbr=false via int->bool
  coercion, vbr_q defaulted" - byte-identical only by accident and fragile.
  **Recommendation: vbr_q-first.** Flag for Dos.

- **F3 - rec default change is a behavior change.** Today recording reads
  `config_.opus_bitrate` (128000) and `config_.mp3` (V0). Right-sizing rec
  defaults to `rec_opus_bitrate=96000` / `rec_mp3="V5"` changes what a default
  install records. Intended per brief, but it IS a behavior shift - confirm the
  exact defaults on review.

- **F4 - Opus modal editing snaps to a fixed list.** `config_.opus_bitrate` is a
  free 6k-510k value (default 128000). The modal Left/Right cycles the fixed set
  {96, 128, 256, 320} kbps. Design: config-file load still accepts any value
  (clamp unchanged); the modal cycle snaps to the nearest list entry on first
  press, then wraps within the list. A config with e.g. 200 kbps therefore jumps
  to a list value the moment the user edits it in the modal. Confirm acceptable.

- **F5 - OPUS_SET_VBR via ope_encoder_ctl needs a build-time header check.**
  libopusenc's `ope_encoder_ctl` forwards the `OPUS_SET_*` request macros from
  opus.h; `OPUS_SET_VBR(x)` is the expected form. I cannot compile - **implement
  gate: confirm the installed libopusenc exposes it via `ope_encoder_ctl`; STOP
  and report if it differs** (e.g. an `OPE_SET_*` variant).

- **F6 - persistence (brief asks to confirm).** Recommend the new rip and rec
  fields persist to config exactly like `mp3`/`opus_bitrate` do today, rip and
  rec sets independent. This matches the existing "session seed from config"
  model. Confirm.

## Codec truth (settled - not re-litigated)

- MP3 VBR: quality is the V-scale V0-V9. MP3 CBR: quality is a bitrate from
  {96,128,256,320}. The Left/Right axis swaps with [M].
- Opus: quality is ALWAYS a bitrate from {96,128,256,320}; no V# axis exists in
  libopus. [M] flips only OPUS_SET_VBR(1/0); Left/Right stays bitrate.

## Config schema (include/Config.h + src/Config.cpp)

Rip set (additive; `mp3` and `opus_bitrate` stay):
```
bool mp3_cbr           = false;    // false = VBR (V-scale), true = CBR (bitrate)
int  mp3_cbr_bitrate   = 256000;   // MP3 CBR bitrate, snapped to {96,128,256,320}k
bool opus_vbr          = true;     // Opus mode: true = VBR (default), false = CBR
```
Rec set (its OWN parallel storage - the sharing is retired):
```
std::string rec_mp3          = "V5";     // rec MP3 V-scale  (default right-sized)
bool        rec_mp3_cbr      = false;
int         rec_mp3_cbr_bitrate = 256000;
int         rec_opus_bitrate = 96000;    // rec Opus bitrate (default right-sized)
bool        rec_opus_vbr     = true;
```
- Update the Config.h:53-56 comment: the "Quality reuses opus_bitrate / mp3 (one
  quality truth, no duplicate knobs)" invariant is INTENTIONALLY retired - rec now
  carries its own quality set so radio can be right-sized independently of CD rip.
- load (Config.cpp ~:199-219): 8 new keys, mirroring the existing `mp3` (V0-V9
  validation) and `opus_bitrate` (clamp) parsers. `*_cbr`/`*_vbr` are "1"/"0".
  `*_cbr_bitrate`/`rec_opus_bitrate` clamp to the allowed set (or 6k-510k like
  opus_bitrate, snapping happens in the modal - keep load permissive).
- save (Config.cpp ~:339-346): 8 new lines alongside `mp3=`/`opus_bitrate=`.
- Remember-both is FREE: each MP3 side keeps its V-level (`mp3`/`rec_mp3`) and its
  CBR bitrate (`mp3_cbr_bitrate`/`rec_mp3_cbr_bitrate`) in separate fields, so [M]
  only flips the `*_cbr` bool and never loses the other axis. No extra state.

## Encoders

### Mp3Encoder (include/Mp3Encoder.h, src/Mp3Encoder.cpp) - CBR is strictly additive

- Ctor (F2): `explicit Mp3Encoder(int vbr_q = 0, bool cbr = false,
  int cbr_bitrate_bps = 256000)`. Members gain `bool cbr_`, `int cbr_bitrate_bps_`.
- `open()`: split on `cbr_`.
  - VBR (`!cbr_`): emit the EXACT existing sequence unchanged -
    `lame_set_VBR(vbr_mtrh)` + `lame_set_VBR_q(vbr_q_)` + `lame_set_bWriteVbrTag(1)`,
    and the finalize Xing rewrite. Byte-identical, the seam contract.
  - CBR (`cbr_`): `lame_set_VBR(lame_, vbr_off)` + `lame_set_brate(lame_,
    cbr_bitrate_bps_ / 1000)`; DO NOT call `lame_set_bWriteVbrTag(1)` (no Xing/Info
    frame for CBR). `lame_set_quality(2)`, channels, samplerate, `STEREO`,
    id3-off stay identical.
- `finalize()`: gate the Xing rewrite (:63-70) on `!cbr_` - CBR has no reserved
  first frame to overwrite. Flush path otherwise unchanged.

### OpusRipEncoder (include/OpusRipEncoder.h, src/OpusRipEncoder.cpp)

- Ctor: `explicit OpusRipEncoder(int bitrate = kOpusDefaultBitrate,
  bool vbr = true)`. Member gains `bool vbr_`.
- `open()`: after `ope_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_))` (:42) add
  `ope_encoder_ctl(enc_, OPUS_SET_VBR(vbr_ ? 1 : 0))`. `vbr=true` is libopusenc's
  default, so existing output is unchanged. (F5 build-gate on the macro.)

## Construction sites (thread the new fields)

- RipOptions (include/RipFormats.h:47-53) gains `bool mp3_cbr=false;
  int mp3_cbr_bitrate=256000; bool opus_vbr=true;`.
- RecOptions (include/StreamRecorder.h:51+) gains the same three.
- makeEncoder (CDRipper.cpp:877/879):
  `Mp3Encoder(opt.mp3_vbr_q, opt.mp3_cbr, opt.mp3_cbr_bitrate)`,
  `OpusRipEncoder(opt.opus_bitrate, opt.opus_vbr)`.
- Rec factory (StreamRecorder.cpp:395/396): same ctor calls from `opt` (F1).
- Rip opt populate (UIManager.cpp:5491-5493): add the three from the RIP config
  fields (`config_.mp3_cbr`, `config_.mp3_cbr_bitrate`, `config_.opus_vbr`).
- Rec opt populate (UIManager.cpp:5387-5388): read the REC fields, not rip -
  `opt.mp3_vbr_q = parseMp3VbrQ(config_.rec_mp3)`,
  `opt.mp3_cbr = config_.rec_mp3_cbr`, `opt.mp3_cbr_bitrate =
  config_.rec_mp3_cbr_bitrate`, `opt.opus_bitrate = clamp(config_.rec_opus_bitrate)`,
  `opt.opus_vbr = config_.rec_opus_vbr`.

## Shared display: one label home, per-panel values

New header **include/EncoderQuality.h** - pure functions, no panel/config type:
```
// mode+value -> display string; the ONE home so "V0  VBR" / "256 kbps  CBR"
// formatting cannot drift between the two panels.
std::string encoderQualityLabel(RipFormat f, bool alt_mode,
                                const std::string& mp3_v, int bitrate_bps);
//  Flac    -> "level N"      Wav -> "16-bit PCM"     WavPack -> mode word
//  Mp3     -> alt_mode(cbr)? "<kbps> kbps  CBR" : "<mp3_v>  VBR"
//  Opus    -> "<kbps> kbps  " + (alt_mode(=!vbr)? "CBR" : "VBR")
```
Plus the axis-cycle helpers (also pure, unit-testable, shared by UI + test):
```
int         cycleBitrate(int bps, int dir);       // wraps {96,128,256,320}k
std::string cycleMp3Vbr(const std::string& v, int dir);  // wraps V0..V9
int         snapBitrate(int bps);                 // nearest list entry (F4)
```
Render sites both call `encoderQualityLabel` with THEIR OWN fields:
- RipConfirm (UIManager.cpp:1996-2000) - fed the RIP config fields.
- Rec panel rows[] (UIManager.cpp:2088-2095) - fed the REC config fields (Opus/Mp3
  rows only; the Copy row keeps its live-caps string at 2100-2108, untouched).
The label is one home; the VALUES it is handed differ per panel - the shared
formatter must never reintroduce shared values.

## Modal interaction (both modals get per-row editing)

New UIManager members (include/UIManager.h): `int rip_focus_ = 0;`,
`int rec_focus_ = 0;` - the `>` row cursor, session state seeded to 0.

Verified free in both handlers: KEY_UP/KEY_DOWN/KEY_LEFT/KEY_RIGHT and 'm'/'M'
currently hit the rip modal's `default: return` (5462) and the rec panel's
`return; // swallow everything else` (5408) - inert today, no behavior to
preserve. [B] (Local 2-pass), digits, S/T/A/D, and the rip a/c/y/b keys are
untouched.

- RipConfirm handler (before 5462): KEY_UP/DOWN move `rip_focus_` (clamp
  0..kRipFormatCount-1); KEY_LEFT/RIGHT cycle the focused row's active axis
  (Mp3: `mp3_cbr` ? `mp3_cbr_bitrate` via cycleBitrate : `mp3` via cycleMp3Vbr;
  Opus: `opus_bitrate` via cycleBitrate; Flac/Wav/WavPack inert); 'm'/'M' toggles
  the focused row's mode (Mp3 -> `mp3_cbr`, Opus -> `opus_vbr`; else inert).
- RecPanel handler (before 5408): same over the 3 rows (Opus, Mp3, Copy), editing
  the REC fields; Copy row inert (no axis), like Flac/Wav in rip.

Render: replace the leading "  " on the focused row with "> "; fold mode into the
quality string via the formatter; add ONE hint line in the existing blank row
between the format table and the mode block (rip: row `7+kRipFormatCount`; rec: the
blank line above the settings keys) naming the active axis, e.g.
`<-/-> quality (V0-V9)   [M] mode`. Width: "256 kbps  CBR" = 13 cols fits the
existing `%-14s` field; the "> " marker is width-neutral. No box geometry change
beyond what the hint line consumes (both panels have a spare row already).

## Tests (in-slice; ctest both toolchains)

- **rip_encoder_seam_test.cpp** - VBR arms UNCHANGED (byte-identity is the
  contract). ADD a CBR arm: `Mp3Encoder(0, true, 256000)`, drive the fixture,
  assert (a) it encodes and size>0, (b) the VBR `ref==seam==exp` still hold, (c)
  structural: no Xing/Info tag at the stream head (CBR drops bWriteVbrTag) - assert
  the first frame is an audio frame sync, not an "Info"/"Xing" magic.
- **Opus mode test** (new small target, e.g. `encoder_quality_test` or fold into
  an opus test): construct `OpusRipEncoder(96000, false)` and the default
  `(128000, true)`; assert both encode a valid OggS stream; VBR default path
  behaviorally unchanged. Guards F5 at runtime.
- **Config round-trip independence** (new target): save+reload preserves all 8 new
  fields and clamps; set `rec_opus_bitrate=96000` and assert `opus_bitrate`
  unchanged (and vice versa) - the guard on the deliberately-broken sharing.
- **Axis-cycle unit** (pure, over EncoderQuality.h): cycleBitrate wraps both
  directions through {96,128,256,320}k; cycleMp3Vbr wraps V0..V9; snapBitrate picks
  nearest; [M] preserves both remembered values (flip VBR->CBR->VBR is lossless).
  Covers both focus models since the state machine is shared.
- Register each new target in tests/CMakeLists.txt (mirror rip_encoder_seam_test's
  block); fresh-link verify the touched test binaries before trusting a pass.

## Gates

- ctest both toolchains (Win UCRT64 + WSL2 Debian ~/rb) green.
- **Joan Osborne AR v2 2-track gate APPLIES**: the RipOptions struct AND the
  CDRipper `makeEncoder` factory are both touched, so the CD rip path shifts (even
  though the encoder swap is downstream of the verified read). Run the trimmed CD
  gate (2-track [A], full TOC, cancel-at-3; canonical Relish CRCs a377572a/a0c5b3c2
  conf 200) + byte-identity vs the smoke baselines.
- Dos hardware gate: rip a disc with MP3 forced CBR 320 and Opus CBR 96, confirm
  the files decode and tag right in RE-MOCT + an external player; record a station
  at rec Opus 96 VBR and MP3 CBR, confirm the rec files honor the REC knobs and the
  rip knobs are unchanged. Eyes-on both modals in both themes and both builds
  (wingui PDCurses + ncurses): focus cursor, Left/Right axis, [M] relabel.

## Touch list (no source changed until greenlit)

include/Config.h, src/Config.cpp, include/RipFormats.h, include/StreamRecorder.h,
include/Mp3Encoder.h, src/Mp3Encoder.cpp, include/OpusRipEncoder.h,
src/OpusRipEncoder.cpp, src/CDRipper.cpp, src/StreamRecorder.cpp,
include/EncoderQuality.h (new), include/UIManager.h, src/UIManager.cpp,
tests/rip_encoder_seam_test.cpp, tests/CMakeLists.txt + new test TUs.

## Non-goals (unchanged from brief)

No Vorbis (no encoder). No invented Opus V# axis. No FLAC/WAV/WavPack change.
No touch to [B], digit toggles, or rip_sel_. MP3 VBR path stays byte-identical.
No CD read/verify or ar_crc.* change. CHANGELOG entry (Fixed/Added, hyphens only)
added at implement time, not now.
