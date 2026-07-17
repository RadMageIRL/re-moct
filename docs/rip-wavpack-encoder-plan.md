# Slice `rip-wavpack-encoder` — WavPack rip output (PLAN)

**Stage:** PLAN. No source written, nothing committed.
**Baseline:** `experimental/win-pdcurses` at `146638b` (post rip-opus-encoder).
**Position:** slice 5 — the final encoder. After this the modal is
FLAC/MP3/WAV/Opus/WavPack, digits 1-5, any combination from one verified read.

**§0 calls, both taken as recommended:**
1. **Hybrid: pure lossless only** — `wvc_id = NULL`, one `.wv` per track.
   Matches the decode side (which defers `.wvc` structurally), keeps WavPack
   a clean verifiable master, avoids one-format-two-files fan-out.
2. **Art: IN-SCOPE — the probe says it's clean.** TagLib's `APE::Tag` has
   `setData(key, ByteVector)` which creates a Binary item directly; the
   APEv2 art convention is item key `"Cover Art (Front)"` with payload
   `"cover.jpg" + '\0' + <jpeg bytes>`. That is one ByteVector concat plus
   one call — manual, but three lines and idiomatic (setData exists for
   exactly this). Ships with tags+RG; override at review if you'd rather
   defer.

---

## 1. Probe results — the "no new dep" claim is CONFIRMED, both toolchains

- **Encode API in the installed header** (`wavpack/wavpack.h`):
  `WavpackOpenFileOutput(blockout, wv_id, wvc_id)` /
  `WavpackSetConfiguration64(wpc, config, total_samples, chan_ids)` /
  `WavpackPackInit` / `WavpackPackSamples(wpc, int32_t*, count)` /
  `WavpackFlushSamples` / `WavpackUpdateNumSamples(wpc, first_block)`.
- **Symbols resolve in the already-linked libs:** `nm` shows all of them
  defined in MSYS2's `libwavpack.a` AND Debian's `libwavpack.so.1.2.7`.
  **Zero CMake/CI/bundle/notices work** — one library, both directions,
  already found, linked, CI-installed, bundled, and noticed since the
  decode slice.
- **Mode flags:** `CONFIG_FAST_FLAG 0x200` / normal = 0 / `CONFIG_HIGH_FLAG
  0x800` / `CONFIG_VERY_HIGH_FLAG 0x1000` (hybrid's `CONFIG_HYBRID_FLAG`
  exists and is deliberately unused, §0.1).
- **TagLib:** `WavPack::File::APETag(true)` exists; `APE::Tag::addValue`
  (text) + `setData` (binary) cover fields and art (§0.2).
- **Naming: `WavPackEncoder` is free** — libwavpack's only opaque type is
  `WavpackContext`; no clash (unlike the Opus case). Plain name it is.

## 2. Resolved anchors

| what | where |
|---|---|
| Factory switch | [src/CDRipper.cpp:766-772](src/CDRipper.cpp#L766-L772) — +`case RipFormat::WavPack` |
| tagFile branches (additive insert after Opus) | [src/CDRipper.cpp](src/CDRipper.cpp) — `is_opus` branch ends ~:800; add `is_wv` + branch |
| RG formatter lambdas (reused, plain dialect) | `rg_str`/`rg_peak_str` at [:683-690](src/CDRipper.cpp#L683-L690) region |
| Row table | [include/RipFormats.h](include/RipFormats.h) — append `{ WavPack, "WavPack", ".wv", true, true, "" }`, digit 5, final |
| RipOptions | gains `int wavpack_mode` (enum-ish int: 0 fast / 1 normal / 2 high / 3 very-high) |
| Config pattern | `src/Config.cpp` — `wavpack_mode` string key `fast|normal|high|very_high`, default `normal` |
| Modal quality text | [src/UIManager.cpp:1760-1766](src/UIManager.cpp#L1760-L1766) region — shows the mode word |
| Exact-total invariant (reused) | [src/CDRipper.cpp:920](src/CDRipper.cpp#L920) — same finding as WAV §3.7 |
| Decode-side round-trip partner | `src/WavPackDecoder.cpp` (the existing decoder = the lossless-proof reader) |
| Seam test target | `tests/CMakeLists.txt` — +WavPackEncoder.cpp (libwavpack already linked there? NO — the seam test links FLAC/LAME/ogg only; **it gains `${WAVPACK_LIB}`**, the one build-file line this slice needs beyond sources) |

## 3. Design

### 3.1 `WavPackEncoder` (`include/WavPackEncoder.h` + `src/WavPackEncoder.cpp`)

- **open(path, total_frames):** `port::fopenUtf8` →
  `WavpackOpenFileOutput(blockOut, this, NULL)` — the fwrite-shaped block
  callback writes through OUR `FILE*` (the house wide-path move; no `char*`
  crosses the library). Config exactly CD-DA: `bits_per_sample 16`,
  `bytes_per_sample 2`, `num_channels 2`, `sample_rate 44100`, `flags` from
  the mode map. `WavpackSetConfiguration64(..., (int64_t)total_frames, NULL)`
  → header complete upfront (total is TOC-exact per the WAV-slice finding) →
  `WavpackPackInit`.
- **writeFrames:** widen s16 → member `std::vector<int32_t>` scratch
  (`frames × 2`, sign-extension is the implicit int conversion), then
  `WavpackPackSamples(wpc, scratch, frames)`; **strict** — 0 return fails
  the track.
- **finalize(ok):** on ok `WavpackFlushSamples`; **insurance path** for a
  non-exact caller (never taken on the CD path): the block callback keeps a
  copy of the FIRST block; if `WavpackGetSampleIndex64 != declared`, call
  `WavpackUpdateNumSamples(wpc, first_block_copy)` and rewrite it at offset
  0 — the WAV back-patch pattern, WavPack-shaped. `WavpackCloseFile`,
  `fclose`. On `!ok`: close only; partial removal is the caller's (sibling
  behavior).
- **Ctor:** `WavPackEncoder(int mode = kWavPackDefaultMode)` with
  `inline constexpr int kWavPackDefaultMode = 1;  // normal` — the named
  default; the config default maps to the same constant.

### 3.2 tagFile WavPack branch (additive, the fifth format)

`else if (is_wv)` (`path.ends_with(".wv")`) after the Opus branch:
`TagLib::WavPack::File f(wp, false)` → `f.APETag(true)` → `setTitle/Artist/
Album/Track/Year` (APE::Tag implements the base Tag API) → `addValue` for
ENCODER + AR fields → **plain `REPLAYGAIN_*` all four keys via the existing
`rg_str`/`rg_peak_str` formatters** (the FLAC/MP3 dialect — APEv2 is where
that dialect is native; NOT the R128 path; the decode slice already proved
the player reads it back via the generic path: `rg.wv → −6.50`) → art via
`setData("Cover Art (Front)", ByteVector("cover.jpg\0" + jpeg))` →
`f.save()`. Byte-identity for FLAC/MP3/Opus is structural (untouched
`else if` siblings) and re-proven by the slice-4 method: whole-file SHA of
fresh small rips vs the retained artifacts.

### 3.3 Row / config / cue

Row `{ WavPack, "WavPack", ".wv", true, true, "" }` — lossless (`*`),
taggable, no note, digit 5, final. Token `wavpack` free via the table-driven
parse. `wavpack_mode` config: string `fast|normal|high|very_high` → int →
ctor; modal text = the mode word (`normal`). Cue/M3U8: FLAC+WavPack → FLAC
(table order); WavPack-only → `.wv` (a valid lossless master).
`kOpusDefaultBitrate` precedent followed for the named default.

## 4. Hazard decisions

| hazard | decision |
|---|---|
| 16-bit config correctness | exact CD-DA config (§3.1); the §5.1 bit-exact round-trip is the catch — a wrong bits/bytes cannot survive it |
| Lossless proven, not assumed | round-trip through the EXISTING WavPackDecoder (fixture) + WavPack-vs-FLAC PCM identity (real rip) |
| tagFile additivity | additive `else if`; siblings untouched; whole-file SHA re-proof (the slice-4 method) |
| Art in APEv2 | in-scope, `setData` + the filename-NUL convention (§0.2 probe); 3 lines, no half-measures |
| Hybrid | out — `wvc_id = NULL`, recorded |
| Wide paths | block callback over `port::fopenUtf8` FILE* |
| Naming | `WavPackEncoder` free — no clash (probed) |

## 5. Verification to PROPOSE (on greenlight)

1. **Bit-exact round-trip (the lossless proof):** fixture PCM →
   `WavPackEncoder` → decode via `WavPackDecoder` (the shipping decoder) →
   bit-identical to input. Seam-test family, both toolchains. Proves
   compression losslessness AND the 16-bit config in one shot.
2. **WavPack-vs-FLAC PCM identity:** 1-track rip with FLAC+WavPack →
   FLAC's embedded PCM-MD5 vs the decoded `.wv` MD5 → identical.
3. **Default case:** seam oracle green; FLAC/MP3 whole-file SHA identical to
   retained artifacts (and Opus vs the slice-4 subO artifact).
4. **Trimmed [A] gate:** 2 tracks, GHD3N, WavPack in the fan-out
   (full-TOC/cancel-at-3) — canonical CRCs unchanged.
5. **Audition:** the ripped `.wv` plays via the existing decoder — duration
   == TOC, envelope, seek/tail.
6. **Subset checks:** WavPack-only → only `.wv`, plain `REPLAYGAIN_*` in the
   APE tag (no R128 keys), cue/m3u8 → `.wv`; FLAC+WavPack → cue → `.flac`.
7. **Option C `ldd` still 2-DLL**; ctest both toolchains; CHANGELOG entry
   (hyphens only).

## 6. Change inventory (on greenlight)

| file | change |
|---|---|
| `include/WavPackEncoder.h` / `src/WavPackEncoder.cpp` | **new** |
| `include/RipFormats.h` | +enum value, +row, +`wavpack_mode` in RipOptions |
| `src/CDRipper.cpp` | +factory case; +tagFile APEv2 branch (additive) |
| `src/UIManager.cpp` | +quality-text case; mode → RipOptions |
| `include/Config.h` / `src/Config.cpp` | `wavpack_mode` key |
| `CMakeLists.txt` | +source only (lib already linked) |
| `tests/CMakeLists.txt` | +source **+ `${WAVPACK_LIB}`** to the seam test (the one build-line addition — that target links FLAC/LAME only today) |
| `tests/rip_encoder_seam_test.cpp` | +round-trip section (links WavPackDecoder.cpp too for the read-back) |
| `CHANGELOG.md`, `docs/lessons.md` | entries |

**No new dependency, no CI change, no bundle change, no notices change** —
confirmed by symbol probes on both toolchains (§1).

## 7. Recorded (§9 of the brief)

Log-semantics marking (lossless = verifiable master vs derived lossy) is now
a clean self-contained follow-up — **recommend its own slice** per the brief's
default, keeping this one to the encoder. After this slice the overhaul's end
state is reached: five formats, digits 1-5, any combination, one verified read.
