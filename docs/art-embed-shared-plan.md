# Embedded art carryover (shared reader + convert-side writer) - design of record

Scope ID: art-embed-shared. Branch: experimental/win-pdcurses.
Stage: plan (this doc) -> review -> implement -> debrief -> Dos hardware gate -> commit.

Give convert the embedded cover art it lacks: a NEW per-format art extractor
(the piece that does not exist anywhere today) plus a convert-side writer, so a
FLAC->Opus convert carries the picture into the .opus block. Plus an empirical
check of text-tag completeness (RG/AR) across formats.

## Anchors re-resolved live (all confirmed current)

| Anchor | Brief | Live | Status |
|---|---|---|---|
| rip MP3 APIC (hardcoded image/jpeg) | CDRipper.cpp ~:756 | 756-760, mime "image/jpeg" | matches |
| rip FLAC / Opus FLAC::Picture | ~:786 / ~:824 | 786-790 / 824-828, "image/jpeg" | matches |
| rip WavPack APE "Cover Art (Front)" | - | 858-863, "cover.jpg\0"+bytes | matches |
| rec MP3 APIC (magic mime) | StreamRecorder.cpp ~:311 | 311-316, art_mime | matches |
| rec FLAC/Opus FLAC::Picture | ~:336 | 336-341, art_mime | matches |
| rec MP4 covr | ~:383 | 381-385, PNG/JPEG from mime | matches |
| artMimeFromMagic (JPEG/PNG, reject-else) | :166 | 166-172 | matches |
| CoverArt raster guard | - | CoverArt.cpp:29-30 accepts JPEG AND PNG | see F-B |
| convert text carryover | ConvertJob.cpp ~:48-56 | copyTextTags 48-63 | matches |

## Finding A - SECOND ITEM (RG/AR completeness): RESOLVED EMPIRICALLY, no code needed

I built a PropertyMap-carryover probe (the exact copyTextTags round-trip: read
src.properties() -> dst.setProperties() -> save -> read back) over real files
including a real .m4a, tagging each source the rip's way (Xiph fields on FLAC,
ID3v2 TXXX on MP3, MP4 freeform on M4A). Result:

| path | SRC RG/AR | DST RG/AR after carryover |
|---|---|---|
| FLAC -> MP3  | 1 / 1 | **1 / 1** |
| MP3 -> Opus  | 1 / 1 | **1 / 1** |
| M4A -> FLAC  | 1 / 1 | **1 / 1** |

**PropertyMap PRESERVES both REPLAYGAIN_* and AccurateRip fields on every path,
including the MP4 `----` freeform-atom path.** No tags are dropped. **No explicit
RG-carry code is needed - the shipped convert is already complete for RG/AR.**

Trap noted for the record: two earlier probe runs showed DST=0 and looked like a
drop. Both were the TagLib RW-handle trap ([[remoct-env-facts]]): a live write
handle on the file blanked the concurrent read-back. Scoping the write FileRef
closed before reopening for the read gave the true result above. (ConvertJob's
copyTextTags is already correct here: it opens SRC and DST - different files - and
never re-reads the file it is writing, so it has no such collision.)

## Finding B - WRITER decision: READER-ONLY SHARED; rip/rec writers stay inline

Per the brief's rule ("if a byte-identical consolidation is not achievable
cleanly, the writer stays per-caller and only the READER is shared"), I recommend
**reader shared, rip/rec inline writers untouched, convert gets its own writer.**
A truly unified single writer is not cleanly byte-identical here:

1. **MIME policy differs per caller and cannot unify byte-identically.** Rip
   HARDCODES `image/jpeg`; the recorder derives MIME from magic. CoverArt.cpp
   accepts PNG too, so a rip cover CAN be a PNG that rip currently (wrongly but
   really) stamps `image/jpeg`. A magic-deriving unified writer would emit
   `image/png` for that cover - changing rip's bytes. Preserving byte-identity
   means the MIME stays a per-caller decision, i.e. not one behavior.
2. **Byte-identity needs same-open-file operation.** Rip/rec add the picture to
   the SAME open File that writes their text/RG/AR tags, then save ONCE. A shared
   writer that opens its own File and saves = a second save pass, which TagLib
   re-serializes (padding/frame order can shift) - not guaranteed byte-identical.
   A shared writer that instead takes the already-open File would need a fragile
   4-way tag-type dispatch (ID3v2::Tag / Xiph / MP4 / APE) - not clean.
3. **Rip art has no automated byte-gate** (rip_encoder_seam_test tests encoder
   bytes only; the Joan Osborne riprobe fetches NO art - `MBRelease{}`), so a rip
   art regression would be silent until Dos's eyes-on. Not worth the risk for a
   cosmetic consolidation.
4. **stream_record_test DOES pin the recorder MP3 APIC** (`has_art` + `art_size`,
   :181-207). Leaving the recorder writer inline keeps that gate green trivially.

So the shared, genuinely-new piece is the READER. Convert gets a standalone
writer (open + save is correct for convert's freshly-created output - convert
already saves text tags separately, so an art pass is consistent with its shape).
Net: this slice touches NEITHER CDRipper's nor StreamRecorder's tagging path, so
rip/rec output is byte-identical by non-action and the Joan Osborne gate is N/A
(no CD tagging/read path touched). The convert writer covers all five encodable
formats, so "all outputs carry art through the one (convert) writer" holds.

## New module: ArtEmbed (include/ArtEmbed.h + src/ArtEmbed.cpp)

```
struct ArtBlob { std::vector<uint8_t> bytes; std::string mime; };  // mime e.g. "image/jpeg"

// READER (new - the convert need). Front cover preferred, else the first
// picture; returns the picture's OWN stored MIME. nullopt when no art / WAV.
std::optional<ArtBlob> extractEmbeddedArt(const std::string& path);

// WRITER (convert-side; open + save). Writes blob to the dst per its format,
// using blob.mime. Best-effort bool. Rip/rec do NOT call this (they stay inline).
bool embedArt(const std::string& path, RipFormat fmt, const ArtBlob& blob);
```

Reader, per format (TagLib):
- **FLAC:** `FLAC::File::pictureList()` -> the FrontCover, else front of list ->
  `pic->data()` + `pic->mimeType()`.
- **MP3:** `ID3v2::Tag::frameList("APIC")` -> the `AttachedPictureFrame` whose
  type is FrontCover, else the first -> `ap->picture()` + `ap->mimeType()`.
- **MP4/.m4a/.m4b:** `MP4::Tag` item "covr" -> the `CoverArt` list front ->
  `.data()`; MIME from its `format()` (PNG/JPEG).
- **Opus/OGG:** `Ogg::XiphComment::pictureList()` (the METADATA_BLOCK_PICTURE
  parse TagLib does) -> same selection as FLAC.
- **WavPack:** `APE::Tag` binary item "Cover Art (Front)" -> strip the
  "<filename>\0" prefix -> the image bytes; MIME from magic (the APE item stores
  no MIME).
- **WAV:** nullopt (no embedded-art concept).

Writer, per format (mirrors the existing inline shapes, mime from blob):
- MP3 APIC / FLAC Picture / Opus Picture / MP4 covr / WavPack APE binary
  ("<cover.jpg|cover.png>\0"+bytes), FrontCover, description "Cover". WAV: no-op.

A small shared magic->mime helper lives here too (for the WavPack read + any
mime fallback); it does NOT replace StreamRecorder's artMimeFromMagic (that file
stays untouched - a copy avoids perturbing the gated recorder TU).

## Convert integration (the payoff)

In `ConvertJob::convertOne`, AFTER `copyTextTags(src, dst)`:
```
if (auto art = extractEmbeddedArt(src)) embedArt(dst, fmt, *art);   // best-effort
```
Never fails the convert on an art miss/extract failure. A FLAC->Opus convert now
carries the cover into the .opus picture block; a source with no art produces an
output with no picture and still succeeds.

## Tests (in-slice; ctest both toolchains)

- **art_embed_test** (new, headless): synth a fixture of each encodable input via
  makeEncoder, embed a known JPEG and a known PNG blob via embedArt, extract via
  extractEmbeddedArt, assert the bytes + MIME round-trip. Then per input->output:
  convert (ConvertJob::convertOne on a source that HAS art) and assert the output
  has a picture whose bytes equal the source's (reader+writer through convert).
- **no-art source:** convert a fixture with NO embedded art -> output has no
  picture, convertOne returns Converted (best-effort, no failure).
- **byte-identity gates:** rip_encoder_seam_test + stream_record_test pass
  UNCHANGED. Because rip/rec writers are NOT touched (reader-only shared), this is
  by non-action - stated explicitly in the debrief.
- **tag-completeness (Finding A):** a headless assertion of the RG/AR survival
  matrix (FLAC->MP3, MP3->Opus, M4A->FLAC) - encode a source, write RG+AR, run
  the copyTextTags round-trip with handles scoped, assert both survive on the
  destination. Pins the empirical result so a future TagLib bump can't silently
  regress it.
- **Joan Osborne AR v2 gate: N/A** - the rip tagging/read path is untouched (no
  writer consolidation into CDRipper). Stated, not run.

## Touch list (no source changed until greenlit)

New: include/ArtEmbed.h, src/ArtEmbed.cpp, tests/art_embed_test.cpp. Modified:
src/ConvertJob.cpp (the two-line art pass after copyTextTags), CMakeLists.txt
(ArtEmbed.cpp into remoct), tests/CMakeLists.txt, CHANGELOG.md (implement time).
CDRipper.cpp and StreamRecorder.cpp are NOT touched.

## Non-goals (unchanged from brief)

No recursive convert, no native-rate convert, no new art SOURCES (carryover only,
source-embedded to dst-embedded), no CD read/verify or ar_crc.* change. Rip and
recorder art writers are deliberately left inline (Finding B).
