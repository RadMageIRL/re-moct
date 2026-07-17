# Slice `rip-wav-encoder` — WAV rip output (PLAN)

**Stage:** PLAN. No source written, nothing committed.
**Baseline:** `experimental/win-pdcurses` at `b61592b` (post rip-format-select).
**Position:** slice 3 — first encoder-add slice. One `IEncoder`, one table row,
no library, no quality key.

**§0 answer: recommendation taken** — WAV gets `*` (lossless is the honest
property) plus a visible `(untagged)` note on the row (§3.4). Table order
keeps FLAC the referenced master whenever it's selected; WAV only becomes the
CUE/M3U8 master in rips where it's the first lossless selection (WAV-only,
MP3+WAV), which is correct. Override at review if you want `*` reserved.

---

## 1. Findings vs. the brief (all favorable, one required guard confirmed)

1. **`rip_formats` needs NO parse edit.** `parseRipFormats`
   ([UIManager.cpp:193](src/UIManager.cpp#L193)) matches tokens against
   `kRipFormats[].label` — the `wav` token starts working the moment the row
   exists. Same for the summary line, digit range hint, and toggle handler
   (all table-driven). The modal logic diff for this slice is **zero lines**,
   as slice 2 designed.
2. **The tagFile skip is genuinely required, and the guard is one additive
   line.** `tagFile` dispatches `is_mp3 ? MPEG::File : FLAC::File`
   ([CDRipper.cpp:660](src/CDRipper.cpp#L660), :685, :722) — a `.wav` path
   would fall into the FLAC branch and hand a RIFF file to
   `TagLib::FLAC::File`. The guard goes at the tag-pass loop
   ([CDRipper.cpp:1925](src/CDRipper.cpp#L1925)), keyed on a new `taggable`
   row property (§3.2). Byte-identity for FLAC/MP3 is structural: the loop
   body they execute is unchanged — the new `continue` is simply false for
   them, and `tagFile` itself stays at a zero-line diff for the third slice
   running.
3. **`total_frames` is exact — no header back-patch needed on any kept file
   (§3.7).** The invariant is written in the code:
   [CDRipper.cpp:920](src/CDRipper.cpp#L920) — "always produce exactly
   `track.length_lba * SECTOR_SAMPLES`: (sectors × 588) − corr_sub_skip
   [loop] + corr_sub_skip [tail]". `open()` receives that same product. Every
   path where fewer frames are written (read failure, encoder failure,
   cancel) ends with the file **removed** (ripTrack cleanup), so a kept WAV
   always matches its header. Defensive addition anyway (§3.1): finalize
   back-patches the two size fields **only if** written ≠ declared — a no-op
   for every CD rip, self-healing for any future non-exact caller, and
   byte-invisible when exact.

## 2. Resolved anchors

| what | where |
|---|---|
| Row table to append | [include/RipFormats.h:25-26](include/RipFormats.h#L25-L26) |
| Encoder factory switch | [src/CDRipper.cpp:761-763](src/CDRipper.cpp#L761-L763) (`makeEncoder`) |
| Tag-pass loop (the guard site) | [src/CDRipper.cpp:1925](src/CDRipper.cpp#L1925) |
| tagFile extension dispatch (unchanged) | [src/CDRipper.cpp:660](src/CDRipper.cpp#L660) |
| Exact-frame invariant | [src/CDRipper.cpp:920](src/CDRipper.cpp#L920), :938 |
| CUE/M3U8 master logic (unchanged) | [src/CDRipper.cpp:1933-1936](src/CDRipper.cpp#L1933-L1936) |
| Modal row printf | [src/UIManager.cpp:1759](src/UIManager.cpp#L1759) |
| Quality-text switch | [src/UIManager.cpp:1754-1757](src/UIManager.cpp#L1754-L1757) region |
| `rip_formats` parse (unchanged) | [src/UIManager.cpp:193](src/UIManager.cpp#L193) |
| Seam-test target (gains WavEncoder.cpp) | [tests/CMakeLists.txt:467-468](tests/CMakeLists.txt#L467-L468) |

## 3. Design

### 3.1 `WavEncoder` (`include/WavEncoder.h` + `src/WavEncoder.cpp`)

Implements the seam exactly; the whole encoder is ~80 lines, no library:

- **open(path, total_frames):** `port::fopenUtf8(path, "wb")`; write the
  canonical 44-byte RIFF/WAVE header immediately — PCM fmt 1, 2 ch,
  44100 Hz, byteRate 176400, blockAlign 4, 16 bps, `data` size =
  `total_frames * 4`, RIFF size = 36 + data size. Fields serialized
  explicitly little-endian byte-by-byte (moot on both LE targets, but
  recorded and endian-proof).
- **writeFrames:** `fwrite(interleaved, 4, frames, f_)` — the one encoder
  that is a verbatim dump; also accumulates `written_frames_`.
- **finalize(ok):** if `ok` and `written_frames_ != declared_frames_`,
  seek-and-patch offsets 4 and 40 (never taken on the CD path, §1.3);
  `fclose`. On `!ok` just close — partial-file **removal stays the caller's
  job**, exactly like the sibling encoders (ripTrack's cleanup loop removes
  every `outs` path; finalize(false) does not remove, matching
  FLAC's/LAME's behavior and slice 1's error-semantics table).
- **Ctor: none/default** — no quality knob, no default to pin; WAV's oracle
  is bit-exactness (§5.1), not a default-equality arm.

### 3.2 Table row + properties

`RipFormatRow` gains two fields, both properties later slices also need:

```cpp
struct RipFormatRow {
    RipFormat id; const char* label; const char* ext;
    bool lossless;
    bool taggable;        // false -> skipped by the tag/R128 pass
    const char* note;     // row annotation, "" for most ("(untagged)" for WAV)
};
{ RipFormat::Flac, "FLAC", ".flac", true,  true,  "" },
{ RipFormat::Mp3,  "MP3",  ".mp3",  false, true,  "" },
{ RipFormat::Wav,  "WAV",  ".wav",  true,  false, "(untagged)" },   // digit 3, append-only
```

Existing rows gain values that reproduce today's behavior exactly. Opus
(slice 4: `true` taggable) and WavPack (slice 5: lossless+taggable) slot in
with no further mechanism.

- **Tag-pass guard** at :1925: `if (const auto* r = ripFormatRow(o.fmt); r && !r->taggable) continue;` — the only existing-code edit outside the table/factory/modal-text.
- **makeEncoder** gains `case RipFormat::Wav: return std::make_unique<WavEncoder>();`.

### 3.3 CUE/M3U8 interplay (§3.4 of the brief) — resolves correctly, no edit

The master rule at :1933 is "first selected lossless in table order, else
first selected": FLAC+WAV → FLAC (row 1); default+WAV → FLAC; **MP3+WAV →
WAV** (first lossless — correct: WAV is the verifiable master there);
WAV-only → `.wav`. No code change; verified in §5.6.

### 3.4 Modal row display

Quality-text switch gains `RipFormat::Wav -> "16-bit PCM"` (informational,
not a setting). The `note` renders after the marker: WAV's row reads
`[ ] 3  WAV      16-bit PCM     * (untagged)` — 43 chars at x=3, well inside
BOX_W 68; FLAC/MP3 rows render byte-identically (empty note appends
nothing). BOX_H grows 19 → 20 for the third row, hint becomes `(1-3 toggle)`
automatically from `kRipFormatCount`.

## 4. Hazard decisions

| hazard | decision |
|---|---|
| tagFile byte-identity | structural: additive `continue` false for FLAC/MP3; `tagFile` body untouched (§1.2) |
| Header correctness | explicit LE serialization + the §5.1 oracle asserts every field and the data-size math |
| total_frames exactness | exact on all kept files (§1.3); conditional back-patch as dead-path insurance |
| Master marker | `lossless=true` per §0 recommendation, with `(untagged)` note; property-keyed, order-safe |
| Append-only digit | WAV = row index 2 = digit 3; Opus/WavPack take 4/5 (recorded in the table comment) |
| Endianness | both targets LE and header explicitly serialized LE — recorded, moot |
| Disk footprint | noted; no action per brief |

## 5. Verification to PROPOSE (on greenlight)

1. **WAV bit-exact oracle** (in `rip_encoder_seam_test`): fixture blocks +
   odd tail through `WavEncoder`; assert (a) each of the 11 header fields is
   exactly canonical for the fixture's frame count, (b) the `data` chunk
   equals the input PCM byte-for-byte, (c) file size = 44 + data size. Green
   both toolchains.
2. **WAV-vs-FLAC PCM identity:** 1-track [Y] rip with FLAC+WAV → decode the
   FLAC (`flac -d` or metaflac MD5 vs the WAV data chunk's MD5) → identical
   PCM. Two lossless encoders, one buffer — a mismatch is a real bug.
3. **Default-case unchanged:** seam oracle green; WAV code inert when
   unselected (absent from `outs` by slice-2 construction).
4. **Trimmed CD gate:** 2-track [A] on the GHD3N with FLAC+MP3+WAV selected
   (full-TOC/cancel-at-3 method from slice 2) — AR v2 conf on tracks 1-2
   unchanged, WAV files present and exact.
5. **Playback audition:** the ripped `.wav` plays in RE-MOCT (built-in
   dr_wav), correct duration/level.
6. **Subset checks:** WAV-only → only `.wav` + cue/m3u8 → `.wav` + tag pass
   skipped cleanly (no TagLib call on the wav — verifiable by the absence of
   any tag bytes: file size stays exactly 44 + data); MP3+WAV → cue → `.wav`
   (the master rule's new case); FLAC+WAV → cue → `.flac`.
7. ctest both toolchains; CHANGELOG entry ("WAV rip output - untagged
   bit-exact PCM"; hyphens only).

## 6. Change inventory (on greenlight)

| file | change |
|---|---|
| `include/WavEncoder.h` / `src/WavEncoder.cpp` | **new** |
| `include/RipFormats.h` | +row, +`taggable`/`note` fields (existing rows: today's values) |
| `src/CDRipper.cpp` | +1 factory case; +1 guard line at the tag pass |
| `src/UIManager.cpp` | +1 quality-text case; note rendering; BOX_H 20 |
| `CMakeLists.txt` / `tests/CMakeLists.txt` | +WavEncoder.cpp to both targets |
| `tests/rip_encoder_seam_test.cpp` | +WAV bit-exact oracle section |
| `CHANGELOG.md`, `docs/lessons.md` | entries |

No new deps, no CI package changes, no config key, `tagFile`/`ar_crc.*`/read
path untouched.
