// CdbSgIo.h — pure SCSI CDB builders for the Linux SG_IO CD seam (Phase 3 slice 6).
//
// The byte-exact heart of the SG_IO ICdIo twin, factored out of CdIoSgIo.cpp so
// the CDBs can be asserted HEADLESS (cdb_sgio_test) — the injection-safety
// analog of NotifyArgv.h. Kept under src/platform/linux/, NOT include/core/: a
// SCSI CDB is TRANSPORT knowledge, exactly as the notify-send argv and WinINet
// flags stayed transport-side. No platform headers here — just fixed-size byte
// arrays a test can compare and the impl can hand straight to sg_io_hdr.cmdp.
//
// Each builder maps one core::ICdDevice primitive to its MMC command, byte-for-
// byte parity with the Windows IOCTL it twins (CdIoWin.cpp). Layouts are the
// canonical MMC-3 CDBs, cross-checked against libcdio's mmc_read_cd / mmc_read_toc.
#pragma once
#include <array>
#include <cstdint>

namespace platform::lnx {

// ── READ CD (0xBE) ────────────────────────────────────────────────────────────
// The audio sector read that feeds AR CRC — the parity crux (design §3).
//   byte 1 = 0x04  : Expected Sector Type = CD-DA (001b) in bits 4-2 (DAP/RELADR 0),
//                    the twin of RAW_READ_INFO.TrackMode = CDDA.
//   bytes 2-5      : Starting LBA, big-endian — the seam's RAW lba, native (NO
//                    Windows *2048 DiskOffset scaling: that's a RAW_READ_INFO
//                    artifact that does not cross the seam).
//   bytes 6-8      : Transfer Length in blocks = sectors.
//   byte 9 flags   : (sync<<7)|(header<<5)|(user_data<<4)|(edc_ecc<<3)|(c2<<1).
//                    User Data (bit 4) always → 2352 audio bytes/sector.
//                    want_c2 sets C2 error block (bits 2-1 = 01b) → +294 C2 bytes,
//                    sector layout [2352 audio][294 c2] (CDRipper de-interleaves it).
//                      no C2 : 0x10  (2352/sector)
//                      C2    : 0x12  (2646/sector)
//   byte 10        : Subchannel Selection = 0 (none).
inline std::array<uint8_t,12> cdbReadCd(uint32_t lba, uint32_t sectors, bool want_c2) {
    return {{
        0xBE,
        0x04,                                  // sector type = CD-DA (001b << 2)
        (uint8_t)((lba >> 24) & 0xFF),
        (uint8_t)((lba >> 16) & 0xFF),
        (uint8_t)((lba >>  8) & 0xFF),
        (uint8_t)( lba        & 0xFF),
        (uint8_t)((sectors >> 16) & 0xFF),
        (uint8_t)((sectors >>  8) & 0xFF),
        (uint8_t)( sectors        & 0xFF),
        (uint8_t)(want_c2 ? 0x12 : 0x10),      // user-data | (C2 error block if want_c2)
        0x00,                                  // subchannel: none
        0x00                                   // control
    }};
}

// ── READ TOC/PMA/ATIP (0x43) ──────────────────────────────────────────────────
// Twins IOCTL_CDROM_READ_TOC (format 0) and IOCTL_CDROM_GET_LAST_SESSION (format 1)
// — both are this command under cdrom.sys.
//   byte 1 = 0x02 : MSF = 1 (bit 1) — MSF addresses, as the Windows IOCTLs return.
//   byte 2        : Format in bits 3-0 (0 = formatted TOC, 1 = multi-session).
//   byte 6        : Starting Track/Session = 0 (from the beginning). The parsed
//                   TOC is gate-validated against the known Relish layout.
//   bytes 7-8     : Allocation length, big-endian (804 = 4 hdr + 100*8 descriptors).
inline std::array<uint8_t,10> cdbReadToc(uint8_t format, uint16_t alloc_len) {
    return {{
        0x43,
        0x02,                                  // MSF = 1
        (uint8_t)(format & 0x0F),
        0x00, 0x00, 0x00,
        0x00,                                  // starting track/session
        (uint8_t)((alloc_len >> 8) & 0xFF),
        (uint8_t)( alloc_len       & 0xFF),
        0x00                                   // control
    }};
}

// TOC allocation: 4-byte header + up to 100 (Red Book tracks 1..99 + lead-out)
// 8-byte descriptors. Matches core::CdToc::SLOTS.
static constexpr uint16_t kTocAllocLen = 4 + 100 * 8;   // 804

// ── SET CD SPEED (0xBB) ───────────────────────────────────────────────────────
// Twins IOCTL_CDROM_SET_SPEED. Same KB/s units (1x = 176, 0xFFFF = max).
//   bytes 2-3 : Read Speed, big-endian = read_kbps.
//   bytes 4-5 : Write Speed = 0 (mirrors the Windows CDROM_SET_SPEED WriteSpeed
//               zero-init). Fire-and-forget, result ignored.
inline std::array<uint8_t,12> cdbSetCdSpeed(uint16_t read_kbps) {
    return {{
        0xBB,
        0x00,
        (uint8_t)((read_kbps >> 8) & 0xFF),
        (uint8_t)( read_kbps       & 0xFF),
        0x00, 0x00,                            // write speed = 0
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00                                   // control
    }};
}

// ── INQUIRY (0x12) ────────────────────────────────────────────────────────────
// Twins IOCTL_STORAGE_QUERY_PROPERTY for model(). Standard INQUIRY: response
// bytes 8-15 = Vendor ID, 16-31 = Product ID; the consumer trims+joins to
// "VENDOR PRODUCT" (the AR offset lookup keys on it — design §3 risk 1).
//   byte 1 = 0 : EVPD = 0 (standard data).  byte 2 = 0 : page code.
//   bytes 3-4  : Allocation length, big-endian.
inline std::array<uint8_t,6> cdbInquiry(uint16_t alloc_len) {
    return {{
        0x12,
        0x00,
        0x00,
        (uint8_t)((alloc_len >> 8) & 0xFF),
        (uint8_t)( alloc_len       & 0xFF),
        0x00
    }};
}

// INQUIRY allocation: standard data is 36 bytes; 96 is the conventional safe
// request (covers Vendor+Product+Revision with margin).
static constexpr uint16_t kInquiryAllocLen = 96;

// ── TEST UNIT READY (0x00) ────────────────────────────────────────────────────
// Twins IOCTL_CDROM_CHECK_VERIFY for mediaPresent(). All-zero 6-byte CDB, no data
// transfer: GOOD status = ready/present, CHECK CONDITION = not-ready/no-media.
inline std::array<uint8_t,6> cdbTestUnitReady() {
    return {{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }};
}

// ── START STOP UNIT (0x1B) — eject ────────────────────────────────────────────
// Twins IOCTL_STORAGE_EJECT_MEDIA for eject(). Byte 4 = LoEj|Start bits: 0x02 =
// LoEj set, Start clear -> stop the disc and open the tray. No data transfer.
inline std::array<uint8_t,6> cdbEject() {
    return {{ 0x1B, 0x00, 0x00, 0x00, 0x02, 0x00 }};
}

} // namespace platform::lnx
