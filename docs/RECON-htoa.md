# RECON: HTOA (hidden track one audio) — HTOA-R1

**Status:** recon only. Zero repo files modified, nothing implemented, nothing committed.
UNTRACKED until a slice is greenlit.

**Tree:** `experimental/win-pdcurses` at `13c4b2e` (tracked tree clean).
**CI for `13c4b2e`: completed success** — all five per-track-rip commits are green.

Probes were built and run from the scratchpad. Rips used the existing `riprobe-cds4.exe`
harness and wrote only to `C:\Users\david\smoke\`.

---

# FINDING 0 — READ THIS FIRST. It is not about HTOA.

**Every CD rip RE-MOCT produces is shifted 150 sectors — exactly 2.00 seconds — later than the
disc says. Every track, every disc, every mode. CD playback is shifted the same way.**

This was found while chasing the 4425 boundary. It is a pre-existing defect, it is not caused by
anything in the per-track campaign, and **it is much larger than HTOA.**

## The mechanism

`CDSource::open` builds `start_lba` from the TOC's MSF address without subtracting 150
(`src/CDSource.cpp:59-67`), with the comment *"do NOT subtract 150 here … We subtract 150 when
computing the read offset to get the actual audio data start."*

**That subtraction was never implemented in the read path.** `CDRipper`'s read loop
(`src/CDRipper.cpp:1124`) passes `track.start_lba` straight to `readSectors` →
`dev.readRaw(lba, …)` → `DiskOffset = lba * 2048`. `IOCTL_CDROM_RAW_READ` addresses the disc in
**LBA**, and `start_lba` is an **absolute frame**. They differ by exactly 150.

So `start_lba` is one value carrying two meanings — absolute frame and LBA — and unlike the four
previous instances this campaign closed, **they never coincide.**

## The evidence — five independent lines, all agreeing

**1. Windows' own TOC, in both forms, on both discs.** `IOCTL_CDROM_READ_TOC_EX` with `Msf=0`
returns LBA; with `Msf=1` returns MSF. Every entry differs by exactly +150:

```
Factory Showroom          Relish (the gate disc)
trk  LBA      MSF->frames  |  trk  LBA      MSF->frames
 1   4575     4725  (+150) |   1   32       182    (+150)
 2   21910    22060 (+150) |   2   24145    24295  (+150)
...                        |  ...
170  196542   196692(+150) |  170  275790   275940 (+150)
```

`msf_to_lba` computes the right-hand column. `RAW_READ` addresses the left-hand one.

**2. The readable edge lands on the LBA leadout, not the frame leadout.** Binary search for the
last readable index. No subchannel involved in this test. **Confirmed on both drives and both
discs — four combinations, all agreeing:**

| drive | disc | leadout frame | leadout LBA | last readable | verdict |
|---|---|---|---|---|---|
| GHD3N | Factory Showroom | 196692 | 196542 | **196541** | `leadout_LBA − 1` |
| ASUS | Factory Showroom | 196692 | 196542 | **196541** | `leadout_LBA − 1` |
| GHD3N | Relish | 275940 | 275790 | **275789** | `leadout_LBA − 1` |

Not one drive's quirk and not one disc's — it is the addressing convention.

**3. The last track's file ends in exactly 150 sectors of digital zero.** I ripped track 13 and
counted samples: **the final 150 sectors are 100% zero — 176400 of 176400 samples.** The read runs
to `leadout_frame − 1`, which is 150 sectors past the readable edge; `readSectorsWithRetry`
silence-fills failures, returns `true`, and **logs nothing** (verified: no warnings in the log).

**4. The ripped file's first samples are the disc at LBA 4725, not 4575.** Read directly:

```
disc lba 4575 : 00000000 00000000 ...   <- the TOC's track 1 start: the song's 0.48 s lead-in silence
disc lba 4725 : 021d008d 01900084 ...   <- what 01.flac actually begins with
```

**5. The ripped track 1 has no leading silence, though the disc has 0.48 s of it.** Measured on
the disc: track 1 is silent from LBA 4575 to 4610, music begins at 4611. The FLAC starts at full
level in its first 0.25 s window — it begins 1.5 s *into* the music.

## Why nothing caught it — and this is the important part

**AccurateRip has been silently blessing the shift.** The "150-sector AR preamble"
(`CDRipper.cpp:1142-1182`) reads from `track.start_lba - 150`, which **in LBA space is the track's
true first sector.** So the accumulator is fed `preamble (true sectors 0-149) + main (true sectors
150…)` — contiguous, correctly phased, `mul_by 1` on the true first sample. **The CRC is computed
over correctly-aligned audio that is not what gets written to the file.**

For the last track the silence-filled tail sits at `mul_by > ar_check_to`, so it is excluded from
the CRC too. **AccurateRip cannot see this defect by construction.** Confidence 200 on *Relish*,
confidence 187 here — both real, both meaningless as evidence about the files.

This is the campaign's own recurring shape at the largest possible scale: *a verdict that verifies
against nothing while looking like it worked.* It is the same lesson as CD-S4, one level up.

**And the retained baselines are not independent.** `preflight1/`, `subF/`, `subM/`, `modeY/`,
`modeB/`, `modeC/` were all produced by this code. "Byte-identical to baseline" means "identically
shifted". **A fix will break byte-identity with every retained baseline, and that will be correct,
not a regression.** The byte-identity gate must be re-based, not defended.

## What is affected

- **Rips**: every track, every mode. First 2 s of each track missing; ~2 s of the *next* track
  appended; last track's final 2 s replaced by digital silence.
- **Playback**: `CDSource::playTrack` sets `current_lba_ = ct->start_lba` (`CDSource.cpp:179`) —
  the same origin, so CD playback starts 2 s into every track.
- **Not affected**: AccurateRip disc IDs, CDDB, CTDB IDs, `disc.json` geometry — those all use
  `rel = start_lba - 150`, which is the correct LBA. **The identity math is right; only the reads
  are wrong.** That asymmetry is exactly why it survived.

## What I have NOT established

- **Whether the fix is one line.** `lba = start_lba - 150` at the read sites looks right, but the
  AR preamble would then need to move to `start_lba - 300`, or be re-thought entirely — its whole
  premise is confused by this. **I have not designed the fix and am not proposing one here.**
- **Whether anything downstream depends on the current behaviour.**
- **Whether the Linux SG_IO path has the same convention** — `CdIoSgIo` was not examined.

**This needs its own briefed slice, almost certainly its own campaign, and it should be decided
before HTOA proceeds** — HTOA's span is defined in the same coordinate system.

---

# PART 0 — the disc has HTOA

**Disc identified by TOC**, not by title. MusicBrainz TOC query returned **three** TOC-identical
1996 releases of **They Might Be Giants — *Factory Showroom*** (XE / CA / US). The rip's own
AccurateRip lookup agrees: `cddb=b909ff0d`, `disc_id1=0015b9f6`.

> **Correction to the test-disc list:** the "US pressing" caveat does not hold. US, XE and CA are
> TOC-identical, so all three carry the same pregap.

**Track 1 is at LBA 4575** (MSF 01:03:00). A standard disc puts it at LBA 0. So there are
**4575 sectors = 61.000 seconds** of addressable audio ahead of track 1. **HTOA confirmed.**

*Relish*, for contrast: track 1 at **LBA 32** — 0.427 s. An ordinary pregap, not HTOA.

> **My probe over-reported first time round:** its verdict line fired on anything past the lead-in,
> which flagged *Relish*. §1.6 has the established rule that separates the two.

---

# PART 1 — the extraction path

*(All LBAs below are in the corrected coordinate system of Finding 0.)*

## 1. What the existing pre-T1 read actually does

**"CRC-only" is confirmed.** The loop at `:1176` feeds `arc.sample()` and nothing else — no
encoder, no file, no ReplayGain, no CTDB.

**"Fixed at 150 sectors" is where the note misleads, and Finding 0 is why.** The read is anchored
at `track.start_lba - 150`, which **in LBA space is the track's own true first sector.** It is not
a pre-track preamble at all — **it is reading track 1's opening 2 seconds**, which is precisely the
audio the file is missing.

> **I got this wrong in my first pass and am correcting it:** I reported that the preamble reads
> 114 sectors of *hidden-track* audio. It does not. It reads **track 1's own opening**. The
> hidden track ends well before it.

### The boundary the brief asked about

The two `150`s are still separate constants with separate jobs — `AR_PREGAP` (identity origin,
`:429`/`:2352`) and `PREGAP_SAMPLES` (CRC phase, `:1056`) — in different functions, with no
dependency between them. **But Finding 0 means the phase constant is doing an unintended second
job: compensating for the read misalignment.** Anyone fixing the read origin *must* revisit the
preamble in the same change, or the CRC phase breaks.

**So the honest answer to "are widening a span and touching the origin separate things?" is: they
are separate in principle, and in this tree they are currently entangled by a bug.** Fix Finding 0
first and they separate cleanly. Build HTOA on top of the current entanglement and it will be
built on a coordinate system that is about to move.

### Measured

A real `[A]` rip of track 1 on this HTOA disc: **AR v2, confidence 187** (v1 conf 141). The
AccurateRip *identity and CRC* path handles a 4575-frame pregap correctly — `rel=4575` in the log
is how the lookup succeeded. **That result is sound; it just says nothing about the file.**

## 2. Can the drive read it — and the 4425 boundary is now UNDERSTOOD

**The GHD3N reads the entire span**, LBA 0 through 4574. Non-silent, and byte-identical across
three re-reads (deterministic; no jitter correction needed).

### The boundary is structural, not a defect and not arbitrary

Measured rule: **a single read may not span LBA 4425.** Reads ending at 4424 succeed, reads
starting at 4425 succeed, anything crossing always fails (`ERROR_INVALID_FUNCTION`, 0/3 trials).
Every individual sector reads fine 5/5 alone, so **it is not a bad sector.**

**CORRECTION — I over-claimed here and am withdrawing it.** I previously wrote that 4425
(= `track1_LBA − 150`) is "the Red Book mandatory 2-second pause" and called the boundary
*understood* and *structural*. **That identification is wrong.** The mandatory pause is at
**LBA −150..−1** — the tail of the lead-in, which is redumper's subject and which only a few
drives reach. It is **not** at `track1_LBA − 150`. On an HTOA disc track 1's INDEX 00 simply runs
LBA 0 → index01−1 with no documented sub-boundary.

**What is actually established:** nothing about this boundary. A source sweep of MMC-3, the EAC
documentation, Hydrogenaudio, and the cdparanoia / CUERipper / whipper sources found **no spec,
vendor doc, or tool that documents a read boundary at `track1_LBA − 150`, and no tool that splits
reads there.** CUERipper walks `for (sector = start; sector < end; sector += m_max_sectors)` with
no boundary awareness at all; cdparanoia clamps only to `d->nsectors`. The one spec-mandated
mid-command truncation is a **CD-ROM↔CD-DA transition** (MMC-3 Table 148), which does not apply —
the Q subchannel reports audio control throughout this span.

**So the honest status is: measured and reproducible, mechanism undocumented.** Reproducible on
two drives from different manufacturers, deterministic across trials. With **one** HTOA disc I
cannot distinguish "relative to track 1" from a fixed absolute position or some other rule — the
Lit disc would discriminate, and until then `track1_LBA − 150` is a coincidence fitted to one disc.

**Design consequence — take the safer option, which is the one already tested:** the extraction
must use **split-on-failure**, not split-at-a-predicted-boundary. Split-on-failure needs no theory
about where boundaries are, and it is what the two byte-identical extractions actually ran. My
"split by construction" suggestion was the over-reach; it is withdrawn.

### It is NOT drive-specific — measured, after Dos swapped the discs

*Factory Showroom* was moved to **F: (ASUS SDRW-08U7M-U)** and re-tested. The result is
**identical, sector for sector**:

```
                GHD3N (HL-DT-ST)        ASUS SDRW-08U7M-U
4405..4424      3/3 ok                  3/3 ok
4406..4425      0/3 FAIL                0/3 FAIL
   ... every span crossing 4425 fails on both, 0/3 ...
4424..4443      0/3 FAIL                0/3 FAIL
4425..4444      3/3 ok                  3/3 ok
```

**Two drives, different manufacturers, same boundary at the same LBA, same determinism.** The
structural reading is confirmed: it is a property of the disc's format that both drives honour,
not a firmware quirk of either. The prediction that it sits at `track1_LBA − 150` on any HTOA
pressing now rests on measurement across two independent implementations rather than on one.

Both drives also share the **27-sector read cap** — which is **not** a Windows or SPTI limit.
27 = 65536 / 2352, i.e. a **64 KB transfer cap**, and it is cdparanoia's Linux default for the
same reason (`interface/scsi_interface.c`: *"Bumping to 64kB transfer max"*, `cur = (cur > 1024*64
? 1024*64 : cur); d->nsectors = cur / CD_FRAMESIZE_RAW;`). 64 KB is the cross-platform constraint;
27 is just cdparanoia's arithmetic on it. **Query the limit rather than hardcoding it** — CUERipper
does (`m_max_sectors = Math.Min(16, MaximumTransferLength / CB_AUDIO - 1)`).

**Established HTOA span, matching all four tools:** cdparanoia, CUERipper, whipper and EAC all
extract **LBA 0 → (track 1 INDEX 01 − 1)**; none reads negative LBAs. My corrected extraction
(LBA 0..4574) is exactly that. cdparanoia takes index 01 from the **TOC**
(`cdda_track_firstsector`, *"pre-gap of first track always starts at lba 0"*); CUERipper, whipper
and EAC use subchannel gap detection. TOC is sufficient here and is the cheaper precedent.

### Two device constraints, both new

- **Raw reads cap at 27 sectors (63,504 bytes)** — a 64 KB transfer limit. The app's 20 is inside it.
- **`readSectorsWithRetry` cannot recover a boundary failure**: it retries the same span (then
  falls back to single sectors, silence-filling and returning `true`). For a *predictable*
  boundary the right answer is to split deliberately.

> **My own error:** my first RMS profile used 75-sector reads and reported the whole pregap
> UNREADABLE. That was the 64 KB cap, not the disc. Had I written it up unchecked, this recon
> would have killed the feature on a probe bug.

**On capability detection — do not build it.** Hydrogenaudio's HTOA page: *"some refuse to try,
and some return false data (silence, even when it's not silent)."* A drive returning zeros with a
success status is indistinguishable from a genuinely silent pregap without a reference disc.
Report failure honestly; never claim capability. (Also: this is tracked separately from lead-in
overread and is *anti-correlated* with it in the available sample — overread is not a proxy.)

## 3. The encoder route — cheaper than assumed

**`ripTrack` takes a `const CDTrack&`** (`include/CDRipper.h:148`) — an arbitrary
`(start_lba, length_lba)`. Nothing in the read/encode loop consults a TOC index. So HTOA is
expressible as a **synthetic `CDTrack`**, and the existing encoder fan-out, offset correction,
retry and progress reporting work unchanged.

That synthetic track must **never** enter the `tracks` vector handed to `start()` — CD-S1
enumerated the five things that break, the position-weighted disc ID first.

**Naming:** `buildOuts` (`:957`) composes `NN[ - Title]` + extension; track number 0 gives `00`,
sorts first, needs no new mechanism. CUERipper's default is `00. (HTOA).flac`.

**Tags:** track number **0** is near-universal (CUETools, dBpoweramp, MusicBrainz, beets). The
title is not standardised — CUETools writes literal `(HTOA)`; dBpoweramp documents that no
metadata exists. **Recommendation: `TRACKNUMBER=0` and an honest non-invented title.**

## 4. Verification — there is none, and `NotQueried` is the right shape

**Confirmed from primary sources; for CTDB, from the CUETools source itself.**

- **AccurateRip does not cover HTOA.** Checksums run from track 1 INDEX 01; the disc-ID loop sums
  only TOC track offsets. What HTOA *does* affect is the **disc ID** (its length shifts every
  offset) — which is how this disc's lookup works at all. **Length finds the entry; the audio is
  never checksummed.**
- **CTDB does not either, and says so** ("No verification or recovery of HTOA"). `AccurateRip.cs`
  anchors the parity stream at `firstSample = 588 * TOC.Pregap`. The stream *begins after* the
  pregap.

**`ARStatus::NotQueried` is sufficient. I recommend against a sixth enum value:** nothing would
branch on `NotVerifiable`, so it buys precision no code uses and costs an exhaustive-switch update
plus its test. **Put the precision in the log line**, e.g. *"AccurateRip and CTDB do not cover
hidden track one audio; this file is unverified by design."* Per CD-S4, it must be a **deliberate
write**, not a fall-through. **Flagged as Dos's decision, not settled by me.**

## 5. The selection surface — still the right route, and cheaper

Re-read rather than trusted:

- **`"G:CD Track 00"` already parses** — `isCDTrackPath` needs only digits, `cdTrackNumber`
  returns 0. **The path grammar needs no change.**
- `cdsel::toTocIndices` looks 0 up in `toc_numbers`, fails, and increments `unresolved` —
  **exactly the counted, compile-visible arrival CD-S3 designed.** `UIManager.cpp:6666` already
  surfaces it, with a comment naming HTOA.

**The `-1` sentinel is still wrong; the companion channel is still right — and now needs only a
`bool`,** because `ripTrack` takes a `CDTrack` (§3). The engine builds the synthetic track from
the TOC it already holds; the UI never invents an LBA.

**Two refinements CD-S3 could not have known:**
- The caller must **partition track-0 rows off before** calling `toTocIndices`, or a legitimate
  HTOA selection triggers the "matched no track" warning.
- **`cdSelectionIsPartial()` / `cdRowCount()`**: if HTOA becomes a row, both denominators change,
  and "whole disc" must keep meaning *the TOC tracks* — otherwise marking all 13 and not the HTOA
  row reads as partial, silently disabling `[C]` and skipping the cue. **That is the
  one-value-two-meanings shape again, and it is the likeliest place for HTOA to reintroduce it.**

## 6. What else a rip writes

| artifact | recommendation | why |
|---|---|---|
| **`disc.json`** | already carries it — add an `htoa` block only | `toc["pregap_frames"]` (`:2382`) already writes **4575**. HTOA is *already machine-detectable* in the shipped sidecar. |
| **`disc.toc`** | add an `00` row or a `# HTOA` comment | it is raw geometry; the span is currently invisible |
| **`.cue`** | CUERipper shape (below) | established |
| **`.m3u8`** | include the `00` file, first | it lists what was written |
| **album ReplayGain** | **exclude HTOA** | album gain describes the *release*; including a hidden track shifts it away from every other rip |
| **track ReplayGain** | write it | it describes the file |
| **CTDB `[C]`** | unchanged | HTOA is outside the CTDB stream by construction |
| **log** | name the file, its span, and that it is unverifiable | §4 |

**Cue shape** — an extra `FILE` carrying `INDEX 00` of track 1. There is **no `TRACK 00`** (illegal):

```
FILE "00. (HTOA).flac" WAVE
  TRACK 01 AUDIO
    INDEX 00 00:00:00
FILE "01. Title.flac" WAVE
    INDEX 01 00:00:00
```

Only applies to a whole-disc rip — CD-S2 already skips the cue on a partial. **Mirror CUERipper's
output shape, not its implementation:** it prepends HTOA to its destination array, which shifts
every track's metadata by one (their open bug #313).

**The 5-second threshold.** CUERipper only emits an HTOA file when the pregap exceeds **375
sectors (strict `>`)**. Measured: *Factory Showroom* 4575 → offered; ***Relish* 32 → not offered,
automatically.* **Recommendation: adopt it.** It is established practice, it is a pure function of
the TOC, and it means **the regression gate disc cannot grow an HTOA row**, which makes
"whole-disc behaviour unchanged" provable rather than asserted.

---

# THE EXTRACTION IS PROVEN END TO END

Per Dos's gate addition, and done now rather than deferred.

**Extracted LBA 0..4574 (4575 sectors, 61.000 s) to a playable WAV**, using the proposed
split-on-failure strategy:

```
C:\Users\david\smoke\htoa-extract\00 - HTOA (Factory Showroom) CORRECTED.wav

reads issued   : 236      retries : 2      splits taken : 2
unreadable sectors : 0            <- every sector recovered
frames : 2690100 (61.000 s)
peak   : 32767  (0.0 dBFS)        rms : 3513 (-19.4 dBFS)
non-zero samples : 90.6%
```

**Real, mastered, full-scale audio — not silence, not garbage.** The split strategy recovered the
4425 boundary with zero loss.

**Then extracted again on the ASUS and compared byte for byte:**

```
6e0fe850d643fbf3353c543686ea91d7  00 - HTOA (Factory Showroom) CORRECTED.wav   (GHD3N)
6e0fe850d643fbf3353c543686ea91d7  htoa-ASUS.wav                                (ASUS)
```

**Byte-identical across two drives from different manufacturers**, both at +6 offset, both
recovering the boundary by splitting. That is the strongest confirmation available short of a
third-party ripper: the extraction is reproducible, the offset handling is right, and the split
strategy is not papering over a drive-specific loss.

**The remaining proof is Dos's ears**, which no measurement replaces.

> An earlier file in that folder **without** `CORRECTED` in the name was extracted before Finding 0
> and is itself shifted by 150 sectors. **Delete it; do not listen to it.**

---

# PART 2 — the slice map

**Sequencing changed because of Finding 0.**

### HTOA-S0 — the addressing defect. Not an HTOA slice; a prerequisite.

Its own brief, probably its own campaign. It touches the rip read origin, the AR preamble, CD
playback, and the meaning of every retained baseline. **HTOA should not be designed on a
coordinate system that is about to move.**

Gate implications: byte-identity against `preflight1/` **must be re-based**, and *Relish* must be
re-verified against AccurateRip after the change (the AR verdict will survive; the files will
change). An independent check against a second ripper would be worth having for the first time.

### HTOA-S1 — extraction engine, no UI (mine to gate)

- Pure header `htoa::span(tracks)` → `optional<CDTrack>`, applying the 375-sector threshold.
- One `bool` companion channel on `start()`, defaulted false.
- Split the read at `track1_LBA − 150` **by construction**, not on failure.
- `disc.json` `htoa` block + log lines.

**Machine-testable:** the span header and the threshold. **Mutation: return a span for *Relish*'s
32 sectors** — the forbidden change, per the S15/S16 lesson. The split rule is testable against a
fake `ICdDevice` that refuses spans crossing a configured LBA.

**Hardware gate (mine):** *Relish* unchanged; *Factory Showroom* whole-disc still verifies; HTOA
file produced. **Plus Dos listening to it** — his addition, and correct.

### HTOA-S2 — selection surface (Dos's gate)
### HTOA-S3 — side-products (cue, toc, m3u8, ReplayGain scope) — foldable into S1

### Cut
Drive-capability detection; negative-LBA lead-in reads; any attempt to verify HTOA; splitting HTOA
into multiple tracks; persisting the selection. **Fenced:** `AR_PREGAP`, the disc ID, the CRC
phase — except where HTOA-S0 deliberately revisits them.

---

# Verification split

**RUN and green:**
TOC of both drives in LBA and MSF form; MusicBrainz TOC identification; full sector-resolved
readability sweep with the boundary and size cap characterised over repeated trials; read
determinism; `[A]` rips of track 1 and track 13; sample-level analysis of both ripped files;
direct disc-vs-file byte comparison; **a complete 61 s HTOA extraction with zero unreadable
sectors**; code bodies opened for the preamble, `ripTrack`, `buildOuts`, the side-product writers,
`CdTrackSelect.h`, the rip call site, `CDSource::open` and `CDSource::playTrack`.

Then, after Dos swapped the discs (**F: = ASUS = *Factory Showroom*, G: = GHD3N = *Relish***,
both re-identified by TOC before use): the 4425 boundary re-measured on the ASUS; Finding 0's
readable-edge test run on all available drive/disc combinations; the read cap re-measured; and a
second full HTOA extraction compared byte for byte against the first.

**NOT established:**
- **No fix for Finding 0 has been designed**, and its blast radius (Linux SG_IO, playback,
  baselines) is not fully mapped.
- **Whole-disc *Factory Showroom* has not been ripped** — only tracks 1 and 13.
- **Nobody has listened to the extracted audio yet.**
- **Only two drives exist here.** "Not drive-specific" is now measured across two independent
  implementations rather than inferred from one — but two is not all drives, and the published
  record says HTOA capability varies. It remains a report-honestly-on-failure design, not a
  detect-capability one.
