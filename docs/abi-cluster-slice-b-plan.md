# ABI cluster slice B: copy/remux — PLAN

**Stage:** PLAN deliverable. Anchors resolved, shapes proposed. No source,
no commit. The last backlog item — this review doubles as the
"are we actually done" checkpoint.
**Baseline probed:** `experimental/win-pdcurses` @ `254baca` (slice A: the
ABI is open; three fns declared-NULL awaiting this slice).

## 1. The AAC container decision (§2.1) — RESOLVED: M4A, decisively

Tree facts that settle it:

- **`src/AacDecoder.cpp` already decodes BOTH raw ADTS and AAC-in-MP4**
  (its own header comment; ADTS frame-header walker at :185 building a
  per-frame sample table; MP4 box parsing — findBox/moov/stsd/esds — at
  :99-:185). So Dos's decision #2 (in-app playback) is satisfied by
  EXISTING machinery for either container — the design doc's ".aac decode
  is a later slice" fear was stale; the decoder was already built for the
  audiobook suite.
- **Tagging is the discriminator.** TagLib has no home for bare ADTS —
  decision #1 (no untagged backlog) is unsolvable for `.aac` without
  inventing a tag convention. TagLib::MP4 tagging is library-supported
  (the app already READS .m4a/.m4b tags through FileRef), and the
  recorder's tag pass gains a small MP4 branch (title/artist/comment +
  `covr` art — RG excluded on copy by design).

| | mux/write | tags (#1) | in-app play (#2) | new surface |
|---|---|---|---|---|
| **M4A** | hand-rolled ADTS→MP4 writer | TagLib MP4 (small branch) | EXISTS (AacDecoder) | **one piece: the mux writer** |
| bare .aac | trivial append | **NO HOME — fails #1** | exists | a tag convention we'd invent |

**Recommendation: M4A.** The mux writer (~300-400 lines: `ftyp` + `mdat`
of de-ADTS'd access units + `moov` with mvhd/trak/mdia/stbl —
stsd(mp4a+esds from the ADTS header's AudioSpecificConfig fields),
stts (constant 1024/sr), stsz per-frame, stco) is bounded, deterministic,
and **round-trip verifiable in-tree**: the fixture decodes the produced
.m4a through RE-MOCT's own AacDecoder and TagLib reads its tags — the
writer's correctness oracle already exists. MP3 copy is unaffected either
way: raw frame append, existing ID3 tag path, existing decode.

## 2. Discovery findings (§3)

1. **The three NULL slots** — plugins/stream/StreamPluginAdapter.cpp
   descriptor tail (slice A left `encoded_caps` / `set_encoded_capture` /
   `read_encoded` NULL with the comment naming this slice). They wire to
   new StreamSource methods over a tee ring member.
2. **Compressed-byte ingress (the tee taps):**
   - Continuous (ICY, MP3 or AAC): the post-metaint byte layer —
     `rawRead`/`readAudio` (StreamSource.cpp:106 comment: "rawRead —
     de-interleave, ring feed, producers — is shared") — ONE tap serves
     both continuous codecs, after metadata stripping, before any decoder.
   - HLS: the segment payload after fetch, **after the existing timed-ID3
     parse point** (§3.6: the plugin already parses ID3-in-segment for
     now-playing — the tee taps post-strip, so transport metadata never
     reaches the muxer). The discontinuity bit sets on the re-pin/reconnect
     paths (`hls_repin_pending_` sites — the tee marks, the muxer resyncs
     on the next frame header).
   - **Codec for `encoded_caps`**: the plugin already discriminates at
     connect (FDK/AAC worker vs the MP3 continuous decoder; HLS = AAC) —
     the fn reports what connect decided, 0 before connect.
3. **MP4 machinery** (per §1): read side exists (AacDecoder + TagLib
   FileRef); write side = the new muxer + a ~30-line TagLib::MP4 branch in
   the recorder tag pass.
4. **The recorder's copy branch — the shape that keeps it testable:**
   the recorder stays plugin-ignorant. `RecOptions` gains
   `copy_mode` + an injected pull callback
   (`uint32_t pull(uint8_t*, cap, int32_t* codec, int32_t* discont)`);
   **the worker IS the pump** — in copy mode it polls the callback
   (~50 ms) instead of draining the PCM ring, frame-parses, and feeds a
   MuxWriter (MP3 append / M4A mux) per cut. AudioManager::beginRecording
   wires the callback to `stream_plugin_.readEncoded` and arms
   `set_encoded_capture(1)`; endRecording disarms. The PCM tap stays
   un-armed in copy mode (mutually exclusive taps — hazard 5.7: one
   lifecycle, one mode flag, both tees can never arm together because the
   same wrapper arms exactly one). Keep-draining composes: the same
   `set_record_active` signal keeps the producers running through pauses,
   and the tee sits on the producer side — gapless copy through a pause
   comes free.
5. **Split/trim surface in copy mode (§2.3):** the worker counts parsed
   frame DURATIONS (1152/sr MP3, 1024/sr ADTS — sr from the frame header,
   not assumed 44100). The trim hold arms in ms exactly as today and
   counts down in encoded-frame duration units; splits snap to whole
   frames (~23-26 ms granularity, noise under the slop). Ad-aware is
   title-string classification upstream of the writer — routing and
   suppression compose UNCHANGED (a suppressed copy cut = frames pulled
   and dropped, counted). No RG on copy — stated in panel + CHANGELOG;
   re-encode remains the RG-bearing mode.
6. **Frame parser (§2.2):** no host-side parser exists; AacDecoder's ADTS
   walker is in-tree reference. New tiny shared header (`FrameSync.h`):
   `mp3FrameLen(hdr)` / `adtsFrameLen(hdr)` + samples-per-frame + sr
   extraction + the ID3v2-block skipper (the syncsafe logic from the
   mp3-rg-write scratch tool, now with a home) — used by the splitter and
   both writers, unit-tested directly.

## 3. The tee ring (§2.4, Dos's #3 locked)

Plugin-side 2 MiB SPSC byte ring (the recorder-ring discipline applied to
bytes): producers `memcpy` in at their existing ingress points when armed
(one relaxed-atomic check when disarmed — zero cost; the producers are
NETWORK threads, never the audio callback, so even the armed cost sits off
the sacred path). Overflow = **drop-oldest whole region + a counter**
`read_encoded` surfaces (the host shows it like dropped-frames; drop-oldest
is correct here because the reader wants the LIVE tail, and the muxer
resyncs by frame header after any gap — same machinery as the discont
bit). `read_encoded` = worker-thread pull, host buffer, nobody frees
across the line.

## 4. Panel model (§2.5)

Third radio entry: `( ) 3  Copy    as broadcast (no re-encode)` — quality
column shows the live codec from `encoded_caps` ("MP3 320k" / "AAC");
greyed + dim note when `read_encoded` is NULL (old plugin) or caps report
none. `rec_format = copy` joins the config values. Single-select enforces
tap exclusivity at the UI; the beginRecording wrapper enforces it
structurally (§2.4).

## 5. Verification (on greenlight)

- **Byte-fidelity (the copy claim, machine):** stream_record_test drives
  the copy path via the injected pull callback with synthetic MP3 and ADTS
  frame sequences (no network): every emitted cut's frames are a
  byte-subset of the pushed stream; splits land exactly on frame
  boundaries; the M4A cut round-trips through AacDecoder (frame-exact
  sample count) and TagLib reads its tags; trim hold defers by
  frame-duration exactly; Discard suppresses with the counter; tee
  overflow (tiny ring fixture) drops oldest + counts, cut stays valid,
  discont resync produces a clean container.
- **Plugin tee (machine):** hls_pipeline_test extension —
  `set_encoded_capture(1)` → `read_encoded` returns the fixture's exact
  ADTS bytes (byte-compare against the fixture source), discont fires
  across the stall/self-heal boundary, codec reports AAC_ADTS; disarmed =
  zero reads. Compat: the slice-A v1-fixture cell extends (read_encoded
  NULL → Copy unavailable, no crash).
- **Live (both paths):** ICY (KKJO) and iHeart HLS (zc4366) copy captures
  → tagged, playable-in-RE-MOCT cuts (the M4A ones through the app's own
  AacDecoder); a re-pin mid-cut resyncs cleanly. Keep-draining + copy:
  pause mid-copy → gapless (the slice-A gate re-run in copy mode).
- Loaded-module parity re-run (plugin changed — slice-c/d discipline);
  ctest both toolchains; CHANGELOG (zero-loss capture; hyphens).
- **Dos's seals:** copied AAC cut visible-tagged in RE-MOCT and an
  external player; the copy-vs-re-encode A/B listen (the fidelity point).

## 6. Divergences / notes

1. **The design doc's AAC fears were stale** — ADTS decode already exists
   (the audiobook suite built it); "untagged AAC v1" and "bare-.aac
   playback later" both dissolve; M4A satisfies both locked decisions with
   ONE new piece.
2. The recorder's pull-callback shape keeps CDRipper/plugin linkage out of
   tests (the standing norm) and makes the whole copy path headless.
3. Copy + keep-draining compose for free (tee is producer-side).
4. sr on copy is the BROADCAST's (44.1k or 48k as aired) — cuts are not
   resampled (that's the point); stated in CHANGELOG.
