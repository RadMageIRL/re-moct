// cdb_sgio_test.cpp — Linux-only unit test for the SG_IO CD-seam CDB builders
// (Phase 3 slice 6). Asserts the EXACT SCSI CDB bytes headless — the byte-exact
// heart of the parity mapping, the analog of notify_argv_test. This is where the
// C2 READ CD (byte 9 = 0x12) becomes a TESTED artifact even though the non-C2
// gate drive (GHD3N) can never run it live (design §3 / §8.3). A fake/CDB test
// cannot prove the real drive returns the right platter bytes — that is the Joan
// Osborne Relish gate. What THIS proves: we build the right requests.
// Returns nonzero on failure.
#include "CdbSgIo.h"
#include <cstdio>

using namespace platform::lnx;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

int main() {
    // ── READ CD 0xBE — the audio read that feeds AR CRC (the crux) ─────────────
    // (1) no-C2: CD-DA sector type, user-data only (2352/sector), LBA big-endian.
    {
        // Relish T1 @ LBA 32; a distinctive multi-byte LBA also proves byte order.
        auto c = cdbReadCd(32, 1, /*want_c2=*/false);
        CHECK(c[0] == 0xBE);
        CHECK(c[1] == 0x04);          // Expected Sector Type = CD-DA (001b << 2)
        CHECK(c[2] == 0x00);          // LBA 32 = 0x00000020, big-endian
        CHECK(c[3] == 0x00);
        CHECK(c[4] == 0x00);
        CHECK(c[5] == 0x20);
        CHECK(c[6] == 0x00);          // transfer length = 1 block
        CHECK(c[7] == 0x00);
        CHECK(c[8] == 0x01);
        CHECK(c[9] == 0x10);          // user-data (bit 4), NO C2
        CHECK(c[10] == 0x00);         // subchannel: none
        CHECK(c[11] == 0x00);
    }
    // (2) LBA + transfer-length big-endian encoding across all bytes.
    {
        auto c = cdbReadCd(0x00A1B2C3u, 0x001122u, /*want_c2=*/false);
        CHECK(c[2] == 0x00 && c[3] == 0xA1 && c[4] == 0xB2 && c[5] == 0xC3);
        CHECK(c[6] == 0x00 && c[7] == 0x11 && c[8] == 0x22);
    }
    // (3) C2: byte 9 flips to 0x12 (user-data | C2 error block 01b) → 2646/sector,
    //     layout [2352 audio][294 c2]. Everything else identical to the no-C2 CDB.
    //     THE artifact the gate drive can't exercise live.
    {
        auto c = cdbReadCd(0, 27, /*want_c2=*/true);
        CHECK(c[0] == 0xBE);
        CHECK(c[1] == 0x04);          // still CD-DA
        CHECK(c[9] == 0x12);          // user-data | C2 error block (bits 2-1 = 01b)
        CHECK(c[6] == 0x00 && c[7] == 0x00 && c[8] == 27);
    }

    // ── READ TOC 0x43 — format 0 (formatted TOC) and 1 (multi-session) ─────────
    {
        auto c = cdbReadToc(/*format=*/0, kTocAllocLen);
        CHECK(c[0] == 0x43);
        CHECK(c[1] == 0x02);          // MSF = 1 (bit 1)
        CHECK(c[2] == 0x00);          // format 0 = formatted TOC
        CHECK(c[6] == 0x00);          // starting track = from the beginning
        CHECK(c[7] == 0x03 && c[8] == 0x24);   // alloc 804 = 0x0324, big-endian
        CHECK(kTocAllocLen == 804);
    }
    {
        // format 1 = IOCTL_CDROM_GET_LAST_SESSION twin (Enhanced-CD detect).
        auto c = cdbReadToc(/*format=*/1, kTocAllocLen);
        CHECK(c[1] == 0x02);          // MSF still 1
        CHECK(c[2] == 0x01);          // format 1 = multi-session
    }

    // ── SET CD SPEED 0xBB — read speed KB/s big-endian, write speed 0 ──────────
    {
        auto c = cdbSetCdSpeed(0xFFFF);       // max (CDSource::open at :137)
        CHECK(c[0] == 0xBB);
        CHECK(c[2] == 0xFF && c[3] == 0xFF);  // read speed = 0xFFFF, big-endian
        CHECK(c[4] == 0x00 && c[5] == 0x00);  // write speed = 0 (Windows zero-init)
    }
    {
        auto c = cdbSetCdSpeed(176);          // 1x
        CHECK(c[2] == 0x00 && c[3] == 0xB0);  // 176 = 0x00B0
    }

    // ── INQUIRY 0x12 — model() ────────────────────────────────────────────────
    {
        auto c = cdbInquiry(kInquiryAllocLen);
        CHECK(c[0] == 0x12);
        CHECK(c[1] == 0x00);          // EVPD = 0 (standard data)
        CHECK(c[2] == 0x00);          // page code 0
        CHECK(c[3] == 0x00 && c[4] == 96);    // alloc 96, big-endian
        CHECK(kInquiryAllocLen == 96);
    }

    // ── TEST UNIT READY 0x00 — mediaPresent() ─────────────────────────────────
    {
        auto c = cdbTestUnitReady();
        CHECK(c.size() == 6);
        for (auto b : c) CHECK(b == 0x00);
    }

    if (g_fail) { std::printf("%d FAILURE(S)\n", g_fail); return 1; }
    std::printf("cdb_sgio_test: all checks passed\n");
    return 0;
}
