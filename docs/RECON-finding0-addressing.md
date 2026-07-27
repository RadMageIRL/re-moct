# RECON: the CD read-addressing defect — FINDING-0

**Status:** recon only. Zero tracked files modified, nothing implemented, nothing committed.
UNTRACKED until a slice is greenlit.

**Sequencing (Dos, confirmed):** this goes first; HTOA is held behind it, because HTOA's span is
defined in the coordinate system this moves. See `docs/RECON-htoa.md`.

---

## 1. The defect

**Every CD read RE-MOCT issues is 150 sectors — exactly 2.00 seconds — later than intended.
Rips and playback, both platforms, every disc, every mode.**

`CDSource::open` converts the drive's MSF TOC to a sector number **without subtracting 150**
(`src/CDSource.cpp:59-67`) and stores it as `start_lba`:

```cpp
auto msf_to_lba = [](const uint8_t msf[3]) -> uint32_t {
    return msf[0]*60*75 + msf[1]*75 + msf[2];
    // Note: do NOT subtract 150 here — Windows TOC addresses
    // are already absolute (include the 2s lead-in).
    // We subtract 150 when computing the read offset to get
    // the actual audio data start.
};
```

The first half of that comment is **correct** — the TOC really is absolute. The second half
describes a subtraction that **was never implemented in the read path.** `CDRipper.cpp:1124` and
`CDSource.cpp:179` both pass `start_lba` straight through to `ICdDevice::readRaw`, which on both
platforms treats it as an **LBA**.

`start_lba` is one value holding two meanings — absolute frame and LBA. **Unlike the four earlier
instances this project has closed, these two never coincide.** They differ by 150, always.

## 2. This is NOT Windows-only — Dos's hypothesis, tested and disconfirmed

**Dos's read was that `CdIoWin.cpp` and `CdIoSgIo.cpp` are independent implementations against
different APIs, so the two paths are unlikely to share the defect — making Linux a working
reference and the fix Windows-local.**

**The premise is right and the conclusion does not follow.** The two implementations *are*
independent, and **both are correct.** The defect is not in either of them — it is in the
**shared consumer code above the seam**, which both receive their input from.

`include/core/ICdIo.h:14,32-33` says so by design:

> "ALL rip and disc logic stays consumer-side: … **MSF→LBA conversion** … Only 'issue this device
> request' lives behind the seam"
> "MSF→LBA math deliberately stays consumer-side (CDSource's `msf_to_lba`, with its **'do NOT
> subtract 150' contract**)."

Both transports then faithfully treat the value they are handed as an LBA:

| | how it addresses | evidence |
|---|---|---|
| **Windows** `CdIoWin.cpp:82-98` | `DiskOffset = lba * 2048` → `IOCTL_CDROM_RAW_READ` | measured: last readable index = `leadout_LBA − 1`, on two drives and two discs |
| **Linux** `CdIoSgIo.cpp:94-102`, `CdbSgIo.h:34-49` | `cdbReadCd(lba,…)` → READ CD (0xBE) CDB bytes 2-5 | that field is MMC's **Starting Logical Block Address** — unambiguously an LBA |

### Measured at runtime on the Linux toolchain

Rather than argue it from the code, I built `CDSource.cpp` under WSL against a fake `ICdDevice`
that records the value handed to `readRaw`, fed it *Factory Showroom*'s real geometry, and ran it:

```
TOC as the drive reports it : track 1 MSF 01:03:00 = absolute frame 4725
Red Book LBA of that sector : 4575   (LBA = frames - 150)
CDSource::tracks()[0].start_lba = 4725
first lba handed to ICdDevice::readRaw = 4725

RESULT: Linux passes the ABSOLUTE FRAME (4725), not the LBA (4575). Same defect as Windows.
```

**There is no working reference in the tree, and the fix is not Windows-local.**

But the practical consequence is close to what Dos hoped for anyway, from the other direction:
**because the conversion is shared and lives in exactly one function, the fix is one place, not a
cross-platform coordinate change.** Both platform impls stay untouched.

> **Hardware note:** WSL2 exposes no `/dev/sr*` — USB optical passthrough needs `usbipd`, which
> would **detach the drives from Windows**. I did not do that unasked. The probe above measures
> the shared layer, which is where the defect is; a live Linux hardware run remains a gate item.

## 3. Evidence that it is real

### PROVEN against dBpoweramp. This supersedes every inferential argument below.

Dos has independent dBpoweramp rips of the gate disc:
`C:\Users\david\Music\Joan Osborne\Relish` vs RE-MOCT's
`C:\Users\david\Music\re-moct\Joan Osborne - Relish (1995)`.

Decoded both to raw PCM and compared. `SH` = 150 sectors = 88200 frames = 352800 bytes:

| track | same size | byte-identical | `RE-MOCT[0:] == dBpa[SH:]` | RE-MOCT last 150 sectors zero | dBpa last 150 sectors zero |
|---|---|---|---|---|---|
| 01 St. Teresa | yes | **no** | **TRUE** | — | — |
| 06 One of Us | yes | **no** | **TRUE** | no | no |
| 12 Lumina (last) | yes | **no** | **TRUE** | **YES** | **no** |

**RE-MOCT's audio is dBpoweramp's audio starting exactly 150 sectors later — byte for byte, on
every track tested.** Not approximately, not correlated: an exact suffix match.

Two further things this settles:

- **Everything else about the rip is correct.** The match is byte-exact once shifted, so drive
  offset (+6), sample alignment, and the decode/encode chain are all right. **The origin is the
  only defect.**
- **The last track's 2 seconds are real audio, and RE-MOCT loses them.** dBpoweramp's final 150
  sectors of *Lumina* are non-zero; RE-MOCT's are digital silence.

**This is the independent cross-check the project had never had.** It was Dos's call to look, and
it converts Finding 0 from a well-supported inference into a measured fact against an
authoritative reference implementation.

### The earlier lines of evidence

Four more, none relying on subchannel data. All consistent with the above.

**a. Windows' own TOC in both forms.** `IOCTL_CDROM_READ_TOC_EX` with `Msf=0` returns LBA, `Msf=1`
returns MSF. Every entry differs by exactly **+150**, on both discs and both drives.
`msf_to_lba` computes the MSF column; `readRaw` addresses the LBA column.

**b. The readable edge is the LBA leadout.** Binary search for the last readable index:

| drive | disc | leadout LBA | last readable |
|---|---|---|---|
| GHD3N | Factory Showroom | 196542 | **196541** |
| ASUS | Factory Showroom | 196542 | **196541** |
| GHD3N | Relish | 275790 | **275789** |

Always `leadout_LBA − 1`, never `leadout_frame − 1`.

**c. The last track's file ends in exactly 150 sectors of digital zero.** The read runs 150
sectors past the readable edge; `readSectorsWithRetry` silence-fills, returns `true`, and **logs
nothing.** Measured on a fresh rip — and on the **retained regression baseline**:

```
smoke/preflight1/12.flac   final 150 sectors: 176400/176400 samples zero (100.0%)
```

**d. The file's first samples are the disc 150 sectors late.**

```
disc lba 4575 : 00000000 00000000 ...   <- TOC track 1 start (the song's 0.48 s lead-in silence)
disc lba 4725 : 021d008d 01900084 ...   <- what 01.flac actually begins with
```

> **§4 and §5 below were written before the fix was built, and §5's central
> prediction — "the AccurateRip CRCs are bit-identical before and after" — was
> stated as falsifiable and then, on the first attempt, appeared to be falsified.
> It was not. The measured CRC change came from a build in which the preamble had
> been deleted while the addressing fix was only partly applied; once the read
> came from the track's true first sector and the encoder and the accumulator
> consumed the same buffer, the CRCs came back bit-identical on all twelve tracks
> at confidence 200. §4's reasoning holds. What was wrong was my method: I
> reasoned to the conclusion instead of reading how whipper does it, and when the
> measurement disagreed I blamed the disc rather than the change. The reference is
> now cited in the slice plan. Read §9A there before §4 here.**

## 4. What the AR preamble means once addressing is fixed — it stops existing

This is the entanglement Dos asked about, and it resolves more cleanly than expected.

The preamble (`CDRipper.cpp:1142-1182`) reads 150 sectors at `track.start_lba - 150` and feeds
them to the CRC accumulator but never to an encoder. **In LBA terms `start_lba - 150` is the
track's own true first sector.** So the accumulator currently receives:

```
preamble : true track sectors 0..149        (mul_by 1 .. 88200)
main     : true track sectors 150..len+149  (mul_by 88201 ..)
```

— contiguous from the track's true first sample, `mul_by 1` correctly placed. **The preamble is
not a preamble. It is compensation for the read being in the wrong place.**

Fix the read origin to `start_lba - 150` and **delete the preamble**, and the accumulator receives:

```
main     : true track sectors 0..len-1      (mul_by 1 ..)
```

`mul_by 1` lands on the same sample. `ar_check_from` (`AR_SKIP` = 5 sectors for track 1) and
`ar_check_to` are expressed in `mul_by` and are unchanged. **The AccurateRip CRCs should be
bit-identical before and after.**

That is a hard, falsifiable prediction and it is the natural gate (§5). It also means:

- `PREGAP_SAMPLES`, `ar::arPreambleReadable`, and the `preamble_failed` machinery all become dead.
- One fewer device read per track.
- **`AR_PREGAP = 150` in `fetchARData` and `disc.json` is untouched.** That constant is the
  identity origin and it was always right — `rel = start_lba - 150` is the correct LBA, which is
  why disc IDs, CDDB and CTDB IDs have always been correct. **The identity math is not in scope.**
- The last track's 6-sample offset borrow still reads one sector past the track. Post-fix that is
  the ordinary end-of-disc overread every ripper has (6 samples, 0.14 ms) instead of **2 seconds**.

## 5. Byte-identity inverts — the gate becomes AccurateRip agreement

**Every retained baseline was produced by this code**, so all of them carry the shift —
demonstrated above for `preflight1/12.flac`. `preflight1/`, `subF/`, `subM/`, `modeY/`, `modeB/`,
`modeC/` are **not independent evidence**; "byte-identical to baseline" currently means
"identically shifted".

**A correct fix MUST break byte-identity with all of them. Breaking it is the pass condition, not
the failure.**

**And the replacement gate already exists on this machine.** §3's dBpoweramp rips of *Relish* are
an external reference for the exact disc the project regression-tests on:

1. **PRIMARY: after the fix, RE-MOCT's *Relish* rip must be BYTE-IDENTICAL to
   `C:\Users\david\Music\Joan Osborne\Relish`.** Today it matches only when shifted 150 sectors;
   afterwards it must match at zero shift. That is an externally anchored pass/fail with no
   self-reference, and it is strictly stronger than anything the retained baselines can offer.
2. **AccurateRip CRCs and verdicts UNCHANGED** — 12/12 v2 conf 200, same `crc_v1`/`crc_v2`. §4
   predicts this; it is what proves the CRC phase survived the preamble's removal.
3. **The last track must no longer end in 150 zero sectors** — and must instead carry the audio
   dBpoweramp has there.
4. **Then re-capture baselines and retire the old ones**, labelled with which side of the fix they
   are from. A mislabelled baseline here is worse than none.

**Do not defend the old baselines.** They disagree with dBpoweramp by exactly the defect.

## 6. Why nothing caught it

- **AccurateRip cannot see it, by construction.** The CRC is fed the correctly-aligned
  preamble+main composition (§4); the silence-filled tail sits at `mul_by > ar_check_to` and is
  excluded. Confidence 200 is a true statement about the disc and says nothing about the file.
  **Same shape as CD-S4's finding, one level up.**
- **The unit tests are self-consistent under the bug.** `tests/cd_pipeline_test.cpp` builds its
  TOC with `setMsf(e, lba)` — encoding whatever number it was given — and its fake `readRaw`
  serves `pat(lba * 1176 + i)`, a pattern keyed on the LBA it is asked for. The read verifies
  against itself. A test can only catch this if it asserts the **Red Book relation** between the
  MSF the drive reports and the LBA the transport receives, which is precisely what the WSL probe
  in §2 does.
- **The silence-fill is silent in both senses:** `readSectorsWithRetry` zero-fills and returns
  `true` with no log line.

## 7. What it means for existing output

**Every album RE-MOCT has ripped is affected.** Concretely, per track: the first 2 s missing, and
the first 2 s of the *following* track appended in its place; on the last track, 2 s of digital
silence instead.

**A correction path exists for almost all of it**, because the missing audio is not lost — it is
in the neighbouring file:

> track N's missing head = disc[start_N, start_N+150) = **the last 150 sectors of track N−1's
> file**, since `end_{N-1} == start_N`.

So for a **complete album rip**:

```
corrected track N  =  (last 150 sectors of file N-1)  ++  (file N minus its last 150 sectors)
```

- **Tracks 2..last: fully recoverable from the files alone**, losslessly. The last track's
  silence-filled tail is discarded by the same rule, so it repairs itself.
- **Track 1 is the exception.** Its first 2 s were read into the AR preamble and never written
  anywhere. **Not recoverable from files.**
- **Single-track / partial rips are not recoverable** — the neighbour is absent.

**Recommendation: re-rip.** The disc is needed for track 1 regardless, re-ripping is one action
instead of a bespoke tool, and it regenerates ReplayGain and tags correctly. The correction path
above matters for one case only — **a disc the user no longer has** — and is worth recording as
feasible rather than building now.

**How a user can tell an existing folder is affected**, without this document: the last track ends
with exactly 88200 zero samples (2.000 s), and each track's final 2 s is the next track's opening.

**Existing `ACCURATERIP=` tags remain true** — they describe a CRC that really did match. They are
statements about the disc, not the file, and a re-rip should reproduce them exactly.

**Not affected:** `disc.json`, `disc.toc`, `.cue`, the AccurateRip/CDDB/CTDB IDs. All derive from
`rel = start_lba - 150`, which is correct.

## 8. Established practice — this is a solved problem, and the fix is documented

Per Dos: match documented practice, do not derive. All of the following is cited, not reasoned out.

### The conversion

**MMC-3 Draft Revision 10g, Table 333 "LBA to MSF translation", p. 282:**

> **LBA = (M × 60 + S) × 75 + F − 150**

(second branch, `− 450150`, for the negative lead-in range; valid LBA range −45150…404849, negative
values two's-complement). <http://www.13thmonkey.org/documentation/SCSI/mmc3r10g.pdf>

Corroborated three more times inside the same document — §3.1.62 (*"LBA = 0 assigned to
00:02:00"*), PLAY AUDIO §5.x (*"a starting LBA address of 0000 0000h shall begin the audio play
operation at 00:02:00 MSF"*), and most usefully **Annex K.1.4.1 Table K.5**, which prints the same
TOC descriptor in both formats:

> | Byte | MSF = 0 | MSF = 1 |
> | 4 | GAA: **0** | GAA: **00:02:00** |

Canonical implementations agree: cdparanoia III-10.2 `interface/scsi_interface.c:999`
(*"straight from the MMC3 spec"*), libcdio `lib/driver/sector.c`, and the Linux kernel's
`include/uapi/linux/cdrom.h` (`#define CD_MSF_OFFSET 150 /* MSF numbering offset of first frame */`).

### That `IOCTL_CDROM_RAW_READ`'s sector number is an MMC LBA

Microsoft documents `RAW_READ_INFO.DiskOffset` only as *"calculate this offset by multiplying the
starting sector number for the request times 2048"*
(<https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddcdrm/ns-ntddcdrm-__raw_read_info>)
without saying what "sector number" means. The **ReactOS `cdrom` class driver** settles it —
`drivers/storage/class/cdrom/ioctl.c` divides by 2048 (`SectorShift` = 11) and writes the result
**verbatim into READ CD's Starting LBA field**:

```c
startingSector = (ULONG)(rawReadInfo.DiskOffset.QuadPart >> DeviceExtension->SectorShift);
cdb->READ_CD.StartingLBA[3] = (UCHAR)(startingSector & 0xFF);   /* ...etc */
```

**MMC-3 §5.17** defines that field: *"The Starting Logical Block Address field specifies the
logical block that the read operation shall begin"* — the same LBA as Table 333. (The MSF-addressed
sibling is a **different opcode**, READ CD MSF **B9h**, §5.18 — which is itself proof that BEh is
not MSF.) So the Linux CDB path and the Windows IOCTL path are the *same* address space, which is
why §2's runtime probe found them identical.

### That the TOC's MSF includes the 150 — and the reference fix

**libcdio's Windows driver does exactly the conversion this tree is missing**, on exactly this
IOCTL's output — `lib/driver/MSWindows/win32_ioctl.c`:

```c
p_env->tocent[i].start_lsn =
  cdio_lba_to_lsn(                                    /* == x - 150 */
      cdio_msf3_to_lba( cdrom_toc.TrackData[i].Address[1],
                        cdrom_toc.TrackData[i].Address[2],
                        cdrom_toc.TrackData[i].Address[3] ) );
```

**That is the reference implementation for the fix**: take `IOCTL_CDROM_READ_TOC`'s MSF, convert to
a frame count, subtract 150, and use *that* to address reads. It is what `CDSource::msf_to_lba`'s
own comment promised and never did.

### Terminology — a trap worth writing into the code

**libcdio's "LBA" is not MMC's "LBA".** libcdio's `LBA` is the absolute frame; its **`LSN`** is
MMC's LBA (`cdio_lba_to_lsn(lba) { return lba - 150; }`). Confirmed by the maintainers:

> "what libcdio calls LSN is what the MMC spec calls LBA … libcdio's LBA is basically a
> non-negative version of MMC's LBA"
> — libcdio-devel, 2012-11 <https://lists.gnu.org/archive/html/libcdio-devel/2012-11/msg00009.html>

MMC's own names are **LBA** vs **ATIME / (H)MSF**. **Whatever the fix does, the variable must say
which one it holds** — `start_lba` meaning an absolute frame is how this happened.

**There is no standard name for this bug.** Searched T10, ECMA, libcdio, cdparanoia, whipper,
MusicBrainz, Hydrogenaudio — nothing canonical. Do not invent one. Known *instances* worth citing
as precedent that this is a common trap: libcdio's own documentation said *"to convert a LBA into
an LSN you just add 150"* while the code subtracts (acknowledged upstream as a doc bug), and
MusicBrainz disc IDs deliberately use the absolute frame (`LBA + 150`) as their "offset".

**Flagged conflict, do not cite:** libcdio's `read_audio_sectors_win32ioctl()` scales `DiskOffset`
by **2352**, contradicting both Microsoft's docs and the driver. Treat as an outlier/probable
defect; ReactOS + MS docs are authoritative here.

## 9. Shape of the fix — NOT designed, scoped only

Deliberately not designed here. What the recon establishes about its shape:

- **One shared conversion**, not a per-platform change. Both transports stay untouched.
- **Delete the AR preamble** rather than move it (§4).
- **Playback shares the origin** (`CDSource.cpp:179`) and must move with it.
- **`AR_PREGAP` / identity math is out of scope** — it was always correct.
- **A test that asserts the Red Book relation** is the thing missing, and it is cheap: the §2 probe
  is most of it.

**Open:** whether anything else consumes `start_lba` expecting absolute frames (the Enhanced-CD
silence search and the +174 correction were noted but not examined); whether `duration_sec` and
UI positions shift; a live Linux hardware run.

---

## Verification split

**RUN and green:** TOC in both forms on both drives and both discs; readable-edge search across
three drive/disc combinations; sample-level analysis of two fresh rips and one retained baseline;
direct disc-vs-file byte comparison; **a Linux-toolchain runtime probe of the shared conversion
under WSL**; code bodies opened for `CDSource::open`, `CDSource::playTrack`, `CDRipper`'s read
loop and preamble, `CdIoWin::readRaw`, `CdIoSgIo::readRaw`, `CdbSgIo.h`, and
`tests/cd_pipeline_test.cpp`.

**NOT established:**
- **No fix designed, and §4's "CRCs unchanged" prediction is untested** — it is the gate, not a
  result.
- **No live Linux hardware run** (no `/dev/sr*` under WSL; passthrough would detach the drives).
- **The full consumer list for `start_lba` is not enumerated** — §9's open item.
- **No independent-ripper cross-check** has ever been done on this project.
- **Citations pending** (§8).
