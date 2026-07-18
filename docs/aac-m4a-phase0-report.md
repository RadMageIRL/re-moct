# AAC/M4A rip — Phase 0 feasibility report

Scope ID: `aac-m4a-phase0-probe`. Stage: probe + plan only (no production code, no
seam, no `RipFormat` change, not committed). Branch `experimental/win-pdcurses`.
Probe retained under `probes/aac-m4a/`, out of the production build.

**Verdict: both unknowns cleared on both toolchains. AAC/M4A rip is feasible with
NO new dependency and NO new DLL. Recommend proceeding to a Phase 1 encoder slice.**
Probe result: `24 passed, 0 failed` on Windows/UCRT64 and Linux/Debian (WSL2).

---

## Probe 1 — FDK-AAC encoder availability

**Present and linkable on both toolchains, against the SAME libfdk-aac already
shipped. No new lib, no new DLL.**

| | Windows / UCRT64 | Linux / Debian |
|---|---|---|
| `fdk-aac/aacenc_lib.h` | present (87991 B) | present (byte-identical, 87991 B) |
| encoder symbols | `aacEncOpen/Encode/Info/Close/aacEncoder_SetParam` in `libfdk-aac.a` **and exported from `libfdk-aac-2.dll`** | same symbols exported from `libfdk-aac.so.2` (`libfdk-aac-dev` 2.0.3) |
| new runtime dep | none — the exe already ships `libfdk-aac-2.dll` for decode | none |

- **AAC-LC** (`AACENC_AOT = 2`): encodes and decodes cleanly.
- **VBR** (`AACENC_BITRATEMODE = 1..5`): the param is accepted and produces valid
  output. At level 4 on the probe's pure-sine signal it lands ~14 kbps because a
  sine is trivially compressible — on real music VBR 4 is ~128 kbps. The point
  proven is that the VBR ladder *applies*.
- **CBR** (`AACENC_BITRATEMODE = 0` + `AACENC_BITRATE`): 128000 → measured 128104 bps
  by an independent demuxer. Applies.
- The encoder emits the `AudioSpecificConfig` via `aacEncInfo().confBuf` (2 bytes for
  LC/44100/stereo) — that is exactly what the MP4 `esds` needs.

**Divergence from the brief (favorable):** the brief said RE-MOCT "uses only the
DECODER side." True for production `src/`, but `tests/hls_fixture.h` **already drives
the FDK encoder** (`aacEncOpen`, `AACENC_AOT=2`, `AACENC_BITRATE`, `aacEncEncode`) and
compiles green on both toolchains in CI today. So a proven-linking encoder reference
already lives in the tree — Probe 1 was de-risked before it started. The probe's
encoder loop is modeled on that fixture, switched to `TT_MP4_RAW`.

---

## Probe 2 — the MP4 mux (the real unknown)

**Chosen path: a hand-rolled MP4 box writer (`probes/aac-m4a/Mp4Mux.h`). No new
dependency, no new DLL.** This is the reusable Phase 1 artifact.

### Why this path
- **libmp4v2**: absent on *both* sysroots. Adding it = a new build dep **and a new
  runtime DLL** — which would destroy the entire selling point of AAC ("FDK is
  already linked, nothing new to ship"). Rejected.
- **TagLib MP4**: writes tags/atoms to an *existing* MP4 but cannot create the audio
  container from raw frames. It is the tag/art layer, not the mux. (It is exactly
  right for that layer — see below.)
- **Hand-rolled**: mirrors the box-reader patterns already in `AacDecoder.cpp` /
  `Mp4Chapters.cpp`. Pure byte assembly, zero dependencies. ~260-line header.

### What it writes
Exactly and only the boxes RE-MOCT's own decoder reads
(`AacDecoder.cpp` `Mp4Aac::parse`), single audio track, single chunk:
```
ftyp
moov → mvhd, trak → tkhd, mdia → { mdhd, hdlr, minf → { smhd, dinf/dref,
        stbl → { stsd→mp4a→esds(ASC), stts, stsc, stsz, stco } } }
mdat  [access units concatenated]
```
- **Single-chunk layout** makes the sample table trivially correct: one `stsc` run,
  one `stco` offset (patched to the mdat payload after layout), non-uniform `stsz`
  from per-AU sizes, one uniform `stts` entry (`frameLength` = 1024). RE-MOCT's seek
  is `frameIndex / 1024 → sample index`, so this makes scrubbing exact.
- `mdhd`/`mvhd` timescale = sample rate; duration = `nAU * 1024`.
- `esds` carries the `aacEncInfo` ASC in a standard ES/DecoderConfig/DecSpecificInfo
  descriptor chain; `mp4a` is the 28-byte AudioSampleEntry the decoder expects before
  the `esds` child.
- **moov is written before mdat** (streamable/progressive). The rip encodes a whole
  track, so all AU sizes are known at finalize — no second pass, no offset rewrite
  beyond the one `stco` patch.

### Verification (both toolchains)
Driven through the **real production decode path** — `AacDecoder` registered as a
miniaudio custom backend, opened with `ma_decoder` exactly as playback does:

| Check | Result |
|---|---|
| RE-MOCT `AacDecoder` opens the muxed file | PASS (VBR + CBR) |
| Reported length matches encoded duration | PASS (6.060 s, exact) |
| Decoded audio non-silent, format 44100/2ch | PASS |
| **Seek 25/50/75% tracks the 440/880/1760 Hz thirds** | PASS (exact) — sample table valid |
| TagLib writes title/artist/album + `covr` (PNG) | PASS |
| Tags + `covr` read back | PASS |
| Decoder still opens + seeks **after** tagging | PASS — TagLib preserves mdat/stco |

**Independent cross-check (FFmpeg):** ffprobe recognizes the container
(`mov,mp4,m4a`), the audio stream (`aac`, 44100, 2ch, 128104 bps), reads back
artist/album/title, and sees the `covr`. Audio-only decode exits 0 (clean). The one
ffprobe warning (`png inflate error`) is the probe's synthetic 72-byte 1×1 PNG
fixture having a malformed zlib stream — not a container fault; Phase 1 art is real
JPEG/PNG from the CoverArt module.

Verification item (b) from the brief — "plays + shows tags/art in an external
player" as a human eyes-on — remains Dos's, per the standing workflow. The
`out_vbr4.m4a` / `out_cbr128.m4a` the probe writes are there for that look.

---

## Answers to the five lock questions

1. **FDK encoder present + linkable, same lib/DLL, no new dep?** Yes, both toolchains.
2. **VBR (1-5) and CBR both applying via `AACENC_BITRATEMODE`?** Yes, both.
3. **Which mux path, its cost, does it add a dep?** Hand-rolled `Mp4Mux.h`, ~260-line
   header, **no dependency, no DLL**. TagLib (already linked) is the tag/art layer.
4. **Plays + tags/art read + seeks correctly, both platforms?** Yes — verified through
   RE-MOCT's own decoder and corroborated by FFmpeg. Seek is exact.
5. **Live-tree divergences:** (a) the FDK encoder is already exercised in-tree by
   `tests/hls_fixture.h` (favorable). (b) An LP64/LLP64 pointer trap surfaced in probe
   code — `uint64_t*` is not `ma_uint64*` on Linux (LP64 `long` vs LLP64 `long long`);
   pass a local `ma_uint64` to `ma_decoder_get_length_in_pcm_frames`. Same family as
   the tick-math trap in the env lessons; Phase 1 code that touches miniaudio 64-bit
   out-params must use `ma_uint64` locals.

---

## Recommended Phase 1 shape (for the implement brief)

Nothing below is built yet — this is the design the probe unlocks.

- **`RipFormat::M4a`** appended to `kRipFormats` as **row 6 / digit key 6**
  (append-only per `RipFormats.h`; stable). `ext ".m4a"`, `lossless = false`,
  `taggable = true` (so the existing tag + R128/ReplayGain pass runs on it).
- **`M4aEncoder`** implementing the existing `IEncoder` seam, but note it is a
  **finalize-time mux**, not a streaming-append encoder like FLAC/MP3: it accumulates
  FDK access units (`TT_MP4_RAW`) + the ASC during the rip, and `finalize()` assembles
  the boxes via `Mp4Mux.h` and does the single file write, returning `bool` (the
  3-layer ENOSPC laundering lesson applies to that write). A CD track's worth of AAC
  is a few MB in memory — the same order as `AacDecoder` already slurps per file.
  Add it to `EncoderFactory.cpp` (the un-static'd factory from the convert-core slice).
- **Quality UI** mirrors the encoder-bitrate-mode slice: extend `EncoderQuality.h` +
  `RipOptions`/`RecOptions` with `aac_vbr` (default true), `aac_vbr_mode` (1-5,
  default **4**), `aac_cbr_bitrate` (default 128000); reuse the `>`/Left-Right/`[M]`
  per-row editor and the CBR/VBR toggle already shipped for MP3/Opus.
- **Tags/art** reuse `copyTextTags` + `ArtEmbed::embedArt` (its MP4 `covr` writer
  already exists) in the post-encode tagging pass — the same path convert uses.
  Phase-1 verify (not a Phase-0 blocker): confirm the rip R128/ReplayGain write lands
  MP4 `----` atoms on the `.m4a` (TagLib PropertyMap should carry it, as proven in the
  art-embed slice).
- **Fits the "one verified read" rip invariant unchanged:** M4a is just another
  `IEncoder` consuming the same decoded PCM as FLAC/MP3/Opus/WavPack.

### Deferred / out of scope (unchanged from the brief)
HE-AAC, raw `.aac` target, ALAC, a profile knob (AAC-LC only). One caveat to record
for a later slice: FDK adds ~2048 samples of encoder priming delay and the current
`AacDecoder` ignores edit lists, so exact gapless start (iTunSMPB / `elst`) is a
possible future refinement — inaudible for normal track playback and not needed for v1.

---

## Files
- `probes/aac-m4a/Mp4Mux.h` — reusable hand-rolled MP4 writer (the Phase 1 artifact)
- `probes/aac-m4a/aac_m4a_probe.cpp` — encode → mux → verify → tag/art driver
- `probes/aac-m4a/ma_impl.cpp`, `build.sh`, `README.md`
