# Phase 3 slice 6 - CD: SG_IO `core::ICdIo` twin (design, ratified before code)

Ratified by Dos 2026-07-03. The LAST seam - Phase 3 finishes when the Relish rip
comes out byte-identical on Linux. All three §8 decisions of the proposal taken:
(§8.1) the CDB parity mapping is correct primitive-by-primitive; (§8.2)
drive-discovery is IN this slice (the consumer-side un-gate every prior seam
carried - ^D slice 4, ^B/^G/^U slice 2); (§8.3) the non-C2 gate-drive limit is
accepted and documented.

Scope: implement `core::ICdIo` on Linux via SG_IO SCSI passthrough on `/dev/srN` -
`src/platform/linux/CdIoSgIo.cpp`, `platform::lnx::SgIoCdIo` / `SgIoCdDevice` -
the sibling of `platform::win::WinCdIo` (`src/platform/win/CdIoWin.cpp`). Only
device transport moves; ALL rip/AR/disc logic stays consumer-side, byte-for-byte
as on Windows.

## 0. Pinned framing (do NOT re-litigate - this slice touches the CD/AR path)

The 150-sector preamble is a physical property of the disc (AccurateRip's design,
per HydrogenAudio). Disc-ID normalization is T1-LBA-relative. AR CRC math,
offset handling, `normalizeSkip`/`arPreambleReadable`, disc-ID normalization,
+174/Enhanced-CD - all correct and UNTOUCHED. This slice moves device transport
only. The tell for a good boundary: `CDRipper.cpp` and `CDSource.cpp` get zero
rip-logic diff; only the SG_IO impl file is new.

## 1. Survey - the seam, its Windows impl, its two consumers (file:line)

`core::ICdIo` is 7 primitives (`include/core/ICdIo.h:55-98`), each one baseline
`DeviceIoControl` today and one SCSI CDB on Linux:

| # | method | Windows impl (CdIoWin.cpp) | consumer call sites |
|---|---|---|---|
| 1 | `open(drive)` → device | `CreateFileW(\\.\D:)` :139-156 | `CDSource::open` :16-23; `CDRipper` factory :155 |
| 2 | `readToc(CdToc&)` | `IOCTL_CDROM_READ_TOC` :33-53 | `CDSource::open` :32,:107 |
| 3 | `lastSessionFirstTrack(u8&)` | `IOCTL_CDROM_GET_LAST_SESSION` :55-63 | `CDSource::open` Enhanced-CD detect :104 |
| 4 | `readRaw(lba,sectors,want_c2,buf,size,got)` | `IOCTL_CDROM_RAW_READ` :65-83 | `CDRipper::probeC2` :169, `readSectors` :187, binary-search probes :1433,:1443; `CDSource::readSectors` :281 |
| 5 | `setSpeed(u16 kbps)` | `IOCTL_CDROM_SET_SPEED` :85-96 | `CDSource::open` `0xFFFF` :137; `CDRipper::setDriveSpeed` :241 |
| 6 | `mediaPresent()` | `IOCTL_CDROM_CHECK_VERIFY` :98-102 | `CDSource` media-loss poll :224 |
| 7 | `model()` → "VENDOR PRODUCT" | `IOCTL_STORAGE_QUERY_PROPERTY` :104-131 | `CDSource::open` → offset table :27 |

Phase 1 slice 8 shaped the interface to port: `want_c2` is an EXPLICIT flag (not
buffer-size-implicit), raw MSF is handed over (LBA math consumer-side), `got` is
surfaced (the C2 probe keys on it). All three pay off here.

## 2. The CDB mapping - every primitive as one `ioctl(fd, SG_IO, &hdr)`

Common transport: build `sg_io_hdr_t` - `interface_id='S'`,
`dxfer_direction=SG_DXFER_FROM_DEV` (or `SG_DXFER_NONE` for TUR/SET SPEED), the
CDB in `cmdp`, a sense buffer in `sbp`, per-op `timeout`. Success =
`ioctl==0 && (status&0x3e)==GOOD && host_status==0 && !(driver_status&DRIVER_SENSE)`;
`got = dxfer_len - resid`.

| Primitive | CDB (opcode + key fields) | byte-parity argument |
|---|---|---|
| **readToc** | **READ TOC 0x43**, `MSF=1` (byte1 bit1), Format `0000b` (byte2), start-track 0, alloc 804 (4+100×8) | Windows `IOCTL_CDROM_READ_TOC` IS this command under cdrom.sys. Response = 4-byte {len,first,last} + 8-byte descriptors {reserved, ADR/Control, track#, reserved, 0,M,S,F}. Copy descriptors **in response order** into `entries[0..]` - identical to how cdrom.sys fills `CDROM_TOC.TrackData[]`. Zero-init the buffer (Windows' `CDROM_TOC toc{}` does the same). |
| **lastSessionFirstTrack** | **READ TOC 0x43**, Format `0001b`, MSF=1 | `IOCTL_CDROM_GET_LAST_SESSION` = READ TOC format 1. First descriptor's track# = the one byte the consumer reads (:61). |
| **readRaw** | **READ CD 0xBE** - see §3 | the audio read that feeds AR CRC. |
| **setSpeed** | **SET CD SPEED 0xBB**, Read Speed = `read_kbps` bytes 2-3 **big-endian**, Write Speed bytes 4-5 = 0 (mirror Windows `WriteSpeed` zero-init) | same KB/s units as `IOCTL_CDROM_SET_SPEED.ReadSpeed` (1x=176). `0xFFFF`=max. Fire-and-forget, result ignored. |
| **mediaPresent** | **TEST UNIT READY 0x00**, all-zero CDB, `SG_DXFER_NONE` | GOOD=ready→true; CHECK CONDITION (not-ready/no-media)→false. Same "readable media present?" as `IOCTL_CDROM_CHECK_VERIFY`. |
| **model** | **INQUIRY 0x12**, alloc 96 | Vendor ID bytes 8-15, Product ID 16-31; trim+join with the SAME whitespace trim as :118-130. **Load-bearing for byte-identity - §3.** |

## 3. The parity crux - READ CD 0xBE, C2, and the two silent-drift risks

**READ CD 0xBE (12-byte CDB):**
- Byte 0 = `0xBE`.
- Byte 1 = `0x04` - Expected Sector Type `001b` (CD-DA) in bits 4-2. Twin of
  Windows `RAW_READ_INFO.TrackMode = CDDA`.
- Bytes 2-5 = **LBA big-endian = the seam's raw `lba` directly.** The Windows
  `DiskOffset = lba*2048` (`CdIoWin.cpp:74`) is a RAW_READ_INFO artifact that
  does NOT cross the seam; SG_IO addresses the platter in native LBA. Same
  physical sector, no scaling - the sharp catch that otherwise reads wrong sectors.
- Bytes 6-8 = Transfer Length in blocks = `sectors`.
- **Byte 9 = the flag byte, where C2 lives:**
  - `want_c2=false` → `0x10` (User Data bit) → **2352 bytes/sector**, no
    sync/header/ECC/C2. Identical to Windows non-C2 raw read.
  - `want_c2=true` → `0x12` (User Data | Error Flags `01b` in bits 2-1) →
    **2646 bytes/sector**, layout **[2352 audio][294 C2]** per sector - exactly
    the layout `CDRipper.cpp:191-200` de-interleaves (`src + SECTOR_BYTES + b`).
- Byte 10 = `0x00` (no subchannel). Byte 11 = `0x00`.

**Why the audio bytes are byte-identical:** both `IOCTL_CDROM_RAW_READ` and READ
CD 0xBE issue the SAME MMC READ CD to the SAME drive firmware with CD-DA sector
type + user-data. The 2352 platter bytes are the drive's, not the OS's. AR CRC is
computed over those 2352 → identical → same `crc_v1`/`crc_v2`/`pcm_crc32`/frame450.

**Byte layout (libcdio-confirmed):** byte 1 = `(read_sector_type << 2)` with
CD-DA=1; byte 9 = `(sync<<7)|(header<<5)|(user_data<<4)|(edc_ecc<<3)|(c2<<1)`,
so user-data=bit4 (0x10), C2-error-block=bits2-1 (0x02) → 0x10 / 0x12. Matches
`mmc_read_cd`.

**Two silent-drift risks - named, each with a verification step:**

1. **`model()` → +6 offset is load-bearing, not cosmetic.** The model string
   feeds the AccurateRip offset table (consumer-side); Dos's GHD3N resolves to
   **+6**, which SHIFTS the sample window. A Linux INQUIRY string that doesn't
   match the table the same way Windows' `STORAGE_DEVICE_DESCRIPTOR` did → offset
   changes/misses → **different bytes → gate fails looking like a read bug.**
   *Mandatory pre-check: before the rip, log Linux `model()` and confirm it
   resolves to the identical +6 offset as the Windows baseline. Check, don't assume.*

2. **The C2 probe converges by two mechanisms - accepted limit.** GHD3N is
   non-C2 (baseline prints **"C2 support: no"**). Windows: 2646 buffer →
   `got=2352` → `ok && got==2646` false. Linux: byte9=`0x12` on a non-C2 drive →
   **CHECK CONDITION (invalid field in CDB)** → `ok=false`. Both → probe false →
   the entire rip runs the `want_c2=false` (2352) path; the consumer gates only
   on `ok && got==2646` (`CDRipper.cpp:171`), robust to the difference. **HONEST
   LIMIT (§8.3):** because the gate drive is non-C2, the C2 de-interleave path
   (byte9=0x12, 2646, [audio][c2]) is proven by `cdb_sgio_test` + the fake +
   probe-rejection - NOT by the Relish hardware rip. Same class as slice 3's
   WinINet `icy_metaint=0` limit: named in-code, pinned in the test, accepted.

**TOC parity detail:** copy descriptors in the drive's response order (not
reindexed by track number) - how cdrom.sys fills `TrackData[]`, and the consumer
reads `entries[t-1]`/`entries[last]` off that ordering. `start-track = 0` requests
from the beginning; **the parsed TOC is gate-validated against the known Relish
layout (12 tracks, T1 @ MSF 182 = LBA 32, leadout) - a one-byte fix if a drive
disagrees.** The Enhanced-CD session-2 leadout falls out of a correct format-1
response.

## 4. Scope fence (what does NOT move)

Only device transport. Untouched, consumer-side, byte-for-byte as on Windows: AR
CRC math, the 150 preamble, `normalizeSkip`/`arPreambleReadable`, disc-ID
normalization (T1-LBA-relative), the frame450 sweep, Enhanced-CD silence search +
+174, retry/silence-fill (`readSectorsWithRetry`), cache-flush, C2 interpretation/
de-interleave, MSF→LBA, the drive-offset table. `CdIoWin.cpp` untouched.

## 5. Consumer-side un-gate - reaching the seam on Linux (decision §8.2, IN scope)

The transport impl alone doesn't make the gate runnable: the rip is UI-triggered,
and drive discovery is wholly `#ifdef _WIN32` today - `listDrives()` returns empty
(`UIManager.cpp:5131-5142`), `activateDrive()`'s CD branch is Windows-only
(:5167-5205, falling through to the portable `fs::exists` browse), and `Ctrl+Y`/
`Ctrl+R` are the roadmap's "come alive at slice 6" keys. Preamble requirement:
**[Drive] is empty on Linux and must list browsable roots/mount points AND CD
drives** - so this un-gate also makes the Linux file browser navigable at all.

- **`listDrives()` Linux branch:** browsable roots + mount points (`/`, `$HOME`,
  and real mounts under `/media`, `/run/media`, `/mnt` parsed from
  `/proc/mounts`) PLUS CD devices (`/dev/sr*`, globbed). CD entries are marked so
  `activateDrive` can distinguish them from directory roots.
- **`activateDrive()` Linux branch:** a `/dev/sr*` entry → the CD-open +
  playlist-populate twin of the Windows block (`audio_.openCD("srN")`); anything
  else falls to the existing `fs::exists` browse (already portable).
- **`open()` spec mapping (impl-side):** `"srN"`/`"/dev/srN"` → the device node;
  opened `O_RDONLY | O_NONBLOCK` (non-blocking so a spinning-up/empty drive can't
  hang the open). Windows keeps `"D" → \\.\D:`.
- **^Y/^R un-gate:** the CD-rip / MB-lookup key cases already run when
  `audio_.cdMode()` is true (`UIManager.cpp:3949,3969`); they become reachable on
  Linux the moment `activateDrive` can enter CD mode. Any lingering `#ifdef _WIN32`
  on those cases removed per-case with a reason (the slice-2/4/5 sweep discipline).

**Actual scope (found at implementation - larger than the ratified framing):**
the ENTIRE CD UI was blanket `#ifdef _WIN32`-gated as "CD off on Linux yet"
scaffolding, not just drive discovery + ^Y/^R. Making the seam reachable meant
un-gating all of it (the "whole-body gates outlive their era" lesson at scale):
the CD media/eject poll, MB/rip-status poll, overlay draw dispatch, `drawRipConfirm`,
the RipConfirm input modal, now-playing CD label + badge + `[CD]`/`[MB]` modes,
progress CD pos/dur + meta, the **cmdline rip/MB status** (so rip progress renders
on Linux), ^Y/^R, and the n/p/queue/select CD-playback routing. Every un-gated
block is portable (ncurses + already-portable CDSource/CDRipper/MBLookup); on
Windows they emit identical tokens, so **the un-gates alone are a Windows
preprocessed-TU non-event.** Kept gated: the Ctrl+F/MBSearch *entry* (roadmap
deferral - its draw/input handlers were already common), the streaming top-bar
label (a pre-existing Linux gap, out of scope), and the 4 genuine Windows-API
calls (`GetLogicalDrives`, `GetDriveTypeW`×2, `GetFileAttributesW`).

**CD-path generalization (a Windows-token change, behavior-identical):** CD track
paths are `"sr0:CD Track NN"` on Linux, but `parseCDPath`/`cdTrackNumber`/
`isCDTrackPath` (`include/StringUtils.h`) hardcoded a single-char drive
(`substr(0,1)`, index-11). Generalized to colon-delimited specs - provably
identical for every Windows single-char drive (`"D:…"` → `drive="D"`), correct for
`"sr0:…"`. This is the ONE change that alters Windows tokens, so the Windows
regression bar shifts from "preprocessed-TU non-event" to **16/16 ctest + a real
Windows CD rip smoke test** (a stronger check for a parseCDPath regression anyway).

## 6. Wiring

- **NEW `src/platform/linux/CdbSgIo.h`** - the pure CDB builders (transport-side,
  under `src/platform/linux/`, NOT `include/core/` - the SCSI CDB shape is
  transport knowledge, exactly as `NotifyArgv.h`). Returns fixed-size
  `std::array<uint8_t,N>`; no platform headers. Consumed by the impl AND the test.
- **NEW `src/platform/linux/CdIoSgIo.cpp`** - `SgIoCdIo`/`SgIoCdDevice` + the
  `core::cdio()` link-time bridge (the HttpCurl/IpcUnixSocket/NotifyNotifySend
  pattern).
- **`SeamStubs.cpp` DELETED** - its last stub (CD) + `cdio()` bridge move to the
  real file; the file dies exactly as slice 0 predicted.
- **`CMakeLists.txt`:** `CdIoSgIo.cpp` into the `if(UNIX)` sources + the
  `remoct_linux_seams` OBJECT lib; drop `SeamStubs.cpp` from both lists.
- **`tests/CMakeLists.txt`:** a `CDIO_IMPL` selector (the `HTTP_IMPL`/`IPC_IMPL`
  twin) so `cd_toc_test` + `cd_pipeline_test` build on BOTH platforms (they inject
  a `FakeCdIo`; the impl is linked only for the `core::cdio()` symbol). NEW
  Linux-only `cdb_sgio_test`.
- **`cd_toc_test.cpp` / `cd_pipeline_test.cpp`:** de-`#ifdef _WIN32` the whole-file
  wrap; cd_pipeline_test's `GetTickCount`/`Sleep` → `port::tickMs`/`port::sleepMs`
  (PortUtil.h - the slice-1 helpers, Windows expansion byte-verbatim), windows.h
  include dropped. FakeCdIo + assertions unchanged.
- **`UIManager.cpp`/`.h`:** the §5 Linux drive-discovery branches + ^Y/^R un-gate.
- **Zero diff:** `ICdIo.h`, `CdIoWin.cpp`, `CDRipper.cpp`/`.h`, `CDSource.cpp`/`.h`.

## 7. Test + gate plan

**Headless:**
- **NEW `cdb_sgio_test`** (Linux-only, the `notify_argv_test` analog): asserts the
  EXACT CDB bytes from `CdbSgIo.h` - READ CD 0xBE byte-1 `0x04` (CD-DA), byte-9
  `0x10` (no-C2) / `0x12` (C2), LBA big-endian, transfer length; READ TOC 0x43
  MSF=1 format 0 & 1; SET CD SPEED 0xBB big-endian KB/s; INQUIRY 0x12; TUR 0x00.
  Makes the C2 byte9=`0x12` CDB a TESTED artifact the gate drive can't run live.
- **`cd_toc_test` + `cd_pipeline_test` made portable** (CDIO_IMPL) - the FakeCdIo
  suites now run on Linux too, proving CDSource/CDRipper drive the seam identically
  off-hardware.
- HONEST LIMIT (in-code): a fake/CDB test cannot prove the real SG_IO read returns
  the right platter bytes - only the hardware rip does.

**THE gate** (Linux, `usbipd attach --wsl --busid 4-1` → `/dev/sr0`, GHD3N, Joan
Osborne - *Relish*):
1. `model()` resolves to the **same +6 offset** as the Windows baseline (§3 pre-check).
2. Rip → **12/12 AR v2, conf 200**, AND **byte-identical to the Windows baseline
   log** - every per-track `crc_v1`/`crc_v2`, all 12 `pcm_crc32`, frame450
   global/local, the T1 first-16-samples dump, the **"C2 support: no"** line.
   conf-200 alone is NOT the bar; byte-identical-to-Windows is.
3. **CD playback through the seam** (the other consumer): play a track, seek -
   `CDSource::readSectors` non-C2 path live.
4. `usbipd detach` returns the drive to Windows.

**Windows regression:** new `platform::linux` file only; `CdIoWin.cpp` untouched →
Windows **16/16** unchanged + UIManager preprocessed-TU non-event. Verified, not assumed.

## 8. Decisions (ratified)

1. **Parity mapping (§2/§3): ratified** - READ CD byte-9 `0x10`/`0x12`, sector
   type CD-DA, LBA-direct (no ×2048), the C2-probe two-mechanism convergence.
2. **Drive-discovery (§5): IN this slice** - the consumer-side un-gate that makes
   the seam reachable; also fixes the empty [Drive] menu + mount-point browsing.
   `O_RDONLY|O_NONBLOCK`.
3. **Non-C2 limit (§8.3 above): accepted, documented** - C2 de-interleave is
   CDB-test/fake-proven, not hardware-proven (GHD3N non-C2).

## 9. Build order (ratified)

1. **CDB builder + `cdb_sgio_test` - assert the bytes FIRST** (this is the cheapest
   point to catch a byte-layout disagreement, before the impl is built on it).
2. **SG_IO impl** (`CdIoSgIo.cpp`) + main CMake + SeamStubs deletion + CDIO_IMPL
   portable tests.
3. **Drive-discovery un-gate** (UIManager) - the empty-[Drive] fix + ^Y/^R.

Then the gate: `model()`→+6 first, then Relish 12/12 AR v2 conf 200 byte-identical
to the Windows baseline, plus CD playback (seek), `usbipd detach` after, Windows
16/16 unchanged + UIManager preprocessed-TU non-event. **Stop for review before commit.**
