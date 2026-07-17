# Slice `decode-ogg-vorbis` — Ogg Vorbis decode + Opus f32 backport (PLAN)

**Stage:** PLAN. No source written, nothing committed.
**Baseline:** `experimental/win-pdcurses` at `ebfaf1c` (decode-opus-wv merged).
**Target on greenlight:** same branch.

All decode-opus-wv infrastructure this slice depends on is confirmed present at
the anchors in §3. Two of the brief's technical premises are wrong in ways that
make the work *smaller* — read §1 first.

---

## 1. Divergence from the brief (READ FIRST)

### 1.1 `op_read_float` is INTERLEAVED, not planar — §6 gets simpler

The brief (§6) says `op_read_float` is "also planar — reuse the exact interleave
loop". The header says otherwise:

```c
OP_WARN_UNUSED_RESULT int op_read_float(OggOpusFile *_of,
 float *_pcm,int _buf_size,int *_li) OP_ARG_NONNULL(1);
```

One flat `float*` buffer, interleaved — the float twin of `op_read`, same shape.
Only Vorbis's `ov_read_float` is planar (`float ***pcm_channels`). Consequences:

- **There is no shared interleave loop.** The loop exists once, in
  VorbisDecoder only.
- **The Opus backport shrinks to a three-line retrofit:** `opus_int16* dst` →
  `float* dst`, `op_read` → `op_read_float` (buffer-size semantics identical:
  a value count in an int), and `ma_format_s16` → `ma_format_f32` in
  `get_format`. Plus the header doc-comment. Nothing else moves.

### 1.2 No include-dir addition is needed for Vorbis — §7.3(b) refuted

The brief expects `vorbisfile.h` to include its siblings unprefixed like
`opusfile.h` did, forcing `.../include/vorbis` onto the path ("plan it as a
step, not a discovery"). It doesn't:

- `vorbisfile.h` → `#include "codec.h"` — **quoted**, resolves relative to its
  own directory with no `-I` help.
- `codec.h` → `#include <ogg/ogg.h>` — prefixed from the root, already served.

Proven by compile probe on the live MSYS2 toolchain: a TU with
`#include <vorbis/vorbisfile.h>` compiles with **only the include root** on the
path. So:

- **No `.../include/vorbis` dir is added. Nothing to order before TagLib.**
  Recommend dropping §7.3(b) entirely rather than adding a dead `-I` — an
  unneeded include dir is exactly the "-I order as a guard" reliance the last
  slice demoted. The CI grep gate (live since `ebfaf1c`, `vorbisfile` already
  in its pattern) is the guard, alone and sufficient. A bare `<vorbisfile.h>`
  would resolve to TagLib's C++ header and fail at first `ov_*` use anyway —
  but lint fails it before any compile.
- What gets banked in lessons.md is this *fact* (quoted-include layout, probe
  result), the vorbis analog of the opus `-I`-order entry.

### 1.3 Buffer-lifetime hazard (§7.2): real, and already the house rule

`ov_open_callbacks` reads lazily for the life of the `OggVorbis_File` — but so
does `op_open_memory` ("the data buffer must remain valid until `op_free`"),
and the merged OpusDecoder already pins `b->file` in the backend struct for
exactly that reason ([src/OpusDecoder.cpp:41-43](src/OpusDecoder.cpp#L41-L43)).
VorbisDecoder inherits the identical ownership shape (§5): the slurped
`std::vector<uint8_t>` lives in the backend, is never resized after open, and
`be_uninit` tears down the decoder (`ov_clear`) **before** the struct (and
buffer) are deleted. The hazard is confirmed and the discipline for it is
already in the tree; the §8 seek + near-EOF test exercises it.

### 1.4 Minor stale-comment note

`AacDecoder.h:10` still says custom backends defer to "the built-in FLAC / MP3
/ WAV / **Vorbis** decoders" — no built-in Vorbis ever existed in this build
(opus plan §1.2), and after this slice Vorbis is a *custom* backend. One-line
comment fix, folded into this slice.

---

## 2. Files to add / edit

| file | change |
|---|---|
| `include/VorbisDecoder.h` | **new** — declares `ma_vorbis_backend_vtable()` |
| `src/VorbisDecoder.cpp` | **new** — libvorbisfile backend (§5) |
| `src/OpusDecoder.cpp` | f32 retrofit (§6) |
| `include/OpusDecoder.h` | doc-comment: s16 → f32 |
| `include/AacDecoder.h` | stale "built-in Vorbis" comment (§1.4) |
| `src/CustomBackends.cpp` | array grows to 4 |
| `include/CustomBackends.h` | comment: name the fourth codec |
| `CMakeLists.txt` | vorbis find block + sources + link |
| `tests/CMakeLists.txt` | `xfade_handoff_test` +1 source, +2 libs |
| `.github/workflows/ci.yml` | +1 package per job |
| `THIRD-PARTY-NOTICES.md` | +libvorbis/libvorbisfile (BSD-3-Clause) |
| `docs/lessons.md` | quoted-include fact (§1.2) |
| `CHANGELOG.md` | extend `[1.3.0] - UNRELEASED` (§9) |

No UI edits: `.ogg` in `AUDIO_EXTS` and `"OGG"` in `fileTypeTag` predate the
opus slice, confirmed still present. No RG edit (§5, last bullet). No CD-path
file → Joan Osborne gate not invoked.

---

## 3. Resolved live-tree anchors (at `ebfaf1c`)

| what | file:line |
|---|---|
| Backend array to extend | [src/CustomBackends.cpp:15-17](src/CustomBackends.cpp#L15-L17) — append `ma_vorbis_backend_vtable(),` |
| Opus read loop to retrofit | [src/OpusDecoder.cpp:54](src/OpusDecoder.cpp#L54) (`opus_int16* dst`), [:59-60](src/OpusDecoder.cpp#L59-L60) (`cap` + `op_read`) |
| Opus format report | [src/OpusDecoder.cpp:81](src/OpusDecoder.cpp#L81) (`ma_format_s16`) |
| Opus header doc to update | [include/OpusDecoder.h:12-17](include/OpusDecoder.h#L12-L17) ("signed-16 PCM") |
| Template for VorbisDecoder | `src/OpusDecoder.cpp` entire (slurp/sniff/vtable shape) |
| CMake insert point | after the wavpack block, [CMakeLists.txt:218-222](CMakeLists.txt#L218-L222) |
| CMake sources list | [CMakeLists.txt:66-69](CMakeLists.txt#L66-L69) (decoder entries) |
| CMake link list | [CMakeLists.txt:294-298](CMakeLists.txt#L294-L298) — vorbis libs go between `${OPUS_LIB}` and `${OGG_LIB}` (§7) |
| CMake include call | [CMakeLists.txt:228-229](CMakeLists.txt#L228-L229) — gains `${VORBIS_INCLUDE}` (root only, §1.2) |
| xfade test sources | [tests/CMakeLists.txt:324-326](tests/CMakeLists.txt#L324-L326) |
| xfade test libs | [tests/CMakeLists.txt:337-339](tests/CMakeLists.txt#L337-L339) — vorbis libs before `${OGG_LIB}` |
| CI Debian packages | [.github/workflows/ci.yml:39-43](.github/workflows/ci.yml#L39-L43) — `+libvorbis-dev` |
| CI MSYS2 packages | [.github/workflows/ci.yml:73-78](.github/workflows/ci.yml#L73-L78) — `+mingw-w64-ucrt-x86_64-libvorbis` |
| CI grep guard (already covers vorbisfile) | [.github/workflows/ci.yml:44-57](.github/workflows/ci.yml#L44-L57) |
| RG generic read (no edit needed) | [src/LocalFileSource.cpp:69-72](src/LocalFileSource.cpp#L69-L72) — first `tryRG("REPLAYGAIN_TRACK_GAIN")` |

---

## 4. Decision confirmations (per brief §4 — checked against the tree, not re-opened)

- **Hand-roll libvorbisfile, not stb_vorbis** — confirmed on all three grounds:
  miniaudio is still the single vendored header (0.11.25, no `extras/`, no
  `stb_vorbis.*` anywhere); the stb path is gated on a macro nothing defines
  (the opus-flight compile probe stands); and the `onInitFileW` argument holds
  — a backend without it is never offered the file on Windows.
- **Wide-path slurp pattern** — reused verbatim from OpusDecoder (`slurpW` /
  `slurpA`, `FSEEK64`/`FTELL64`, 4-byte `OggS` pre-sniff in the slurp).
- **Opus header gain unaffected by the f32 retrofit** — confirmed:
  libopusfile applies `OP_HEADER_GAIN` in its decode pipeline regardless of
  which `op_read*` entry point drains it; the R128 conversion
  (`value/256 + 5`, no header-gain term) is untouched.

---

## 5. VorbisDecoder design

Mirror of OpusDecoder with three deltas, all forced by the `ov_*` API:

1. **Open:** no `ov_open_memory` exists → `ov_open_callbacks` over a `MemStream`
   (the WavPackDecoder shape: `data/size/pos`). Callbacks are fread-shaped:
   `read_func(ptr, size, nmemb, ds)` returning items read; `seek_func(ds,
   offset, whence)` returning 0/-1; `tell_func`; `close_func = nullptr` (the
   backend owns the buffer — §1.3). A non-`OV_OK` open (`OV_ENOTVORBIS` for
   Opus-in-Ogg) → `MA_NO_BACKEND`.
2. **Read:** `ov_read_float(vf, &pcm, want, &bitstream)` returns frames per
   channel and hands back **planar pointers into decoder-internal buffers** —
   interleave immediately into the caller's buffer before the next call
   (`dst[i*ch + c] = pcm[c][i]`, the one new loop in this slice). `OV_HOLE`
   (−3) → `continue` (resync), other negatives / 0 → stop, mirroring the Opus
   loop's `OP_HOLE` handling.
3. **Format:** `ma_format_f32`, `ov_info(vf,-1)->channels`, **native
   `ov_info(vf,-1)->rate`** (not a fixed rate — Vorbis has no 48 kHz
   convention; the forced-format converter resamples).

Seek `ov_pcm_seek` (sample-exact), length `ov_pcm_total(vf,-1)` (negative →
`MA_NOT_IMPLEMENTED`), cursor tracked manually like the siblings. Teardown:
`ov_clear` in `be_uninit` before `delete` (§1.3 ordering).

**ReplayGain: confirmed no edit.** Vorbis carries plain-dB
`REPLAYGAIN_TRACK_GAIN` in Xiph comments; TagLib's generic `properties()` path
feeds the existing first `tryRG` ([anchor §3]). The `.opus`-gated R128 branch
is not touched and cannot capture `.ogg` (it keys on extension).

## 6. Opus f32 retrofit (exact shape)

At the §3 anchors: `dst` becomes `float*`, `op_read` becomes `op_read_float`
(same `cap` computation — both take a value count in an int), `get_format`
reports `ma_format_f32`. Header comment updated. Nothing else — no interleave
loop (§1.1), no gain math change (§4), no seek/length/cursor change.

**Mandatory re-test, not assumed-safe** (retouches just-merged code): the full
opus RG matrix re-runs on the f32 path — `R128_TRACK_GAIN=-2560` must still
read **−5.00 exactly**, RG-on must not be silent, loudness within ~1 dB of the
same-master FLAC. The merged flight's probe and generated files still exist
(`C:\Users\david\smoke\`, WSL `~/owv/`) and re-run as-is.

## 7. Coexistence (brief hazard 1) — mechanism + proof plan

Two backends now sniff `OggS`; discrimination is each library's own parser:
Opus-in-Ogg fails `ov_open_callbacks` with `OV_ENOTVORBIS`, Vorbis-in-Ogg
fails `op_open_memory` with `OP_ENOTFORMAT` — both map to `MA_NO_BACKEND` and
miniaudio moves to the next vtable (each backend re-slurps from the path
itself, so there is no shared read cursor to corrupt). Half of this is already
measured: the merged flight's `vorbis.ogg` returned OPEN_FAIL with the Opus
backend registered — clean rejection, no crash, no capture. The §8 coexistence
test proves both directions live rather than asserting them. Registration
order stays append-only (`..., wavpack, vorbis`); order-independence is the
same 4-byte-sniff argument as the merged slice, now with the two-OggS caveat
delegated to the libraries' parsers.

## 8. Dependencies / CMake (verified on both toolchains)

| | MSYS2 UCRT64 (7of9) | Debian Trixie (WSL2 + CI) |
|---|---|---|
| libvorbis | **installed** 1.3.7-2 (`libvorbis.a/.dll.a`, `libvorbisfile.a/.dll.a` present) | not installed; candidate `1.3.7-3`; layout `/usr/include/vorbis/` — identical to MSYS2 |

CMake block mirrors the opus one — root include only (§1.2):

```cmake
# ── libvorbis + libvorbisfile (Ogg Vorbis decode: .ogg) ────────────────────
find_library(VORBIS_LIB     NAMES vorbis     PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_library(VORBISFILE_LIB NAMES vorbisfile PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
# Root include only: vorbisfile.h pulls its siblings via QUOTED includes
# (probe-verified), so no <prefix>/include/vorbis entry exists to order.
# The bare-include ban is the CI grep gate, which already covers vorbisfile.
find_path(VORBIS_INCLUDE NAMES vorbis/vorbisfile.h PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "vorbisfile: ${VORBISFILE_LIB}")
```

Link order (dependency chain): `${VORBISFILE_LIB} ${VORBIS_LIB}` inserted
**before the existing `${OGG_LIB}`** at [CMakeLists.txt:297](CMakeLists.txt#L297)
— vorbisfile → vorbis → ogg, and ogg is already positioned last of the Xiph
group from the opus slice. Same insertion in `xfade_handoff_test`'s libs
(which already has its own `${OGG_LIB}` from the opus flight — the static-probe
per-target lesson is already applied; static `libvorbisfile.a` slots into it).
Static Option C: the new `.a`s fold in; **no new DLLs expected on the 2-DLL
allowlist** — verify with `ldd` exactly as last time.

Release-checklist item (record, don't bundle): the **dynamic** Windows bundle
gains `libvorbis-0.dll` + `libvorbisfile-3.dll` (libogg-0.dll already listed
from the opus slice; libvorbisenc is NOT needed — decode only).

## 9. Version / changelog

Propose folding into the existing **`[1.3.0] - UNRELEASED`** block (add an
"Ogg Vorbis playback" bullet + Opus-now-f32 note) rather than minting 1.4.0 —
1.3.0 hasn't shipped, and "the lossy-decode release" reads as one unit. Dos
confirms at review (podcast renumbering is separately handled per the last
brief).

## 10. Proposed verification (on greenlight)

1. **Vorbis decode:** generated + real `.ogg` end-to-end; seek to mid; **a
   near-EOF read after a long seek** (the §1.3 buffer-lifetime exercise);
   duration; OGG label already renders.
2. **Coexistence:** `.opus` and `.ogg` decoded correctly in the same session
   with all four backends registered; `vorbis.ogg` no longer OPEN_FAIL (the
   baseline flip), `test.opus` unchanged.
3. **Opus f32 re-test (mandatory):** full RG matrix re-run — −5.00 exact,
   RG-on audible, FLAC loudness cross-check within ~1 dB.
4. **Vorbis RG:** a `REPLAYGAIN_TRACK_GAIN`-tagged `.ogg` applies via the
   generic path (vorbiscomment or sox can write the tag).
5. **Regression:** WavPack/FLAC/MP3/AAC/WAV probe rows unchanged; ctest green
   both toolchains including `xfade_handoff_test`; Option C `ldd` still 2-DLL.
6. **First-file WASAPI** (`.ogg` first file of a fresh session) + listening
   pass: Dos's human gates, as before.
7. **Acceptance:** the original ".ogg still decodes" check — impossible when
   first written, baseline-flipped by this slice — now passes. Close the loop
   explicitly in the debrief.
8. **Gate note:** Joan Osborne not invoked; no CD-path file touched.

## 11. Change inventory (on greenlight)

§2 table, verbatim — 2 new files, 11 edits. Audio thread untouched; no new
threading; pull-model `ma_data_source` on the existing seam, like its three
siblings.
