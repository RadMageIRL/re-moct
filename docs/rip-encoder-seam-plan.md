# Slice `rip-encoder-seam` — IEncoder seam under the rip tail (PLAN)

**Stage:** PLAN. No source written, nothing committed.
**Baseline:** `experimental/win-pdcurses` at `4002f9b` (post opus/wavpack/vorbis).
**Position:** slice 1 of the rip multi-format overhaul. Seam only, zero
user-visible change, output byte-identical.
**Acceptance criterion:** the Joan Osborne gate — Relish 12/12 AR v2,
confidence 200, byte-identical CRCs, real hardware, before and after.

Every anchor below was resolved against the live tree and spot-verified
verbatim. §1 lists where the brief's assumptions diverge from the code — all
four divergences make the slice *smaller*, but two of them reshape the
interface, so read §1 before §3.

---

## 1. Divergence from the brief (READ FIRST)

### 1.1 There are no rip config keys — every encoder setting is hardcoded

Brief §5 asks where `flac_level` / MP3 V0 settings "enter the encoders from
config today". They don't. `DigiConfig` has zero rip-output fields; grep for
flac/mp3/vbr in Config.{h,cpp} is empty. The settings are compile-time
constants in CDRipper.cpp: `FLAC_COMPRESSION = 5` at
[src/CDRipper.cpp:63](src/CDRipper.cpp#L63) (applied :790), and LAME hardcoded at
[:810-817](src/CDRipper.cpp#L810-L817) (`vbr_mtrh`, `VBR_q 0`, `quality 2`,
`STEREO`, manual Xing tag). Consequence: the seam carries **no settings
plumbing at all** — each encoder class bakes in today's literals verbatim.
(The slice-2 note's "quality display-only from config: flac_level, mp3,
opus_bitrate, wavpack_mode" describes keys that will be *created* in slice 2 —
none exist yet.)

### 1.2 Tags and R128 are NOT written by the encoders — the brief's `open(path, meta, r128)` shape doesn't match the tail

Brief §4 proposes `open()` taking `TrackMeta` + `R128Result` and tags moving
"behind finalize()". The real sequencing forbids that, for a good reason:

- Tagging happens in a **post-loop pass at the worker level**, after ALL
  tracks are encoded — [src/CDRipper.cpp:1953-1954](src/CDRipper.cpp#L1953-L1954)
  calls the existing format-dispatched `tagFile()` once per output file.
- It must, because the tags carry **album gain**, which only exists after the
  last track: per-track `ebur128_state`s are handed back to the worker
  ([:1207-1210](src/CDRipper.cpp#L1207-L1210)) and combined with
  `ebur128_loudness_global_multiple` at [:1917](src/CDRipper.cpp#L1917) —
  true R128 album gating, not a mean of track gains.

So tags/art/R128 **stay exactly where they are** (`tagFile` untouched — it is
already a per-format seam keyed on path extension, TagLib both formats, one
shared `rg` struct, shared formatter lambdas at :676-683, same art bytes).
The IEncoder interface shrinks to pure PCM-to-file: open / writeFrames /
finalize. This is also the strongest possible byte-identity move: the entire
tagging code path has a diff of zero.

### 1.3 There is no whole-track PCM buffer — the tail is a streaming fan-out already

Brief §2/§4 speak of "one canonical PCM buffer" consumed by N encoders. Today
the tail streams **27-sector blocks** (`raw_buf`, 63,504 bytes, lifetime = one
loop iteration, [:905](src/CDRipper.cpp#L905)) and fans each block out to FLAC
→ LAME → ebur128 → AR/CTDB CRC *within one loop iteration*
([:1037-1080](src/CDRipper.cpp#L1037-L1080)). FLAC and MP3 are already
chunk-interleaved consumers of identical in-memory PCM; MP3 re-reads nothing.

Consequence: the restructure is **naming what exists**, not building a new
buffer. The seam's unit of work is the per-block `(const int16_t* interleaved,
size_t frames)` slice of `raw_buf` — introducing a whole-track buffer would
*change* the memory profile and flush cadence for zero benefit and real
byte-identity risk. The plan keeps streaming.

### 1.4 Smaller notes

- **No fsync exists** (brief §4 "fsync as today" = nothing; grep 0 hits).
  `finalize` = library finish/flush + `fclose`, best-effort, return codes
  unchecked — preserved as-is.
- **No `FLAC/`/`MP3/` subdirectory split** — outputs are flat, side-by-side in
  one album dir, differing by extension ([:1509-1510](src/CDRipper.cpp#L1509-L1510)).
- **MP3 is unconditional** — no config flag gates it anywhere; both files are
  always written. The hardcoded `{FLAC, MP3}` list in this slice is therefore
  a faithful transliteration, not a behavior choice.
- **No test links CDRipper.cpp** (confirmed; only `CMakeLists.txt:55`), so
  there is no xfade-style test-target fallout. But see §6 — the seam *creates*
  the first opportunity to put the encode tail under CI, and the plan takes it.

---

## 2. Resolved anchors (all spot-verified)

| what | where |
|---|---|
| The write tail (everything) | `CDRipper::ripTrack`, [src/CDRipper.cpp:755-1265](src/CDRipper.cpp#L755) |
| FLAC setup (new/settings/fopen/init_FILE) | [:787-801](src/CDRipper.cpp#L787-L801) |
| LAME setup (init/settings/init_params/fopen) | [:804-830](src/CDRipper.cpp#L804-L830) |
| Per-block staging buffers | [:905-910](src/CDRipper.cpp#L905-L910) |
| Main read+encode loop | [:994-1126](src/CDRipper.cpp#L994-L1126) |
| The fan-out conversion loop | [:1037-1044](src/CDRipper.cpp#L1037-L1044) |
| FLAC process (main / tail) | [:1067](src/CDRipper.cpp#L1067) / [:1159](src/CDRipper.cpp#L1159) |
| LAME encode+fwrite (main / tail) | [:1071-1074](src/CDRipper.cpp#L1071-L1074) / [:1162-1165](src/CDRipper.cpp#L1162-L1165) |
| Offset-correction tail block | [:1133-1169](src/CDRipper.cpp#L1133-L1169) |
| FLAC finish/delete; LAME flush/Xing/close | [:1172-1187](src/CDRipper.cpp#L1172-L1187) |
| R128: init / add_frames / track result / handoff | [:832-834](src/CDRipper.cpp#L832-L834) / :1079, :1167 / [:1190-1202](src/CDRipper.cpp#L1190-L1202) / :1207-1210 |
| Album gain (multi-state) + copy into RGResults | [:1901-1939](src/CDRipper.cpp#L1901-L1939) |
| Failure cleanup (remove both files) | [:1213-1217](src/CDRipper.cpp#L1213-L1217) |
| Worker path build + ripTrack call | [:1504-1527](src/CDRipper.cpp#L1504-L1527) |
| Worker zero-CRC failure heuristic | [:1533-1543](src/CDRipper.cpp#L1533-L1543) |
| Re-rip temp paths: `.tmp` / `.det` / `.probe` | :1740-1741 / :1810-1811 / :1673-1674 |
| tagFile (both formats, TagLib, art, RG strings) | [:646-749](src/CDRipper.cpp#L646-L749), called :1953-1954 |
| Encoder settings constants | [:57-63](src/CDRipper.cpp#L57-L63), :790, :810-817 |
| Worker thread creation (encode = read thread) | [:2242-2244](src/CDRipper.cpp#L2242-L2244) |

**Untouched by this slice:** the read/retry path, AR preamble (:952-992), all
CRC accumulation (AR v1/v2, CTDB, frame450), `ar_crc.*`, the verify/fetch
logic, `tagFile`, R128 computation, `.cue`/`.m3u`/`disc.json`/log writers,
`RipMode` differences, thread model, UI.

---

## 3. The seam — confirmed interface (matched to the tail, not the brief's sketch)

New file `include/IEncoder.h`:

```cpp
// One rip-output format. Pure PCM-to-file: open a target, accept interleaved
// s16 stereo 44.1k blocks (exactly what the CD read loop produces), finalize.
// Tagging/art/ReplayGain happen AFTER encoding, per format, in the existing
// tagFile() pass (album gain only exists once every track is done) — they are
// deliberately NOT part of this interface.
struct IEncoder {
    virtual ~IEncoder() = default;
    // total_frames: FLAC's total_samples_estimate hint; 0 = unknown.
    virtual bool open(const std::string& path, uint64_t total_frames) = 0;
    // Read-only view of the caller's block; valid only during the call.
    virtual bool writeFrames(const int16_t* interleaved, size_t frames) = 0;
    // ok=false skips quality-tail work (LAME flush + Xing rewrite) but still
    // closes handles — mirroring today's :1172-1187 exactly. Best-effort,
    // no return (today's finish/flush return codes are unchecked).
    virtual void finalize(bool ok) = 0;
};
```

Deltas from the brief's sketch, each forced by §1: no `TrackMeta`/`R128Result`
in `open` (§1.2), input type is `const int16_t*` interleaved (§1.3 — the
tail's real PCM, not `int32_t`), `finalize(bool ok)` with no return (§1.4),
`total_frames` added (FLAC sets `total_samples_estimate` at :794 — dropping it
would change the STREAMINFO header bytes).

### Implementations

`src/FlacEncoder.{h,cpp}` — verbatim transliteration of :787-801 / :1067 /
:1172-1173, constants baked (level 5, verify off, 2ch/16bit/44.1k).
`writeFrames` owns the int16→`FLAC__int32` widening loop (today's :1039-1040)
into a member staging buffer, then `process_interleaved`.

`src/Mp3Encoder.{h,cpp}` — verbatim transliteration of :804-830 / :1071-1074 /
:1174-1187, constants baked (vbr_mtrh, V0, q2, STEREO, manual Xing).
`writeFrames` owns the deinterleave into member `left/right` buffers (today's
:1041-1042), then `lame_encode_buffer` + `fwrite`. `finalize(true)` does
flush → `lame_get_lametag_frame` → seek-0 rewrite → `fclose` → `lame_close`;
`finalize(false)` skips to `fclose`/`lame_close` — today's `ok` gate, exactly.

Headers in `include/`, sources in `src/`, matching the decoder-slice file
granularity. Both classes are small, self-contained TUs — which is what makes
§6's fixture test possible.

### ripTrack restructure (the entire behavioral diff)

`ripTrack`'s signature and every caller (worker, Pass-2 `.tmp`, determinism
`.det`, probe `.probe`) stay **unchanged** — it still takes `flac_path` +
`mp3_path` and internally builds the hardcoded list, in today's operation
order:

```cpp
std::vector<std::unique_ptr<IEncoder>> encoders;
encoders.push_back(std::make_unique<FlacEncoder>());   // FLAC first — today's
encoders.push_back(std::make_unique<Mp3Encoder>());    // open order, :787 then :804
```

- **Open phase:** open in list order; on failure, `finalize(false)` the
  already-opened ones and early-return — reproducing today's unwind including
  its quirk (a LAME failure leaves a finished, empty-but-valid `.flac` on
  disk, :818-823; the worker's zero-CRC heuristic at :1533 then catches it).
  Not cleaned up "better" — identical semantics is the bar.
- **Main loop:** the fan-out lines :1039-1042 leave the shared conversion
  loop; :1067-1074 become `for (auto& e : encoders) if (!e->writeFrames(src,
  samples)) { ok=false; break; }` — same per-block chunking, same FLAC-then-
  MP3 order, so each library sees byte-for-byte the same call sequence it
  sees today. `ebur_interleaved` fill, AR/CTDB CRC accumulation, and the
  progress math stay in the loop untouched.
- **Offset-correction tail** (:1133-1169): same `writeFrames` calls on the
  borrowed samples — the duplicated fan-out block collapses into the seam.
- **Finalize:** `for (auto& e : encoders) e->finalize(ok);` — FLAC first, as
  today (:1172 before :1174).
- **Failure cleanup** (:1213-1217) unchanged — worker/ripTrack still remove
  both paths on `!ok || cancel`.

Buffer ownership (brief hazard 5): `writeFrames` receives a read-only view of
`raw_buf` valid for the duration of the call; encoders copy into their own
member staging buffers before returning (as the stack buffers do today). No
encoder retains the pointer. Same-thread, same-iteration — the decode-backend
pin-the-buffer discipline, trivially satisfied.

---

## 4. Byte-identity argument (hazard 1 + 2)

The claim: **every output byte is produced by the same library call sequence
with the same arguments as today.**

1. **Same samples, same order, same chunk sizes.** The read loop, retry
   logic, offset handling, and 27-sector blocking are untouched; the seam is
   invoked at exactly the two points the inline encoders are today (main loop
   + offset tail). FLAC and LAME are deterministic given identical settings,
   identical input, and identical call chunking — all three are preserved.
   (Chunking matters for MP3: `lame_encode_buffer` boundaries at the same
   sample offsets produce the same frame stream; the block boundaries don't
   move because the read loop doesn't.)
2. **Same settings.** Constants transliterated, not reinterpreted — including
   `set_verify(false)`, `total_samples_estimate` (STREAMINFO), and
   `bWriteVbrTag(1)` + the manual Xing rewrite.
3. **Same write syscall pattern.** FLAC writes through its own `init_FILE`
   handle as today; MP3's per-block `fwrite`, flush `fwrite`, and the
   seek-0 Xing rewrite move into `Mp3Encoder` verbatim. No new buffering
   layer, no fsync introduced (§1.4). The interleaved consumption order is
   already today's order — nothing is reordered (hazard 2 answers itself:
   they were never sequential).
4. **Tags/art/R128: diff = zero lines** (§1.2). `tagFile`, the RG string
   formatters, album-gain math, and both call sites are untouched, so
   hazard 3 (R128 serialization) is vacuous — the single computation feeding
   both writers is today's code, not new code.
5. **AR/CTDB bytes:** CRC accumulation reads `src` before/alongside the
   encoders in the same loop and is not moved — the AR submission window and
   log flow are untouched.

## 5. Error/partial-rip semantics (hazard 4) — mapped and reproduced

| failure today | today's behavior | seam behavior |
|---|---|---|
| FLAC new/fopen/init fails (:788-801) | early return, no/empty flac file, `NotQueried`/`NotFound` | `FlacEncoder::open` false → same return before MP3 opens |
| LAME init/params/fopen fails (:804-830) | FLAC finish+delete (valid empty `.flac` LEFT), return | `Mp3Encoder::open` false → `flac->finalize(false)`, same file left |
| `process_interleaved` / `encode_buffer` fails mid-loop | `ok=false; break` → finalize → **both files removed** (:1213) | `writeFrames` false → identical |
| cancel mid-track | same removal path | unchanged |
| finalize errors | unchecked, best-effort | unchanged (`finalize` returns void) |

No new "one format succeeded" state exists: the loop still breaks on the
first encoder failure and the cleanup still removes both files.

## 6. New CI oracle this slice unlocks (proposed, cheap, permanent)

Because `FlacEncoder`/`Mp3Encoder` are light standalone TUs (unlike
CDRipper's), they can be linked into a test for the first time. Proposed
`tests/rip_encoder_seam_test.cpp` (both platforms, links the two encoder TUs
+ FLAC + LAME):

- Generate a fixed deterministic PCM fixture in-memory (seeded, a few
  hundred 588-sample sectors, fed in 27-sector blocks like the real tail).
- **Reference arm:** the test file carries today's inline encode sequence
  copied VERBATIM from CDRipper.cpp (:787-830, :1037-1074, :1172-1187) — the
  frozen pre-refactor oracle.
- **Seam arm:** the same fixture through `FlacEncoder`/`Mp3Encoder`.
- Assert the two `.flac` outputs are byte-identical and the two `.mp3`
  outputs are byte-identical.

This proves transliteration fidelity in CI forever, on both platforms,
without a disc — the differential-oracle habit applied to our own refactor.
It complements, not replaces, the hardware gate.

## 7. Verification to PROPOSE (on greenlight)

1. **Pre-flight determinism baseline (before any edit):** rip Relish twice on
   the same drive, SHA256 every `.flac`/`.mp3` — establishes that run-to-run
   outputs are naturally byte-identical (deterministic encoders + fixed
   drive/offset say yes; measure, don't assume — if tags embed anything
   run-varying, this is where we find out and scope the diff accordingly).
2. **Joan Osborne gate (the acceptance criterion):** post-refactor Relish rip
   on the real GHD3N/Asus — 12/12 AR v2, confidence 200, CRCs byte-identical
   to the pre-refactor log, [A] mode.
3. **Byte-diff:** SHA256 of every output file pre vs post — identical (per
   the §4 argument; the gate proves it).
4. **Seam unit oracle:** `rip_encoder_seam_test` green both toolchains (§6).
5. **Mode regression:** [C], [Y], [B] each on a disc — behavior identical
   ([B]'s `.det` determinism check is itself a free byte-identity witness:
   it re-rips through the seam and CRC-compares against the kept files).
   Error-path spot check: start a rip, cancel mid-track → both partials
   removed, as today.
6. **No-UI proof:** rip panel pixel-identical; no new keys, no new config.
7. **ctest** green both toolchains (no existing test links CDRipper —
   confirmed — so only the new seam test is affected).
8. **Note:** this is the CD-path slice, so the Joan Osborne gate IS invoked —
   the first time since the decode series began, per the brief's thesis.

## 8. Change inventory (on greenlight)

| file | change |
|---|---|
| `include/IEncoder.h` | **new** — the interface (§3) |
| `include/FlacEncoder.h` / `src/FlacEncoder.cpp` | **new** — transliterated FLAC writer |
| `include/Mp3Encoder.h` / `src/Mp3Encoder.cpp` | **new** — transliterated MP3 writer |
| `src/CDRipper.cpp` | `ripTrack` encode tail → seam calls; fan-out loop loses :1039-1042; setup/finalize/tail blocks move out |
| `CMakeLists.txt` | +3 sources (no new deps — FLAC/LAME already found+linked) |
| `tests/CMakeLists.txt` | +`rip_encoder_seam_test` (links the 2 encoder TUs + FLAC/LAME) |
| `tests/rip_encoder_seam_test.cpp` | **new** — the differential oracle (§6) |
| `docs/lessons.md` | one entry: the tail's real shape (streaming fan-out, tags-after-album-gain) |
| `CHANGELOG.md` | nothing user-visible — propose a one-line internal note only, or omit; Dos's call |

No config keys, no UI files, no `ar_crc.*`, no read/verify path, no
`tagFile`, no thread changes, no new deps, no CI package changes.

## 9. Recorded for later slices (unchanged from the brief, not built now)

Slice 2 modal (design locked as specified — noting from §1.1 that its
"quality from config" keys must be *introduced* there, none exist);
slice 3 WAV; slice 4 Opus encode (R128_* dialect — the decode slice's
`/256 + 5` lesson runs in reverse: write `(gain_dB - 5) * 256` rounded, and
the header output-gain field decision); slice 5 WavPack (+hybrid `.wvc`);
log marking of verifiable-master vs derived-copy when selection lands.
