# CD-ADDRESSING-LOCKED

## The rule

The TOC reports **ATIME** — absolute frames, lead-in included. Reads address in **LBA**.

    LBA = frame − 150        (MMC-3, Table 333)

`CDTrack::start_frame` is an ATIME and is **never** a read address.
`lba()` is the only thing that goes to a read.

`computeCDDB`, `fetchARData`, `tocOffsets` and CTDB keep `start_frame`. They must never become `lba()`.

## The two 150s

Two constants share the number 150 and are different transforms. They are never substituted for each other.

| | `kMsfLeadIn = 150` | `AR_PREGAP = 150` |
|---|---|---|
| what | MSF↔LBA origin — MSF 00:02:00 ≡ LBA 0 | AccurateRip disc-ID origin (LSN) |
| authority | MMC-3 Table 333 | HydrogenAudio topic 97603 |
| used by | read addresses only | disc identity only |

`AR_PREGAP` is a fixed physical property of the disc by AccurateRip's design. Disc-absolute, never T1-relative.

**That coincidence is what hid the bug.**

## Why it was invisible

`msf_to_lba` did not subtract 150, so every read landed 150 sectors late. A separate 150-sector preamble read fed the AccurateRip accumulator ahead of the main read — so the checksum covered the track's true audio while **the file did not**.

Every version that ever ripped a CD produced shifted files that verified at full confidence.

**An AccurateRip match proves the stream you checksummed is correct. It says nothing about the file you kept unless they are the same read.**

## The evidence

- *Relish* — 12/12 byte-identical to dBpoweramp, zero shift. 12/12 AR v2, conf 200.
- *BRAT* — 15/15 byte-identical to dBpoweramp. The same disc under v1.3.2: 15/15 shifted +150, last track silence-filled — **at the same AR confidence 123**.
- *Factory Showroom* — 13/13 AR v2, conf 182–187, 4,575-sector pregap.

27 tracks byte-identical to an independent reference across two albums.

## Status

Fixed in **`f759781`**. Never re-litigated.
