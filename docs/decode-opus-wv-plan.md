# Slice `decode-opus-wv` — Opus + WavPack decode support (PLAN)

**Stage:** PLAN. No source written, nothing committed.
**Scope:** decode/playback only, additive, non-CD-path.
**Live tree resolved:** `E:\code\remoct` (7of9), WSL2 Debian Trixie at `/mnt/e/code/remoct`.
**Target branch on greenlight:** `github.com/radmageirl/experimental`.

Every anchor below was resolved against the live checkout. Divergences from the
brief are in §1 — read that first, two of them change the shape of the work.

---

## 1. Divergence from the brief (READ FIRST)

The `/mnt/project` snapshot the brief was written from lags the live tree. Six
findings differ from what the brief describes. Per the brief's §0 STOP clause I
am reporting rather than proceeding.

### 1.1 `.opus` is ALREADY in the extension gate — and is a browse-and-fail trap today

The brief's §5 says the playable-extension allow-list is unknown, and asks to
"plan the addition of `.opus` and `.wv`". Live, the allow-list is
[PlaylistManager.cpp:32-35](src/PlaylistManager.cpp#L32-L35):

```cpp
static const std::vector<std::string> AUDIO_EXTS = {
    ".mp3", ".flac", ".ogg", ".opus", ".wav", ".aiff", ".aif",
    ".m4a", ".m4b", ".aac", ".wma", ".mp4"
};
```

`.opus` is already there, and `"OPUS"` is already a file-type label in
[UIManager.cpp:2198-2199](src/UIManager.cpp#L2198-L2199). So `.opus` files are
**already browsable, already labelled, and already un-playable** — `open_decoder`
has no Opus backend, `ma_decoder_init_file_w` fails, `LocalFileSource::open`
returns false. This slice does not *add* Opus to the gate; it *repairs* a gate
entry that has been advertising a format the engine cannot decode.

Only `.wv` needs adding, and only to two lists.

### 1.2 There is no built-in Vorbis decoder — `.ogg` is the same trap, and hazard 4 is moot

The brief's hazard 4 asks to protect the built-in Vorbis decoder from being
shadowed by the Opus vtable on `.ogg`. **There is no built-in Vorbis decoder in
this build.** miniaudio gates its entire stb_vorbis backend on
`#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H` ([lib/miniaudio.h:65330](lib/miniaudio.h#L65330)),
which is only defined if the user includes stb_vorbis themselves before
miniaudio.h. There is no `stb_vorbis.*` anywhere in the tree, and
[AudioManager.cpp:1](src/AudioManager.cpp#L1) is a bare `#define MINIAUDIO_IMPLEMENTATION`.

Proven empirically in WSL rather than by inspection, compiling exactly as the
host does, with a control to show the probe is sensitive:

```
--- host's own flags (-DMINIAUDIO_IMPLEMENTATION, no stb_vorbis):
VORBIS_IS_ABSENT
--- control (+ -DSTB_VORBIS_INCLUDE_STB_VORBIS_H):
VORBIS_IS_COMPILED_IN
```

Consequences:
- **Hazard 4 dissolves.** Nothing can be shadowed; vtable order cannot break Vorbis.
  The Opus sniff must still be tight (it is, §4.1), but not to protect Vorbis.
- **A latent defect is exposed:** `.ogg` in `AUDIO_EXTS` and `"OGG"` in `fileTypeTag`
  are dead exactly like `.opus`. `.ogg` Vorbis files do not play today. The
  brief's §8 regression check "`.ogg` Vorbis still decodes" **cannot pass, and
  cannot ever have passed.**
- **This is out of scope but cheap later.** This slice links libogg anyway; a
  follow-up could add stb_vorbis or an Ogg-Vorbis vtable. Flagging, not doing.

### 1.3 The R128 ReplayGain path exists and is actively wrong — a tagged Opus file plays SILENT

The brief asks to "plan an Opus branch in the RG read". A branch already exists,
at [LocalFileSource.cpp:59-63](src/LocalFileSource.cpp#L59-L63):

```cpp
float rg_db = tryRG("REPLAYGAIN_TRACK_GAIN");
if (rg_db == 0.0f) rg_db = tryRG("replaygain_track_gain");
if (rg_db == 0.0f) rg_db = tryRG("R128_TRACK_GAIN");
```

`tryRG` is `std::stof` — it reads `R128_TRACK_GAIN` as **plain decimal dB**. That
tag is Q7.8 fixed-point (dB × 256). A perfectly ordinary `-1234` (= −4.82 dB)
is read as **−1234 dB**, and the audio thread
([AudioManager.cpp:811-821](src/AudioManager.cpp#L811-L821)) then computes
`pow(10, -1234/20)` → `0.0` → `clamp(0, 0, 4)` → **0.0** → total silence for the
whole track, with ReplayGain enabled.

So this is not "add a branch" — it is "fix a branch that currently mutes tagged
Opus files". It is inert today only because nothing can decode Opus in the first
place; **making Opus decode is what arms this bug.** The RG fix is therefore not
optional polish, it is part of the decode slice.

### 1.4 The Debian/MSYS2 include layouts are IDENTICAL, not different

The brief's §7 says Debian's "nested include layout differs from MSYS2" and asks
to handle the difference. It does not differ. Verified by unpacking the actual
Trixie `.deb`s (no install) and listing MSYS2:

| header | MSYS2 UCRT64 | Debian Trixie |
|---|---|---|
| opus | `include/opus/opus.h` | `/usr/include/opus/opus.h` |
| opusfile | `include/opus/opusfile.h` | `/usr/include/opus/opusfile.h` |
| wavpack | `include/wavpack/wavpack.h` | `/usr/include/wavpack/wavpack.h` |

Both nest identically, exactly like `fdk-aac/aacdecoder_lib.h`. **No per-platform
include handling is needed**, and the CMake block collapses to a verbatim mirror
of the FDK-AAC one (§6). This makes the dependency work materially smaller than
the brief budgeted for.

### 1.5 There is no `extras/` — miniaudio is a single vendored header

The brief's §3 note suggests evaluating miniaudio's reference libopus backend
from its `extras/` folder. That folder does not exist in this tree: miniaudio is
one vendored `lib/miniaudio.h` (v0.11.25, 4.1 MB). Adopting the reference
backend means vendoring new third-party files + `THIRD-PARTY-NOTICES.md` churn.
Combined with §4.4, the recommendation is **hand-roll**.

### 1.6 Header/file-layout details

- Headers live in `include/`, not beside the `.cpp` (`include/AacDecoder.h` +
  `src/AacDecoder.cpp`). New headers follow that.
- The `Shift+F` file-type column's own comment
  ([UIManager.cpp:2186](src/UIManager.cpp#L2186)) still says "F11" — stale, the real
  binding is `Shift+F` at [UIManager.cpp:5365](src/UIManager.cpp#L5365). Cosmetic,
  not touched by this slice.

---

## 2. Files to add

| file | responsibility |
|---|---|
| `include/OpusDecoder.h` | declares `ma_opus_backend_vtable()`; doc-comment mirroring `AacDecoder.h` |
| `src/OpusDecoder.cpp` | libopus/libopusfile-backed `ma_data_source` backend for `.opus` |
| `include/WavPackDecoder.h` | declares `ma_wavpack_backend_vtable()` |
| `src/WavPackDecoder.cpp` | libwavpack-backed `ma_data_source` backend for `.wv` |

Both mirror `AacDecoder.{h,cpp}` structurally: anonymous namespace, `ma_data_source_base ds`
as first member, whole-file slurp on init, magic-sniff before the slurp completes,
`MA_NO_BACKEND` on any non-match, singleton vtable via an `extern "C"` accessor.

**Naming note:** libopus's own `opus.h` typedefs a struct called `OpusDecoder`.
Our *file* `OpusDecoder.h` does not collide (libopus ships no such filename) and
we declare no C++ type of that name — only `ma_opus_backend_vtable()`. Safe, but
worth knowing before someone adds a class later.

---

## 3. Resolved live-tree anchors

| what | file:line | detail |
|---|---|---|
| Registration site | [src/LocalFileSource.cpp:92-95](src/LocalFileSource.cpp#L92-L95) | in file-static `open_decoder` (declared :79) |
| Forced output format | [src/LocalFileSource.cpp:89](src/LocalFileSource.cpp#L89) | `ma_decoder_config_init(ma_format_f32, 2, 44100)` |
| TagLib hints discarded | [src/LocalFileSource.cpp:81](src/LocalFileSource.cpp#L81) | `(void)hint_channels; (void)hint_rate;` |
| BPM analysis decoder | [src/AudioManager.cpp:911-914](src/AudioManager.cpp#L911-L914) | `detectBpm`, separate AAC registration |
| WASAPI warm-up | [src/AudioManager.cpp:170-204](src/AudioManager.cpp#L170-L204) | `initDevice`, function-local `static bool` |
| Extension allow-list | [src/PlaylistManager.cpp:32-35](src/PlaylistManager.cpp#L32-L35) | `AUDIO_EXTS`, lowercase, dot-prefixed |
| Allow-list predicate | [src/PlaylistManager.cpp:37-41](src/PlaylistManager.cpp#L37-L41) | `isSupportedAudio`, case-insensitive |
| File-type labels | [src/UIManager.cpp:2198-2199](src/UIManager.cpp#L2198-L2199) | `known[]` in `fileTypeTag`, uppercase, no dot |
| RG tag read | [src/LocalFileSource.cpp:52-63](src/LocalFileSource.cpp#L52-L63) | `tryRG` chain in `populate_track_info` |
| RG storage | [include/TrackInfo.h:24](include/TrackInfo.h#L24) | `float replaygain_db = 0.0f;` |
| RG audio-thread mirror | [include/AudioManager.h:271-275](include/AudioManager.h#L271-L275) | `std::atomic<float> current_rg_db_` |
| RG apply | [src/AudioManager.cpp:811-821](src/AudioManager.cpp#L811-L821) | `maDataCallback`; `pow(10,db/20)` × `pow(10,6/20)` preamp, clamp ≤4.0 |
| CMake FDK-AAC dep block | [CMakeLists.txt:181-194](CMakeLists.txt#L181-L194) | the block to mirror |
| CMake sources | [CMakeLists.txt:66](CMakeLists.txt#L66) | `src/AacDecoder.cpp` |
| CMake link | [CMakeLists.txt:253-271](CMakeLists.txt#L253-L271) | `${FDKAAC_LIB}` at :258 |
| Test that links decoders | [tests/CMakeLists.txt](tests/CMakeLists.txt) | `xfade_handoff_test` links `src/AacDecoder.cpp` |

---

## 4. Design

### 4.1 `src/OpusDecoder.cpp`

**Library:** libopusfile (`op_*`) over libopus. `#include <opus/opusfile.h>` — see
the collision hazard in §5.1, the `opus/` prefix is **mandatory**, not stylistic.

**Open path:** slurp → `op_open_memory(data, size, &err)`.

Using the *memory* entry point (not `op_open_file`) is the load-bearing choice:

- It matches the house slurp pattern.
- It **sidesteps the Windows Unicode-path problem entirely.** `op_open_file` takes
  a `const char*`; the host opens files via `ma_decoder_init_file_w` with a
  `wchar_t*` ([LocalFileSource.cpp:98](src/LocalFileSource.cpp#L98)). With
  `op_open_memory` we do our own `_wfopen` slurp (mirroring `slurpW` at
  [AacDecoder.cpp:361](src/AacDecoder.cpp#L361)) and libopusfile never sees a path.

**Sniff (two-stage, cheap first):**
1. In the slurp: bytes 0-3 == `"OggS"`. Fails on FLAC/MP3/WAV/AAC before reading
   the whole file, exactly like `looksAdts`/`looksMp4`.
2. After slurp: `op_open_memory` failing (`OP_ENOTFORMAT` for Vorbis-in-Ogg) →
   `MA_NO_BACKEND`.

libopusfile's own parser is the Opus-vs-Vorbis discriminator. **No hand-rolled Ogg
page walking** — the brief's hazard 4 sniff is the library's job, and it is the
one component guaranteed to agree with the format spec.

**Format reported by `get_format`:** `ma_format_s16`, `op_channel_count(of, -1)`
channels, **48000 Hz always** (Opus is always 48 kHz internally; libopusfile
always outputs 48 kHz regardless of the encoder's `input_sample_rate`). The
forced-format converter resamples 48000 → 44100. Read via `op_read` (native
`opus_int16`), mirroring AAC's s16 output.

**Seek / cursor / length:** `op_pcm_seek`, `op_pcm_total(of, -1)` — both exact and
native, so this backend is simpler than `AacDecoder`'s sample-table machinery
(no MP4 box tree, no ADTS frame walk).

**Header output gain:** left alone deliberately. libopusfile applies the
`OpusHead.output_gain` field by default (`OP_HEADER_GAIN`, offset 0), so the PCM
`op_read` returns already has it applied. We must **not** call
`op_set_gain_offset`, and the RG branch in §4.3 is defined relative to
post-header-gain level, which is exactly what RFC 7845 specifies for
`R128_TRACK_GAIN`. This is why §4.3's conversion has no header-gain term.

### 4.2 `src/WavPackDecoder.cpp`

**Library:** libwavpack. `#include <wavpack/wavpack.h>`.

**Open path:** slurp → `WavpackOpenFileInputEx64` with a `WavpackStreamReader64`
over the in-memory buffer, `wvc_id = NULL`, flags `OPEN_NORMALIZE`.

Same reasoning as Opus (house style + Windows wide paths), plus one bonus: passing
`wvc_id = NULL` **defers `.wvc` by construction** rather than by a flag we might
forget — a memory reader has no filename to derive the sidecar from.

**Sniff:** bytes 0-3 == `"wvpk"`. Trivial and unambiguous; disjoint from every
other magic in play.

**Bit depth — the brief's hazard 3, and a deliberate deviation from the AAC template:**

`WavpackUnpackSamples` always returns `int32_t` per sample, whatever the source
depth. Rather than truncate to s16, this backend reports **`ma_format_f32`**:

- `WavpackGetBitsPerSample()` → scale int32 by `1.0f / (1 << (bps-1))`.
- `WavpackGetMode() & MODE_FLOAT` → samples are already float bit-patterns in the
  int32 buffer; `memcpy` them out (`OPEN_NORMALIZE` puts them in ±1.0).

**Why deviate from AAC's s16:** FDK-AAC is natively fixed-point s16, so s16 costs
AAC nothing. WavPack is natively up to 32-bit and commonly 24-bit; emitting s16
would throw away 8 bits *for free*, since the device chain is f32/44100 anyway
([LocalFileSource.cpp:89](src/LocalFileSource.cpp#L89)). f32 out is lossless
through the converter and costs nothing. Flagging as an intentional,
justified departure from the template.

This directly answers the brief's "confirm the forced-format conversion path
accepts >16-bit input": it does — miniaudio's converter takes any
format/rate/channel combination the data source reports and converts to the
config's f32/2/44100. 24-bit and non-44.1 kHz are **converted, not rejected**.
Non-44.1 kHz WavPack resamples the same way Opus's 48 kHz does.

**`.wvc` hybrid correction: DEFERRED (recommended, per the brief).** Plain `.wv`
only. A hybrid `.wv` without its `.wvc` still decodes — it is a valid lossy
stream — so the failure mode is "plays at lossy quality", not "fails". Acceptable
for this slice; revisit only if asked.

**Seek / cursor / length:** `WavpackSeekSample64`, `WavpackGetNumSamples64`.

### 4.3 The Opus ReplayGain branch — exact conversion

Replace the broken `R128_TRACK_GAIN` fallback (§1.3) with a correctly-typed branch.

```
replaygain_db = (float)R128_TRACK_GAIN_q78 / 256.0f + 5.0f
```

Two terms, each load-bearing:

1. **`/ 256.0` — Q7.8 fixed-point → dB.** The tag is an *integer* string of
   dB×256. This is the term whose absence causes today's silence bug.
2. **`+ 5.0` — reference re-basing.** R128 references **−23 LUFS**; ReplayGain
   references **−18 LUFS** (89 dB). The tag's gain lands the track at −23 LUFS;
   reaching −18 LUFS needs **5 dB more**. Without this, Opus plays ~5 dB quiet
   relative to every other format — the brief's stated hazard, and the number
   is 5, not 6 (6 dB is the unrelated preamp, see below).

**No header-gain term** — libopusfile already applied it (§4.1), and RFC 7845
defines `R128_TRACK_GAIN` as relative to post-header-gain level. The two facts
cancel exactly; this is why the design pins `OP_HEADER_GAIN` default.

**Ordering:** the Opus branch must be tried **before** the generic
`REPLAYGAIN_TRACK_GAIN` lookups, or an Opus file carrying both tags takes the
wrong path. Gate it on the file actually being Opus, not on tag presence — some
taggers write both, and `R128_TRACK_GAIN` is authoritative for `.opus`.

**Explicitly NOT touched:** the unconditional `+6 dB` preamp at
[AudioManager.cpp:815](src/AudioManager.cpp#L815). It is a pre-existing
83→89 dB-reference preamp applied to *every* format. The goal here is that Opus
arrives at `replaygain_db` **on the same convention as `REPLAYGAIN_TRACK_GAIN`**,
so the preamp treats Opus and FLAC identically. Re-litigating the preamp is a
different slice. (Two pre-existing oddities noticed in passing, both out of
scope: the preamp boosts *untagged* tracks +6 dB, and `REPLAYGAIN_TRACK_PEAK` is
written by the ripper but never read back, so there is no clip protection.)

**WavPack RG needs no branch** — APEv2 carries plain-dB `REPLAYGAIN_TRACK_GAIN`,
which the existing first `tryRG` already handles.

### 4.4 Recommendation: hand-roll, do not adapt miniaudio's reference backend

**Hand-roll.** Three reasons, the third decisive:

1. **There is no `extras/` in the tree** (§1.5). "Adapt" means vendoring new
   third-party source + notices churn, not reusing something already present.
2. **House style.** The reference backend is streaming-callback shaped; the house
   pattern is slurp-then-parse, deliberately ("this removes a class of streaming-I/O
   bugs" — [AacDecoder.cpp:6-9](src/AacDecoder.cpp#L6-L9)).
3. **The reference backend does not implement `onInitFileW`, and would fail
   silently on Windows.** miniaudio hard-stops when it is null —
   [lib/miniaudio.h:63111-63113](lib/miniaudio.h#L63111-L63113):

   ```c
   if (pVTable->onInitFileW == NULL) {
       return MA_NOT_IMPLEMENTED;
   }
   ```

   No fallback to the callback path. Since the host opens via
   `ma_decoder_init_file_w` on Windows, a backend without `onInitFileW` is never
   given the file — the custom-backend loop just moves on and Opus silently never
   plays. `AacDecoder` implements `be_init_file_w` ([AacDecoder.cpp:427](src/AacDecoder.cpp#L427))
   for exactly this reason. Adapting the reference backend would mean rewriting
   its entry points anyway, which is most of the work.

### 4.5 Registration site edit

At [LocalFileSource.cpp:92-95](src/LocalFileSource.cpp#L92-L95), the array grows
to three; the forced-format `cfg` above it is untouched and still applies:

```cpp
static ma_decoding_backend_vtable* k_custom_backends[] = {
    ma_aac_backend_vtable(),
    ma_opus_backend_vtable(),
    ma_wavpack_backend_vtable(),
};
cfg.ppCustomBackendVTables = k_custom_backends;
cfg.customBackendCount     = 3;
cfg.pCustomBackendUserData = nullptr;
```

**Ordering: irrelevant to correctness, by construction.** The three sniffs are
disjoint on the first four bytes — `FF Fx`/`ftyp` (AAC), `OggS` (Opus), `wvpk`
(WavPack) — and each returns `MA_NO_BACKEND` on a miss. No format can shadow
another, and with no built-in Vorbis (§1.2) there is nothing downstream to
shadow either. Appending is chosen purely to keep the diff additive.

### 4.6 BPM-analysis decoder: IN SCOPE (decision)

[AudioManager.cpp:911-914](src/AudioManager.cpp#L911-L914) registers AAC
separately for `detectBpm`. **Decision: register all three there too.**

Rationale: leaving it AAC-only means BPM silently returns nothing for `.opus`/`.wv`
while working for every other playable format — a per-format inconsistency users
would read as a bug, at the cost of two array entries. It is a throwaway
analysis decoder on its own worker thread, no audio-thread involvement.

One caveat to record, **not to fix here**: that site uses
`ma_decoder_config_init(ma_format_f32, 1, 0)` — channels 1, **rate 0** — which is
precisely the format-sniffer path `open_decoder`'s comment warns against
([LocalFileSource.cpp:82-88](src/LocalFileSource.cpp#L82-L88)). Pre-existing, off
the main thread, unchanged by this slice.

### 4.7 Extension gate + label edits

Only `.wv`. `.opus` is already present in both (§1.1).

- [PlaylistManager.cpp:32-35](src/PlaylistManager.cpp#L32-L35): add `".wv"`.
- [UIManager.cpp:2198-2199](src/UIManager.cpp#L2198-L2199): add `"WV"` to `known[]`.
  Label `WV` (2 chars, fits the 5-col `ftw`). `"WAVPACK"` would overflow.

Neither `isAudiobook` ([PlaylistManager.cpp:43](src/PlaylistManager.cpp#L43)) nor
the MP4 chapter probe ([UIManager.cpp:1741](src/UIManager.cpp#L1741)) applies —
neither format is a book or MP4-family container.

### 4.8 TagLib coverage — confirmed, no gap, no per-format branch

TagLib 2.2.1 (MSYS2 `mingw-w64-ucrt-x86_64-taglib 2.2.1-1`), with
`opusfile.h`/`opusproperties.h`/`xiphcomment.h` and
`wavpackfile.h`/`wavpackproperties.h`/`apetag.h` all present.

`FileRef` resolves `.opus` → `Ogg::Opus::File` (Xiph comments) and `.wv` →
`WavPack::File` (APEv2). `populate_track_info`'s generic `ref.tag()` +
`tag->properties()` path picks up both with **no per-format branch** — title /
artist / album / genre / comment / year / track and `audioProperties()`
duration / bitrate / sampleRate / channels all work as-is.

The single exception is the RG *value semantics* for Opus (§4.3) — which is a tag
**interpretation** gap, not a tag **coverage** gap.

`mp4AacChannelCount(path)` ([LocalFileSource.cpp:73](src/LocalFileSource.cpp#L73))
is called unconditionally but returns ≤0 for non-MP4 input, so it is inert for
both formats. No change needed.

---

## 5. Hazards — decisions

### 5.1 NEW hazard the brief did not anticipate: TagLib ships its own `opusfile.h`

TagLib installs `taglib/opusfile.h` (its C++ `TagLib::Ogg::Opus::File` class), and
that directory is **already on the remoct target's include path** —
[CMakeLists.txt:238](CMakeLists.txt#L238) adds `${TAGLIB_INCLUDE_DIRS}`
(= `${MSYS2_PREFIX}/include/taglib`). Confirmed present on **both** platforms:

- MSYS2: `C:\msys64\ucrt64\include\taglib\opusfile.h`
- Debian: `/usr/include/taglib/opusfile.h`

Meanwhile `opusfile.pc` advertises `Cflags: -I${includedir}/opus`. **If we follow
pkg-config's Cflags and write `#include <opusfile.h>`, which header wins depends
on `-I` order** — and `LocalFileSource.cpp` includes TagLib headers in the same
TU as the decoder registration. A C++ tag-reader class silently substituted for
the C codec API is a confusing compile failure at best.

**Decision:** always `#include <opus/opusfile.h>` with the explicit `opus/`
prefix, and do **not** add opusfile's pkg-config `Cflags` include dir. This
mirrors the existing `#include <fdk-aac/aacdecoder_lib.h>` convention exactly
([AacDecoder.cpp:13](src/AacDecoder.cpp#L13)) and makes the collision
unreachable. Same for `<wavpack/wavpack.h>`.

This is also the tiebreaker for §6's find_library-vs-pkg-config choice.

### 5.2 WASAPI first-init warm-up — already covers `.opus`/`.wv`, no change

**Confirmed format-agnostic.** [AudioManager.cpp:170-204](src/AudioManager.cpp#L170-L204)
is gated on a function-local `static bool audio_backend_warmed`, uses a
hardcoded f32/2/44100 throwaway device, and runs on the first `initDevice()` of
the process regardless of the file. Nothing about the file's codec reaches it —
no parameter, no extension check, no `file_src_` inspection. The code comment
states the intent outright: *"it's the first backend init that's fragile, not the
AAC file"*.

**Reasoning:** the bug is that the first WASAPI bring-up in a process is fragile
when it coincides with a *custom* (non-built-in) `ma_decoder` backend. `.opus`
and `.wv` are custom backends, i.e. the same class of trigger as `.aac` — and the
warm-up already neutralises it for the whole class by making the first bring-up
coincide with no decoder at all. **No change needed; already covered.** Test in §7.2
verifies rather than assumes.

(Noted in passing, pre-existing, out of scope: the warm-up sets `wc.dataCallback`
with `pUserData = this`, and its comment claims safety because `file_src_` is
null. On the first `play()` that premise is false — `file_src_` is assigned at
[AudioManager.cpp:150](src/AudioManager.cpp#L150), *before* `initDevice()` at
:154 — so the warm-up callback can consume a few frames from the real decoder.
Also the outer comment says "init/uninit (no start) is enough … the callback
never fires" while the code does `start`/`stop`; that comment is stale.)

### 5.3 Summary of hazard decisions

| hazard | decision |
|---|---|
| Opus RG offset | `q78/256.0 + 5.0`. `/256` fixes today's silence bug; `+5` re-bases −23 → −18 LUFS. No header-gain term (libopusfile applies it; RFC 7845 defines the tag relative to post-header-gain). |
| WavPack bit depth | Backend reports **f32**, not s16 — justified deviation from the AAC template (§4.2). Converter accepts >16-bit and non-44.1 kHz by conversion, not rejection. |
| `.wvc` correction file | **Deferred** (as recommended). Memory reader with `wvc_id = NULL` defers it structurally. Hybrid `.wv` still plays, at lossy quality. |
| `.ogg` disambiguation / vtable order | **Moot** — no built-in Vorbis exists (§1.2). Sniffs are disjoint on 4-byte magic; order irrelevant. Opus-vs-Vorbis discrimination delegated to `op_open_memory`. |
| BPM decoder scope | **In scope** — register all three (§4.6). |
| WASAPI warm-up | **No change** — already format-agnostic (§5.2). |
| TagLib `opusfile.h` collision | **NEW** — mandatory `opus/` include prefix (§5.1). |

---

## 6. CMake — exact edit shape

**pkg-config vs find_library: use `find_library` + `find_path`.** Three reasons:
the FDK-AAC/FLAC/LAME/ebur128 blocks all do
([CMakeLists.txt:160-194](CMakeLists.txt#L160-L194)) and `PATHS "${MSYS2_PREFIX}/lib"`
falls through to system paths on Linux (already proven in Phase 3); the layouts
are identical across platforms so pkg-config buys nothing (§1.4); and pkg-config's
`Cflags: -I${includedir}/opus` would actively *arm* the TagLib collision (§5.1).
TagLib's pkg-config usage is the exception, and it exists because TagLib needs it.

**Dependency availability — verified on both toolchains:**

| | MSYS2 UCRT64 (7of9) | Debian Trixie (WSL2) |
|---|---|---|
| opus | **installed** 1.6.1-1 | not installed, candidate `1.5.2-2` |
| opusfile | **installed** 0.12-4 | not installed, candidate `0.12-4+b3` |
| wavpack | **installed** 5.9.0-1 | not installed, candidate `5.8.1-1` |

Windows needs **no new package installs**. Linux/CI needs
`apt-get install libopus-dev libopusfile-dev libwavpack-dev` — which also means
**`.github/` CI needs its Debian install list extended**, or the Linux job breaks
at configure on the `REQUIRED` finds. (wavpack 5.9.0 vs 5.8.1 is a harmless minor
skew; every API used exists in both.)

**Insert after [CMakeLists.txt:194](CMakeLists.txt#L194)** (the `fdk-aac` message):

```cmake
# ── libopus + libopusfile (Opus decode: .opus) ─────────────────────────────
find_library(OPUS_LIB      NAMES opus     PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_library(OPUSFILE_LIB  NAMES opusfile PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
# Nested exactly like fdk-aac/, and identical on MSYS2 + Debian. The opus/
# prefix is mandatory at the include site: TagLib ships its own opusfile.h and
# its include dir is already on this target's path.
find_path(OPUS_INCLUDE NAMES opus/opusfile.h PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "opusfile: ${OPUSFILE_LIB}")

# ── libwavpack (WavPack decode: .wv) ───────────────────────────────────────
find_library(WAVPACK_LIB NAMES wavpack PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_path(WAVPACK_INCLUDE NAMES wavpack/wavpack.h PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "wavpack: ${WAVPACK_LIB}")

target_include_directories(remoct PRIVATE ${OPUS_INCLUDE} ${WAVPACK_INCLUDE})
```

**Why a second `target_include_directories` call rather than extending the one at
[:128-135](CMakeLists.txt#L128-L135):** that block already interpolates
`${FDKAAC_INCLUDE}` at **:134 — 50 lines before `find_path` sets it at :184**.
CMake evaluates top-to-bottom, so that entry expands to empty today. It works
only because `${MSYS2_PREFIX}/include` is added at :137 anyway. Adding our vars
there would silently inherit the same latent bug. Placing the call *after* the
finds is correct and additive. **The pre-existing FDKAAC ordering bug is left
alone** — flagging, not fixing, in a decode slice.

**Sources** — after [CMakeLists.txt:66](CMakeLists.txt#L66):

```cmake
    src/OpusDecoder.cpp           # libopus/libopusfile (.opus) backend
    src/WavPackDecoder.cpp        # libwavpack (.wv) backend
```

**Link** — after [CMakeLists.txt:258](CMakeLists.txt#L258) (`${FDKAAC_LIB}`):

```cmake
    ${OPUSFILE_LIB}   # must precede ${OPUS_LIB}: opusfile depends on opus
    ${OPUS_LIB}
    ${WAVPACK_LIB}
```

**Static probe (`-DREMOCT_STATIC_PROBE=ON`) — already satisfied, one line to verify.**
Static libopusfile needs libogg explicitly, and
[CMakeLists.txt:288](CMakeLists.txt#L288) *already* links
`"${MSYS2_PREFIX}/lib/libogg.a"` (added for static libFLAC). The static probe is
therefore covered for free; worth an explicit check rather than an assumption.
Unlike fdk-aac, opus/opusfile/wavpack are **not** shared with `remoct_stream`, so
they need no dynamic-by-design carve-out ([:186-193](CMakeLists.txt#L186-L193)).

**`plugins/stream/` — untouched.** It links its own FDK-AAC + miniaudio for the
radio path and has zero custom-backend registrations. Opus/WavPack are file-decode
only; the plugin does not need them.

**`tests/CMakeLists.txt` — MUST change or the build breaks.** `xfade_handoff_test`
(Windows-only) links `src/LocalFileSource.cpp` **and** `src/AacDecoder.cpp`. Once
`open_decoder` references `ma_opus_backend_vtable()`/`ma_wavpack_backend_vtable()`,
that target fails to **link** unless it also gets the two new `.cpp`s plus
`${OPUSFILE_LIB} ${OPUS_LIB} ${WAVPACK_LIB}`. This is the one non-obvious
build-breaking edit in the slice; nothing else in `tests/` links `LocalFileSource.cpp`.

---

## 7. Proposed verification (to run on greenlight, not now)

### 7.1 Decode
Known-good `.opus` and `.wv` play end-to-end; seek lands correctly; duration and
the `[OPUS]`/`[WV]` labels correct. WavPack matrix: 16-bit, **24-bit**, and a
non-44.1 kHz file (the conversion-not-rejection claim, §4.2). One hybrid `.wv`
*with its `.wvc` deliberately absent* → must still play (lossy), proving the
defer is graceful rather than a crash.

### 7.2 First-file / WASAPI
`.opus` as the very first file of a fresh session; `.wv` as the very first file of
another fresh session. Both must play with no chirp-then-silence. This tests §5.2's
"already covered" reasoning against reality — it is the claim most worth being
wrong about, since it is the one where the plan changes nothing.

### 7.3 ReplayGain (validates the R128 offset)
A tagged Opus file's applied gain matches a reference within tolerance. Concretely:
- With RG **off**, and with RG **on**, on the same Opus file — RG-on must **not**
  be silent. That is the direct regression test for the §1.3 silence bug.
- Compare applied gain against `R128_TRACK_GAIN/256 + 5` computed by hand from the
  tag; and cross-check loudness against a FLAC of the same master with
  `REPLAYGAIN_TRACK_GAIN` — they should land within ~1 dB. This is what actually
  validates `+5` rather than `+6`/`0`.
- A `.wv` with APEv2 `REPLAYGAIN_TRACK_GAIN` → unchanged generic path.

### 7.4 Regression
FLAC / MP3 / AAC / `.m4a` / `.m4b` playback unchanged (proves the 3-entry vtable
array shadows nothing and the sniffs fail fast). Both platforms: Windows ctest
20/20, Linux 21/21 green — including `xfade_handoff_test` after the §6 test edit.

**`.ogg` Vorbis is deliberately NOT a regression check** — it does not decode today
(§1.2), so "still decodes" cannot pass. The honest check is that `.ogg` behaviour
is *unchanged* (still fails to open). Recording this so a red `.ogg` result is not
later misread as caused by this slice.

### 7.5 Gate note
**The Joan Osborne regression gate is NOT invoked for this slice.** Justification:
the slice touches no CD-path code. `CDRipper.*`, `CDSource.*`, `ar_crc.*`,
`include/core/ICdIo.h` and both `CdIo*` transports are untouched; the ripper's
AccurateRip CRC path is not reachable from any file changed here. The changed set
is `LocalFileSource.cpp` (decode registration + RG read), `AudioManager.cpp` (BPM
decoder registration), `PlaylistManager.cpp` + `UIManager.cpp` (two extension
literals), `CMakeLists.txt` + `tests/CMakeLists.txt`, and the four new decoder
files. None is CD-adjacent, so the CD gate has nothing to protect against here.

Linux smoke: WSL2 Trixie is available and the repo mounts at `/mnt/e/code/remoct`,
so the Linux build + `.opus`/`.wv` decode can be smoke-tested there once
`libopus-dev libopusfile-dev libwavpack-dev` are installed.

---

## 8. Change inventory (on greenlight)

| file | change |
|---|---|
| `include/OpusDecoder.h` | **new** |
| `src/OpusDecoder.cpp` | **new** |
| `include/WavPackDecoder.h` | **new** |
| `src/WavPackDecoder.cpp` | **new** |
| `src/LocalFileSource.cpp` | 2 includes; vtable array → 3; Opus R128 RG branch |
| `src/AudioManager.cpp` | 2 includes; `detectBpm` vtable array → 3 |
| `src/PlaylistManager.cpp` | `+".wv"` in `AUDIO_EXTS` |
| `src/UIManager.cpp` | `+"WV"` in `fileTypeTag::known[]` |
| `CMakeLists.txt` | dep block, 2 sources, 3 link entries, 1 include call |
| `tests/CMakeLists.txt` | `xfade_handoff_test`: +2 sources, +3 libs (**link-breaking if missed**) |
| `.github/` CI | Debian job: +3 `-dev` packages (**configure-breaking if missed**) |

Audio thread untouched. No new threading. Both backends are pull-model
`ma_data_source` on the existing decode path. No CD-path file touched.
