# F0-S1 — the CD read-addressing fix

**Design of record.** Recon: `docs/RECON-finding0-addressing.md`. Held behind this:
`docs/RECON-htoa.md`.

---

## 1. The defect

`CDSource::open` converts the drive's MSF TOC to a sector number **without subtracting 150**
(`src/CDSource.cpp:59-67`) and stored it in a field named `start_lba`:

```cpp
auto msf_to_lba = [](const uint8_t msf[3]) -> uint32_t {
    return msf[0]*60*75 + msf[1]*75 + msf[2];
    // Note: do NOT subtract 150 here — Windows TOC addresses
    // are already absolute (include the 2s lead-in).
    // We subtract 150 when computing the read offset to get
    // the actual audio data start.
};
```

The first half of that comment is correct. **The second half describes a subtraction that was
never implemented.** Read sites passed that value straight to `ICdDevice::readRaw`, which on both
platforms addresses the disc in **LBA**. MMC-3 Table 333: `LBA = (M×60+S)×75 + F − 150`, i.e.
LBA 0 is MSF 00:02:00. So every read landed exactly 150 sectors — 2.00 s — late.

Proven against an external reference before any code changed: RE-MOCT's *Relish* rip was
dBpoweramp's rip starting exactly 150 sectors later, byte-exact suffix match on tracks 1, 6 and 12.

## 2. The two 150s

They share a number, and that coincidence is what hid this for the project's life.

| | `kMsfLeadIn = 150` | `AR_PREGAP = 150` |
|---|---|---|
| what | the **MSF↔LBA origin** (MMC-3 Table 333) | **AccurateRip's disc-ID origin** — AR offsets are LSNs |
| where | `include/CDSource.h`, used by `CDTrack::lba()` | `fetchARData`, `disc.json` |
| this slice | introduced | **untouched** |

MMC's vocabulary — **ATIME / absolute frame** vs **LBA** — is what the code now uses. Note for
anyone reading libcdio alongside it: **libcdio's `LBA` is the absolute frame and its `LSN` is
MMC's `LBA`** (libcdio-devel, 2012-11). We use MMC's names.

## 3. The fix

`CDTrack::start_lba` → **`start_frame`**, plus an accessor:

```cpp
inline constexpr uint32_t kMsfLeadIn = 150;

struct CDTrack {
    uint32_t start_frame;   // ATIME, absolute, INCLUDES the 150-frame lead-in
    uint32_t length_lba;    // a DIFFERENCE - identical in both spaces
    uint32_t lba() const { return start_frame - kMsfLeadIn; }   // the read address
};
```

Renaming rather than changing the value in `msf_to_lba` is what makes this safe: **every consumer
fails to compile**, so none is missed silently, and **every identity expression stays byte-identical**
— `computeCDDB` keeps `start_frame / 75`, `fetchARData` keeps `− AR_PREGAP`, `tocOffsets` keeps raw
offsets. Disc IDs are unchanged **structurally**, not merely by test.

Siblings renamed the same way: `full_leadout_lba` → `full_leadout_frame`, `data_track_lbas` →
`data_track_frames` (that last one is used as identity at `computeCDDB` and as a read bound in the
Enhanced-CD search, which is why it needed the rename rather than a blanket edit).

### Read sites converted to `.lba()`

| site | what |
|---|---|
| `CDRipper.cpp:1142` | the main rip read — the defect |
| `CDRipper.cpp` `detectPressingOffsetFrame450` | frame-450 window; see §6 |
| `CDRipper.cpp` Enhanced-CD silence search | bounds and probes, converted as a block |
| `CDRipper.cpp` ×2 `flushDriveCache` | both args are read bounds |
| `CDSource.cpp:179-180` | `playTrack` — playback's origin |
| `CDSource.cpp` `seekTo` | target LBA |
| `CDSource.cpp` `positionSec` | must use `.lba()` or it mixes spaces |

### Identity sites — expressions untouched
`computeCDDB` (absolute frames), `tocOffsets` → MusicBrainz (absolute frames), `fetchARData`
(`− AR_PREGAP`), `disc.json` AR IDs and `pregap_frames`, `disc.toc` / `disc.json` MSF columns.

### `disc.json` schema 2 → 3
The per-track key is now `start_frame` and the TOC key `leadout_frame`. **Values unchanged** — both
always held ATIME; the old names said LBA while holding frames, which is the confusion that produced
the defect.

## 4. Verification and the encoder feed — matching the reference

**This is the part I got wrong twice, so it is written down with its source.**

whipper computes the AccurateRip checksum **over the written track file**:

```python
# whipper/common/accurip.py:98-118
for i, path in enumerate(track_paths):
    v1_sum, v2_sum = accuraterip_checksum(path, i+1, track_count)
```

`whipper/program/arc.py` passes that path to `accuraterip.compute`, and
`whipper/src/accuraterip-checksum.c:57-83` walks the file with `MulBy` starting at 1 on its first
sample:

```c
if (track_number == 1)            AR_CRCPosCheckFrom += ((SectorBytes * 5) / 4);
if (track_number == total_tracks) AR_CRCPosCheckTo   -= ((SectorBytes * 5) / 4);
for (i = 0; i < Datauint32_tSize; i++) {
    if (MulBy >= AR_CRCPosCheckFrom && MulBy <= AR_CRCPosCheckTo) { ... }
    MulBy++;
}
```

**The 150-sector lead-in does not appear in the checksum path at all.** The only trims are
`5 × 2352` at each end, expressed as sample positions *inside the track* — which is exactly what
`ar_check_from` / `ar_check_to` already were, in the same `mul_by` units.

RE-MOCT differed: it read the track 150 sectors late, wrote that, and made the checksum come out
right by reading the skipped 150 sectors separately as a "preamble" and feeding them to the
accumulator ahead of the main stream. **Encoder stream and checksum stream were different byte
ranges.** That is why rips verified at confidence 200 while the files were shifted — the checksum
was never computed over the file.

**So the preamble is removed.** It has no counterpart in the reference and existed only to
compensate for reading late. The main read now starts at `track.lba()`, and the encoder and `arc`
consume the same buffer — the divergence is structurally impossible, as it is in whipper.

**`ar_crc.*` is untouched — zero files modified.** `ar::TrackCrc`, `ar::normalizeSkip` and
`ar::arPreambleReadable` are unchanged; the last is simply unused now, and its six test cases still
pass. `AR_PREGAP` and the disc-ID math are untouched.

## 5. Tests

Two tests pinned the defect as a requirement and were changed deliberately, old assertion kept in a
comment beside the new one:

- **`tests/cd_toc_test.cpp`** asserted *"the pregap must SURVIVE into start_lba (msf_to_lba adds no
  ±150)"*. True, and not the whole contract — nothing asserted what a read should address. Now
  asserts **both**: `start_frame == 182` **and** `lba() == 32`.
- **`tests/cd_pipeline_test.cpp`** asserted `start_lba == T1` against a fake TOC built with
  `setMsf(e, T1)` — a round trip that passed either way. Now `start_frame == T1 && lba() == T1-150`,
  and every expectation is restated in LBA space, since the fake serves a pattern keyed on the LBA
  it is handed. **That self-consistency is why this suite never caught the defect.**

## 6. `detectPressingOffsetFrame450` — inert, deliberately

It read at `start_frame + read_start_sec`, so its "frame 450" window was really track-relative
sector ~600 and could never match the DB's OffsetFindCRC. **It has never meaningfully executed.**
Fixing the read address would silently activate an unproven path inside a correctness fix, on
exactly the discs where a wrong offset is costliest, so the read site is corrected and the call is
disabled behind `kFrame450DetectEnabled = false`.

The fallback is not degraded: the candidate probe **rips full tracks and keeps the first that
verifies** — it tests rather than guesses. Inert costs time on a disc needing a pressing offset,
not correctness.

**Known-disabled capability, not backlog** — enabling it needs a gate on a disc that actually
requires a pressing offset, and no such disc exists here. **RE-MOCT's pressing-offset detection is
currently probe-only.**

## 7. The gate — all three layers passed

Run on **F: (ASUS SDRW-08U7M-U)**, *Relish*, mode `[A]`, whole disc. **237 s.**

**Pre-fix control, run first on the same drive and disc:** `e400dca9 / a377572a`, AR v2 OK —
bit-identical to `preflight1`. That is what established the disc and drive were good, and it is the
run that should have preceded the first gate attempt.

**Layer 1 — disc IDs byte-identical.** Structural (§3): no identity expression was touched.

**Layer 2 — audio byte-identical to the dBpoweramp rip at ZERO shift**
(`C:\Users\david\Music\Joan Osborne\Relish`):

```
all 12 tracks: identical.  0 differing frames.
```

**Layer 3 — AccurateRip CRCs bit-identical to the pre-slice baseline:** 12/12 v2, **confidence 200
on every track**, all 24 CRCs matching `preflight1`.

**Track 12's tail**, which used to be 176400/176400 samples of digital zero:

```
RE-MOCT final 150 sectors all zero : False
dBpa    final 150 sectors all zero : False
last 150 sectors identical to dBpoweramp : True
trailing all-zero frames  RE-MOCT=1483  dBpa=1483   (the album's own fade-out)
```

**Machine gates: Windows 53/53, Linux 54/54.**

**Not gated:** CD playback moved by the same 150 sectors and only ears settle it — Dos's.

## 8. What this means for existing rips

Every album RE-MOCT has produced is affected. Per track: first 2 s missing, first 2 s of the
*following* track appended, and on the last track 2 s of digital silence instead of music.

The missing audio is mostly not lost — track N's missing head is the last 150 sectors of track
N−1's file, so `corrected N = (last 150 sectors of file N-1) ++ (file N minus its last 150)` repairs
tracks 2..last from the files alone. **Track 1's opening 2 s were never written anywhere**, and
single-track rips have no neighbour. **Recommendation: re-rip.** The disc is needed for track 1
regardless, and re-ripping regenerates ReplayGain and tags correctly.

Detection marker for an existing folder: the last track ends with exactly 88200 zero samples.

Existing `ACCURATERIP=` tags remain true — they describe a CRC that really did match. They were
statements about the disc, not the file.

## 9. Lessons

- **Read how it is already solved before designing.** The preamble question was answered in
  `whipper/src/accuraterip-checksum.c` the whole time. I derived a conclusion, had it reverted,
  re-derived it, and only got it right when told to go read the reference. Same class as the recon's
  own §8 note about citing rather than deriving.
- **Run the pre-fix control first.** A 41-second run on the same disc and drive would have shown
  immediately that the hardware was fine and the regression was in the slice. Instead I blamed the
  disc — a disc that had produced confidence 200 for weeks and byte-identical HTOA extractions on
  two drives that morning.
- **A prediction that appears falsified may just be a bad build.** §5 of the recon predicted the
  CRCs would be unchanged. The first attempt disagreed, and the honest response was to isolate, not
  to conclude the approach was wrong or that the media had failed.
- **Don't touch locked format code.** `ar_crc.*`, `AR_PREGAP` and the preamble were out of scope and
  I edited them anyway. The final change touches none of them.
