# Slice `rip-opus-encoder` — Opus rip output (PLAN)

**Stage:** PLAN. No source written, nothing committed.
**Baseline:** `experimental/win-pdcurses` at `f5ea409` (post rip-wav-encoder).
**Position:** slice 4 — first library encoder, first lossy new output, first
real `tagFile` edit.

**§0 cover-art call: taken IN-SCOPE, and it is near-free** — TagLib's
`Ogg::XiphComment` has native picture support (`addPicture(FLAC::Picture*)`,
verified in the installed 2.2.1 headers), so the Opus branch reuses the FLAC
branch's `FLAC::Picture` construction verbatim and attaches it to the comment
instead of the file. No base64/`METADATA_BLOCK_PICTURE` hand-rolling.

---

## 1. Probe results (the §3 discovery, all run on the live toolchain)

### 1.1 THE RESAMPLING QUESTION — answered empirically: libopusenc resamples internally

A compiled probe fed **88,200 frames of 44.1 kHz s16** through
`ope_encoder_create_file(..., 44100, 2, 0, ...)` and decoded the result with
opusfile: **exactly 96,000 frames at 48 kHz — 2.000 s, sample-exact**. No
self-resampling, no miniaudio resampler, no added shape. The encoder is
minimal: create at 44100, write s16, drain. (The header doc's "48 kHz is
faster" is about skipping the internal resampler, not a requirement.)
The §7 duration audition remains as the integration catch.

### 1.2 API confirmations

- `ope_encoder_write(enc, const opus_int16* pcm, int samples_per_channel)` —
  interleaved s16, exactly the seam's type. Returns `OPE_OK`/negative —
  strict `writeFrames` maps directly (the WAV fold-in contract).
- `OPUS_SET_BITRATE` / `OPUS_SET_VBR` via `ope_encoder_ctl` (probe-verified,
  returns 0), `OPE_SET_HEADER_GAIN(0)` exists and works (probe-verified) —
  header gain pinned to 0 so the R128 tags are the only gain, matching what
  the decode-side OpusDecoder expects (RFC 7845 convention, both directions).
- **Wide paths:** `ope_encoder_create_file` takes `const char*` (ANSI fopen on
  Windows — the Unicode trap). **`ope_encoder_create_callbacks` exists**
  (`write`/`close` funcs over `void* user_data`): the encoder opens with
  `port::fopenUtf8` and hands libopusenc fwrite-shaped callbacks — the same
  own-the-FILE* move as WavEncoder, no path ever crosses the library.
- `opusenc.h` includes `<opus.h>` **unprefixed** (the opusfile family quirk) —
  served by the `-I<prefix>/include/opus` entry the decode slice already
  added; probe-confirmed compile. **No taglib/opusenc.h exists** — no
  collision, no grep-guard extension needed.
- Deps: MSYS2 `libopusenc 0.3-1` **already installed** (with `libopusenc.a`
  for the static probe); Debian candidate `libopusenc-dev 0.2.1-2+b2`.

### 1.3 Divergence: the bundle script named in §8 does not exist

`Collect-RemoctCodecDeps.ps1` is nowhere in the tree (recursive search;
`tools/` holds only `README.md` + `src`). Nothing to update — the release
note becomes: **dynamic Windows bundle gains `libopusenc-0.dll`** (opus/ogg
already bundled), recorded in the debrief/notices as with prior slices. If
the script lives outside the repo, the regex to add on your side is
`^libopusenc-\d+\.dll$`.

## 2. Resolved anchors

| what | where |
|---|---|
| tagFile FLAC Xiph branch (the template) | [src/CDRipper.cpp:722-750](src/CDRipper.cpp#L722-L750) |
| tagFile dispatch booleans | [:661-662](src/CDRipper.cpp#L661-L662) (`is_mp3`/`is_flac` — Opus adds `is_opus`, and the dispatch should key on extension exactly as siblings do) |
| RG formatter lambdas (shared) | [:677-684](src/CDRipper.cpp#L677-L684) |
| Decode-side R128 conversion (to be factored) | [src/LocalFileSource.cpp:63-77](src/LocalFileSource.cpp#L63-L77) region — `q78 / 256.0f + 5.0f` |
| Factory switch | [src/CDRipper.cpp:760-767](src/CDRipper.cpp#L760-L767) |
| Row table | [include/RipFormats.h](include/RipFormats.h) — append `{ Opus, "Opus", ".opus", false, true, "" }`, digit 4 |
| Tag-pass guard (taggable=true → no change) | [src/CDRipper.cpp:1929](src/CDRipper.cpp#L1929) region |
| Modal quality text switch | [src/UIManager.cpp:1755-1760](src/UIManager.cpp#L1755-L1760) region |
| Config load/save | `src/Config.cpp` (flac_level/mp3 pattern, slice 2) |
| RipOptions | [include/RipFormats.h](include/RipFormats.h) — gains `opus_bitrate` |
| CMake opus block | [CMakeLists.txt:196-215](CMakeLists.txt#L196-L215) — `find_library(OPUSENC_LIB opusenc ...)`; link **opusenc before opus** |
| CI package lists | `.github/workflows/ci.yml` — `+libopusenc-dev` (Debian), `+mingw-w64-ucrt-x86_64-libopusenc` (Windows) |
| Notices | `THIRD-PARTY-NOTICES.md` — +libopusenc (BSD-3-Clause, Xiph) |

## 3. Design

### 3.1 `OpusEncoder` (`include/OpusEncoder.h`* + `src/OpusEncoder.cpp`)

- **open(path, total_frames):** `port::fopenUtf8` → `ope_comments_create()`
  (ENCODER comment only — real tags come later via tagFile, the slice-1
  album-gain reason) → `ope_encoder_create_callbacks` over the FILE* →
  `ctl(OPUS_SET_BITRATE(bitrate_))`, VBR default, `ctl(OPE_SET_HEADER_GAIN(0))`.
  `total_frames` unused (no header estimate concept) — `(void)` like Mp3Encoder.
- **writeFrames:** `ope_encoder_write(enc_, interleaved, (int)frames)` —
  strict: `!= OPE_OK` returns false.
- **finalize(ok):** `ope_encoder_drain` when ok, `ope_encoder_destroy`,
  `ope_comments_destroy`, `fclose`. Partial removal stays the caller's job.
- **Ctor:** `OpusEncoder(int bitrate = kOpusDefaultBitrate)` with
  `inline constexpr int kOpusDefaultBitrate = 128000;` in the header — the
  named-constant pin the brief asks for (no byte-oracle, so the default is
  guarded by visibility + the config default referencing the same constant).

*Naming note: the decode slice already flagged that libopus typedefs a C
`OpusDecoder`; symmetrically there is a C `OpusEncoder` type in `opus.h`.
Our class `OpusEncoder` in the global namespace would collide **if a TU saw
both** — and OpusEncoder.cpp includes both `opusenc.h` and our header.
**Decision: name the class `OpusRipEncoder` (file `OpusRipEncoder.{h,cpp}`)**
— the one encoder that can't share its format's bare name; a comment explains
why. (FlacEncoder/Mp3Encoder/WavEncoder had no such clash.)

### 3.2 The tagFile Opus branch (the centerpiece)

Additive `else if (is_opus)` after the FLAC branch, keyed
`path.ends_with(".opus")` like its siblings:

- `TagLib::Ogg::Opus::File f(wp.c_str(), false)` → `f.tag()` (XiphComment).
- Standard fields + ENCODER + AR fields: **verbatim the FLAC branch's calls**
  (same XiphComment API).
- Art: same `FLAC::Picture` construction, attached via `tag->addPicture(pic)`.
- **RG dialect — R128 only:**
  ```cpp
  tag->addField("R128_TRACK_GAIN", std::to_string(r128FromDb(rg.track_gain)), true);
  tag->addField("R128_ALBUM_GAIN", std::to_string(r128FromDb(rg.album_gain)), true);
  ```
  No `REPLAYGAIN_*` keys, no peak keys (R128 defines none — peaks are not
  part of the Opus dialect).
- **Byte-identity for FLAC/MP3 is structural**, same argument as the WAV
  skip: the new branch is an `else if` their control flow never enters; the
  `is_mp3`/`is_flac` branches' bodies are untouched. `tagFile`'s diff is
  additions only — provable by inspection of the diff hunk.

### 3.3 The shared R128↔RG conversion — `include/R128Gain.h` (new)

```cpp
// R128 <-> ReplayGain conversion (RFC 7845 Q7.8, -23 vs -18 LUFS references).
// BOTH directions live here and nowhere else - encode (CDRipper tagFile) and
// decode (LocalFileSource RG read) cannot drift. Round-trip-tested.
inline float dbFromR128(int q78)   { return (float)q78 / 256.0f + 5.0f; }
inline int   r128FromDb(double db) { return (int)std::lround((db - 5.0) * 256.0); }
```

`LocalFileSource.cpp`'s RG branch swaps its inline `q78 / 256.0f + 5.0f` for
`dbFromR128(...)` — a pure refactor of the decode direction (same math, same
floats; the decode-slice RG checks re-run to prove it). Round-trip exactness:
`/256` and `±5` are exact in binary floating point for the Q7.8 integer range,
so `r128FromDb(dbFromR128(q)) == q` holds for every representable tag value —
asserted over the full range in the §5.1 test, not argued.

### 3.4 Row / config / cue-master

- Row: `{ Opus, "Opus", ".opus", false, true, "" }` — lossy (no `*`), tagged
  (no note), digit 4 append-only. `rip_formats` token `opus` free via the
  table-driven parse.
- Config: `opus_bitrate` (int, default `128000`, clamped 6000-510000 — the
  libopus legal range) → `RipOptions.opus_bitrate` → ctor. Modal quality text:
  `"%d kbps VBR"` from the config value (`128 kbps VBR` default).
- Cue/M3U8: Opus-only → no lossless selected → "else first selected" → `.opus`
  referenced — intended behavior for a lossy-only rip. The rip log already
  distinguishes nothing here; the lossless-vs-derived log marking stays the
  recorded later item (§10 of the brief). FLAC+Opus → FLAC referenced,
  unchanged.

## 4. Hazard decisions

| hazard | decision |
|---|---|
| Resampling | **internal to libopusenc, probe-proven sample-exact**; duration audition still runs |
| R128 inverse exactness | one shared header, both directions; full-range round-trip test (machine-proved, not argued) |
| tagFile FLAC/MP3 identity | additive `else if`; bodies untouched; §5.6 re-verifies tags byte-for-byte |
| No byte-oracle | accepted per brief; named-constant bitrate default; round-trip + audition + loudness parity instead |
| Lossy cue master | `.opus` referenced only when no lossless selected — correct; log marking deferred as recorded |
| Wide paths | `ope_encoder_create_callbacks` over `port::fopenUtf8` (§1.2) — no `char*` path crosses the library |
| Class-name clash with libopus's C `OpusEncoder` | class named `OpusRipEncoder` (§3.1) |
| Header gain | pinned 0 explicitly (probe-verified ctl), so R128 tags are the only gain — decode-side convention preserved |

## 5. Verification to PROPOSE (on greenlight)

1. **R128 round-trip unit test**: `r128FromDb(dbFromR128(q)) == q` for the
   full Q7.8 int range, plus spot values (−2560 → −5.00 dB → −2560, the
   decode-flight vector). Both toolchains.
2. **Decode-audition**: rip Opus, decode via the existing OpusDecoder
   (probe harness): duration == TOC duration (the resample catch), real RMS
   envelope, clean seek/tail.
3. **Loudness parity**: decoded Opus RMS within ~1 dB of the same rip's FLAC.
4. **R128 read-back (end-to-end dialect proof)**: the ripped Opus's
   `R128_TRACK_GAIN` through `LocalFileSource`'s RG path yields
   `replaygain_db` equal (±0.01) to the FLAC's `REPLAYGAIN_TRACK_GAIN` value
   — the two dialects land at the same playback gain.
5. **Default case**: seam oracle green; FLAC+MP3 outputs and tags
   byte-identical with the Opus branch present (full-file SHA of tagged
   FLAC/MP3 from a 1-track rip pre/post-slice — tagFile is the one edited
   function, so this is the direct check).
6. **Trimmed CD gate**: 2-track [A] on the GHD3N with FLAC+MP3+Opus
   (full-TOC/cancel-at-3) — AR v2 canonical CRCs unchanged.
7. **Subset checks**: Opus-only → only `.opus`, R128 tags present and NO
   `REPLAYGAIN_*` keys (tag dump), cue → `.opus`; FLAC+Opus → cue → `.flac`,
   each file in its own dialect.
8. ctest both toolchains; CHANGELOG user-facing entry (hyphens only);
   Option C `ldd` still 2-DLL (static `libopusenc.a` folds in).

## 6. Change inventory (on greenlight)

| file | change |
|---|---|
| `include/OpusRipEncoder.h` / `src/OpusRipEncoder.cpp` | **new** |
| `include/R128Gain.h` | **new** — shared conversion, both directions |
| `src/LocalFileSource.cpp` | decode direction → `dbFromR128` (pure refactor) |
| `src/CDRipper.cpp` | +factory case; +tagFile Opus branch (additive) |
| `include/RipFormats.h` | +row; `RipOptions.opus_bitrate` |
| `src/UIManager.cpp` | +quality-text case; `opus_bitrate` → RipOptions |
| `include/Config.h` / `src/Config.cpp` | `opus_bitrate` key |
| `CMakeLists.txt` / `tests/CMakeLists.txt` | +OPUSENC_LIB find/link; +sources |
| `.github/workflows/ci.yml` | +1 package per job |
| `THIRD-PARTY-NOTICES.md` | +libopusenc |
| `tests/rip_encoder_seam_test.cpp` (or sibling) | +R128 round-trip test |
| `CHANGELOG.md`, `docs/lessons.md` | entries |

No golden-file oracle (decision per §0.2). `ar_crc.*`/read/verify untouched.
