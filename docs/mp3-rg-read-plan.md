# MP3 ReplayGain read-back — PLAN (mp3-rg-read)

**Stage:** PLAN deliverable. Root cause (probe-proven), the minimal fix,
verification. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `9f8c393` (stream-record
complete).

## 1. Root cause — probe-proven, with anchors

A throwaway TagLib probe dumped the actual ID3v2 TXXX frames and the
PropertyMap of the rip baseline (`smoke/subM/01.mp3`, known gain -6.92) and a
recorder cut (KKJO "Don't Start Now", -6.65). Both show the SAME defect:

    TXXX frame:  description = 'REPLAYGAIN_TRACK_GAIN=-6.92 dB'   value = ''
    PropertyMap: ['REPLAYGAIN_TRACK_GAIN=-6.92 DB'] = ''
    properties()["REPLAYGAIN_TRACK_GAIN"]  ->  ''  ->  rg_db 0.00

- **Write side** — `CDRipper::tagFile` MP3 branch, the `addTxt` lambda
  (src/CDRipper.cpp:708-712, used at :719-724): it constructs a **generic
  `TagLib::ID3v2::TextIdentificationFrame("TXXX")`** whose single text field
  is the whole `"KEY=value"` blob. A TXXX frame's standard layout is
  `description NUL value`; re-parsed, the ENTIRE blob lands in the
  DESCRIPTION and the value field is empty. (`StreamRecorder`'s `tagCut`
  mirrors the shape, deliberately — same defect, same fix benefits.)
- **Read side** — src/LocalFileSource.cpp:55-58 (`tryRG`): reads
  `tag->properties()[key]`. TagLib's PropertyMap keys a TXXX frame by its
  (uppercased) DESCRIPTION — so the map holds the garbage key
  `REPLAYGAIN_TRACK_GAIN=-6.92 DB` with an EMPTY value, the real key misses,
  `tryRG` gets "", and `rg_db` stays 0.00.
- FLAC/Opus work because Xiph comments are genuine key=value fields;
  properly-tagged THIRD-PARTY MP3s (real desc/value TXXX) also already work
  through the very same PropertyMap lookup. **Only our own MP3s fail.**

This is the brief's hypothesis 2 (a write-shape/read-mechanism mismatch)
— with a **premise correction the brief must absorb**:

**Divergence finding: "the TXXX shape is correct" is FALSE.** The
blob-in-description shape is nonstandard. Foobar2000-class players match
TXXX by description == `REPLAYGAIN_TRACK_GAIN` and read the VALUE field —
they read nothing from our MP3s either. The same malformed shape applies to
the AccurateRip TXXX frames (`ACCURATERIP=`, `ACCURATERIPCRC=`,
`ACCURATERIPCOUNT=` — :714-718), which undermines tagFile's
"CUETools-compatible" intent for MP3. Recorded below as a follow-up decision
— NOT taken in this slice, per the non-goals.

## 2. The minimal fix (decode-side only, ~15 lines)

In LocalFileSource's RG block (src/LocalFileSource.cpp:75-78), after the two
PropertyMap lookups miss AND the file is `.mp3`: walk the ID3v2 TXXX frames
directly —

    ref.file() -> dynamic_cast<TagLib::MPEG::File*> -> ID3v2Tag(false)
      -> frameList("TXXX") -> UserTextIdentificationFrame:
        description matches "REPLAYGAIN_TRACK_GAIN" prefix, case-insensitive
          -> value field non-empty?  parse THAT   (standard-shape safety)
          -> else parse the description's "=..." tail   (our legacy blob)

- **Track gain only** — mode parity with the FLAC/Opus path preserved
  (they read TRACK gain; so does this). No prefix clash: `_ALBUM_*` and
  `_TRACK_PEAK` don't share the `REPLAYGAIN_TRACK_GAIN` prefix.
- Gated on `.mp3` and on the PropertyMap having missed — zero change for
  FLAC/Opus/WavPack/third-party MP3s.
- The parsed value feeds the EXISTING application path unchanged:
  `info.replaygain_db` -> `current_rg_db_` (src/AudioManager.cpp:155, the
  audio-thread mirror) — the machinery FLAC already proves on every play.
- No write change: rips and recordings keep their (mirrored) shape; every
  ALREADY-RIPPED MP3 starts applying its stored gain — which is the point.

## 3. Hazards (brief) — answers

1. **Perceptible library-wide change:** yes, by design — every RE-MOCT MP3
   rip since the RG feature landed will now play at its corrected (usually
   lower) volume, finally matching its FLAC sibling. CHANGELOG states it
   plainly; the A/B gate below makes it audible before commit.
2. **Write/read round-trip:** matched empirically — the fix parses the
   exact bytes the probe dumped from real rip + recorder files, and the
   value-field-first order keeps standard files standard.
3. **Album-vs-track parity:** track gain only, same as FLAC/Opus (§2).

## 4. Verification (on greenlight)

- **New `mp3_rg_read_test`** (both toolchains, already-linked deps only):
  `Mp3Encoder` writes a real small MP3; TagLib tags three variants — the
  legacy blob shape, the standard desc/value shape, no RG — and
  `LocalFileSource` metadata read asserts -X.XX / -X.XX / 0.00. Pins BOTH
  shapes so a future write-side correction can't silently break the read.
- **Round-trip on the real baseline:** `probe subM/01.mp3` → rg_db=-6.92
  (the R1 probe output is the before-picture: +0.00), matching subF/subO's
  -6.92 exactly.
- **Recorder benefits:** probe the KKJO cut → -6.65 == its Opus sibling.
- **The audible before/after (Dos):** A/B subM/01.mp3 vs subF/01.flac —
  loudness should now MATCH (it was the MP3 playing hot).
- **Unregressed:** probe subF/01.flac + subO/01.opus rg_db unchanged; full
  ctest both toolchains.
- **CHANGELOG** (user-facing, hyphens): MP3s now honor their ReplayGain
  tags; existing ripped MP3s will play at the corrected volume.

## 5. Recorded follow-up (Dos's decision, NOT this slice)

**Correct the write shape** in `tagFile` + `StreamRecorder::tagCut`: proper
`UserTextIdentificationFrame(description, value)` for the RG *and* AR TXXX
frames — future MP3s become portable (foobar2000/CUETools-readable), the
read fallback stays for the installed base. Costs if taken: MP3 tag bytes
change, so the smoke tag-SHA baselines (subM) re-baseline — why it is its
own slice, not a rider.
