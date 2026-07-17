# MP3 TXXX write-shape fix — PLAN (mp3-rg-write)

**Stage:** PLAN deliverable. The full malformed-frame inventory, the fix, the
deliberate re-baseline, verification. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `e7c3fc8` (mp3-rg-read
merged — the dual-shape read is the standing guard for this change).

## 1. Discovery findings (brief §3)

### 1.1 The complete TXXX inventory (§3.1) — 7 rip frames + 2 recorder frames

`tagFile`'s `addTxt` lambda (src/CDRipper.cpp:721-724, call sites :726-736)
writes, in the MP3 branch:

| # | call | frame kind | status |
|---|------|-----------|--------|
| 1 | `addTxt("TENC", "RE-MOCT v…")` | TENC (standard text frame) | **CORRECT — untouched** |
| 2 | `addTxt("TXXX", "AccurateRip=" + ar_str)` | TXXX | malformed → split |
| 3 | `addTxt("TXXX", "ACCURATERIPCRC=" + crc)` | TXXX | malformed → split |
| 4 | `addTxt("TXXX", "ACCURATERIPCOUNT=" + conf)` | TXXX | malformed → split |
| 5 | `addTxt("TXXX", "REPLAYGAIN_TRACK_GAIN=" + …)` | TXXX | malformed → split |
| 6 | `addTxt("TXXX", "REPLAYGAIN_TRACK_PEAK=" + …)` | TXXX | malformed → split |
| 7 | `addTxt("TXXX", "REPLAYGAIN_ALBUM_GAIN=" + …)` | TXXX | malformed → split |
| 8 | `addTxt("TXXX", "REPLAYGAIN_ALBUM_PEAK=" + …)` | TXXX | malformed → split |

The recorder's `tagCut` (src/StreamRecorder.cpp:221-229) is the deliberate
mirror: `TENC` (correct) + TXXX `REPLAYGAIN_TRACK_GAIN=` /
`REPLAYGAIN_TRACK_PEAK=` (malformed → same split).

**Premise refinement (divergence to absorb):** the brief's §4.4 "ENCODER/
ACCURATERIP* frames were all malformed too" is half right — the AR TXXX
frames are malformed, but **TENC is not**: it is a standard ID3v2 text frame
(no description/value split exists for it), correctly written today via the
generic `TextIdentificationFrame`. It stays exactly as-is; the fix touches
ONLY the seven+two TXXX frames.

**Keys stay byte-identical** — only the framing changes: descriptions become
`AccurateRip` / `ACCURATERIPCRC` / `ACCURATERIPCOUNT` / `REPLAYGAIN_*`
(the current pre-`=` spellings, preserving the CUETools-compatible naming
intent), values become the current post-`=` strings via the unchanged
`rg_str`/`rg_peak_str`/`ar_str` formatters.

**No other in-tree consumer exists**: a tree-wide grep finds no reader of
these frames outside LocalFileSource's RG path — which mp3-rg-read made
dual-shape (value-first), so it reads the NEW shape natively and the legacy
blob via fallback. Old files are never re-tagged and stay readable (§4.5).

### 1.2 The fix shape (§3.3) — construction already proven at this TagLib

Replace the TXXX half of the lambda with a proper user-text writer:

    auto addUserTxt = [&](const char* desc, const std::string& val) {
        auto* f2 = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
        f2->setDescription(desc);
        f2->setText(TagLib::String(val, TagLib::String::UTF8));
        tag->addFrame(f2);
    };
    // addTxt keeps existing for TENC only.

This exact construction is **already empirically proven at the installed
TagLib on both toolchains**: `mp3_rg_read_test`'s standard-shape case builds
it, and both the PropertyMap read AND the probe dump confirm a genuine
`description NUL value` frame (the read asserts -4.50/+1.25 round-trips).
Foobar2000/CUETools-class readers match TXXX by description and read the
value field — which is precisely what this produces. Identical change in
`tagCut` (its lambda is local; same replacement).

### 1.3 Tag-SHA baselines that re-baseline (§3.4)

The tag-SHA contract lives in the **retained smoke artifacts**, compared by
gate procedure (not a ctest): `smoke/subM/01.mp3` (the 1-track [A] tagged
MP3 baseline — subM.log shows `Track 1/1`, a completed rip, so tagging ran)
and `smoke/defM2/01.mp3` (the default-case sibling). `subF/01.flac` /
`defF2/01.flac` are the FLAC references and **must stay whole-file
IDENTICAL** (nothing in this slice touches FLAC).

**Re-baseline vehicle: ONE fresh 1-track default-case rip** on the GHD3N
(`riprobe <G> <out> A fm 1 0` — completed, tagged, real network AR):

- `01.flac` whole-file SHA **== old subF/defF2** → the strongest
  no-collateral proof (FLAC path untouched end to end).
- `01.mp3` differs from old subM ONLY in the ID3v2 region (§1.4) → retained
  as **`smoke/subM3/`, the new MP3 tag-SHA contract** (old subM kept,
  renamed nothing — the handoff records subM3 supersedes subM for tag-SHA;
  subM stays as the legacy-shape specimen the read fallback is tested
  against forever).
- The rip log's AR line re-proves the canonical verdict (T1 `a377572a`
  conf 200) → AR **values** unchanged, only their framing.

This doubles as the brief's "confirm AR CRC log values identical" — and it
is deliberately NOT a Joan Osborne full gate (audio + AR math untouched;
the 1-track run is the tag-layer's own gate).

### 1.4 Audio-invariance mechanism (§3.5)

MP3 layout here is `[ID3v2 header+frames][audio stream incl. Xing]…` (v4
tag at file start; tagFile saves ID3v2-only, StripNone). Proof: a ~15-line
scratch tool (not committed) reads bytes 6-9 of the ID3v2 header (syncsafe
size), skips `10 + size`, and SHA-256s the remainder — run on old
`subM/01.mp3` vs the fresh rip: **must be equal**. The whole-file SHAs
differ (expected — that IS the re-baseline); the tag-stripped SHAs equal is
what proves "only tag bytes moved." Same check on a recorder cut pre/post
fix (optional — the recorder's audio path is untouched by construction).

## 2. The write→read end-to-end case (§5 / hazard 2)

CDRipper's TU never links into tests (the standing norm), so the chain is
composed of production pieces without linking it:

1. **Production write, structural read:** `stream_record_test`'s `readMp3`
   is strengthened — post-fix it asserts the RG frame is a
   `UserTextIdentificationFrame` with `description() ==
   "REPLAYGAIN_TRACK_GAIN"` and a NON-EMPTY value field (today it only
   greps `toString()`). That's the real recorder tag path writing the
   standard shape on every ctest.
2. **Standard shape → correct read:** `mp3_rg_read_test`'s standard-shape
   case (unchanged) is the proof LocalFileSource reads exactly that shape.
3. **Rip side on hardware:** the re-baseline rip's fresh `01.mp3` through
   the rgprobe frame dump (desc/value populated) + the decode probe
   (`rg_db == the log's written value`, -6.92 canonical).

`mp3_rg_read_test` itself needs ZERO edits and must stay green both before
and after — it is the pair's guard, by design.

## 3. Hazards (brief §4) — answers

1. **Deliberate re-baseline:** stated as the contract move it is — new
   artifacts retained as `subM3/` with the old kept as the legacy specimen;
   the audio-invariance SHA (§1.4) is the separate proof that distinguishes
   "tag bytes moved (intended)" from "audio moved (bug)". FLAC whole-file
   SHA identical is the no-collateral anchor.
2. **Read-back survives:** the dual-shape read is untouched; value-first
   ordering was designed for exactly this moment. Guards: mp3_rg_read_test
   (unchanged, green) + the strengthened structural assert (§2.1).
3. **AR frames genuinely CUETools-readable:** same lambda, same fix — all
   three AR TXXX frames get the split; keys and values byte-identical.
4. **All TXXX keys fixed at once:** the seven+two inventory (§1.1); TENC
   correctly excluded (premise refinement).
5. **Legacy files:** never re-tagged; the reader's description-tail
   fallback keeps them working — subM (old) remains retained as the living
   legacy-shape specimen.

## 4. Verification (on greenlight)

- ctest both toolchains: full suite green; `mp3_rg_read_test` UNCHANGED and
  green (the pair guard); `stream_record_test` with the strengthened
  structural assert green (production recorder write = standard shape).
- The 1-track default-case rip (GHD3N): AR T1 `a377572a` conf 200 in the
  log (values unchanged); FLAC whole-file SHA == old baseline; MP3
  tag-stripped SHA == old baseline (audio invariant); rgprobe dump of the
  fresh MP3 shows all 7 TXXX as proper desc/value; decode probe reads
  rg_db == the written -6.92. Artifacts retained as `subM3/` = the new
  MP3 tag contract.
- A recorder MP3 cut post-fix probed the same way (headless: the ctest
  covers it; a live cut is Dos's optional spot-check).
- Portability eyes-on (optional, Dos): open the fresh MP3 in
  foobar2000/dBpoweramp — RG and AR fields now visible to other players.
- CHANGELOG (user-facing, hyphens): MP3 ReplayGain and AccurateRip tags are
  now written in the standard TXXX form other players read; existing files
  unchanged (and still read fine).

## 5. Sequenced next (unchanged)

Then: log semantics, ad-aware split, copy/remux, per-cut art, split-trim,
keep-draining-while-paused.
