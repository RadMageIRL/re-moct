// CdIoSgIo.cpp — Linux SG_IO implementation of core::ICdIo (Phase 3 slice 6,
// the LAST seam). The sibling of platform::win::WinCdIo (src/platform/win/
// CdIoWin.cpp): every method is ONE SCSI CDB issued via ioctl(fd, SG_IO, &hdr),
// twinning the one baseline DeviceIoControl it replaces, byte-for-byte.
//
// No policy lives here: retry, C2 interpretation/de-interleave, TOC parsing into
// LBAs, disc-ID normalization, the 150 preamble, offset handling, and all rip
// math stay in the consumers (CDRipper/CDSource), exactly as on Windows. Only
// "issue this device request" is behind the seam. The CDB byte layouts live in
// CdbSgIo.h (asserted headless by cdb_sgio_test); this file is the transport that
// carries them to /dev/srN and marshals the responses back into core:: types.
#ifdef __linux__
#include "core/ICdIo.h"
#include "CdbSgIo.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <memory>

namespace platform::lnx {

// Per-command timeouts (ms). NOT a parity knob for the BYTES — Windows uses
// cdrom.sys's default; these only bound a wedged command. READ CD gets the most
// headroom so a drive's own re-read/error-recovery on a marginal sector isn't
// cut short (the consumer's retry/silence-fill policy is what actually decides).
static constexpr unsigned kTimeoutReadMs = 20000;
static constexpr unsigned kTimeoutTocMs  = 10000;
static constexpr unsigned kTimeoutMiscMs = 10000;

class SgIoCdDevice final : public core::ICdDevice {
public:
    explicit SgIoCdDevice(int fd) : fd_(fd) {}
    ~SgIoCdDevice() override { if (fd_ >= 0) ::close(fd_); }

    bool readToc(core::CdToc& out) override {
        // READ TOC format 0 (formatted TOC, MSF) — the IOCTL_CDROM_READ_TOC twin.
        // Overwrite the whole struct, as cdrom.sys writes a zero-init CDROM_TOC.
        out = core::CdToc{};
        uint8_t buf[kTocAllocLen];                       // 804
        std::memset(buf, 0, sizeof(buf));
        auto cdb = cdbReadToc(/*format=*/0, kTocAllocLen);
        std::size_t got = 0;
        if (!doScsi(cdb.data(), (unsigned)cdb.size(), SG_DXFER_FROM_DEV,
                    buf, sizeof(buf), &got, kTimeoutTocMs))
            return false;
        // Response: bytes 0-1 = TOC data length (big-endian, excludes those 2),
        // byte 2 = first track, byte 3 = last track, then 8-byte descriptors.
        out.first = buf[2];
        out.last  = buf[3];
        unsigned data_len = ((unsigned)buf[0] << 8) | buf[1];
        unsigned ndesc = (data_len >= 2) ? (data_len - 2) / 8 : 0;
        unsigned from_got = (got >= 4) ? (unsigned)((got - 4) / 8) : 0;
        if (ndesc > from_got) ndesc = from_got;          // never read past what came back
        if (ndesc > (unsigned)core::CdToc::SLOTS) ndesc = core::CdToc::SLOTS;
        // Copy descriptors in RESPONSE ORDER (how cdrom.sys fills TrackData[]).
        // Descriptor: [0]reserved [1]ADR/Control [2]track# [3]reserved
        //             [4]reserved [5]M [6]S [7]F   (MSF=1).
        // control = low nibble only, matching Windows TRACK_DATA.Control (:4 bitfield);
        // the consumer tests control & 0x04 (data-track bit) — CDSource.cpp:69.
        for (unsigned i = 0; i < ndesc; ++i) {
            const uint8_t* d = buf + 4 + i * 8;
            out.entries[i].track   = d[2];
            out.entries[i].control = (uint8_t)(d[1] & 0x0F);
            out.entries[i].msf[0]  = d[5];
            out.entries[i].msf[1]  = d[6];
            out.entries[i].msf[2]  = d[7];
        }
        return true;
    }

    bool lastSessionFirstTrack(uint8_t& out) override {
        // READ TOC format 1 (multi-session) — the IOCTL_CDROM_GET_LAST_SESSION twin.
        // Response: [0-1]len [2]first-session [3]last-session, then ONE 8-byte
        // descriptor for the first track of the last session. Windows reads
        // TrackData[0].TrackNumber = descriptor byte 2 (= response byte 6).
        uint8_t buf[64];
        std::memset(buf, 0, sizeof(buf));
        auto cdb = cdbReadToc(/*format=*/1, (uint16_t)sizeof(buf));
        std::size_t got = 0;
        if (!doScsi(cdb.data(), (unsigned)cdb.size(), SG_DXFER_FROM_DEV,
                    buf, sizeof(buf), &got, kTimeoutTocMs))
            return false;
        if (got < 7) return false;                       // need through descriptor byte 2
        out = buf[6];
        return true;
    }

    bool readRaw(uint32_t lba, uint32_t sectors, bool want_c2,
                 void* out, std::size_t out_size, std::size_t& got) override {
        // READ CD 0xBE — the audio read that feeds AR CRC. want_c2 sets the CDB's
        // C2 error-field bits (byte 9 = 0x12) → 2646/sector [2352 audio][294 c2];
        // false → 0x10 → 2352/sector. Native LBA (no Windows *2048 DiskOffset).
        // On a non-C2 drive the C2 request fails CHECK CONDITION → ok=false → the
        // probe's `ok && got==2646` is false → the rip runs the 2352 path. got is
        // surfaced (out_size - resid) exactly as the Windows C2 probe needs.
        auto cdb = cdbReadCd(lba, sectors, want_c2);
        return doScsi(cdb.data(), (unsigned)cdb.size(), SG_DXFER_FROM_DEV,
                      out, (unsigned)out_size, &got, kTimeoutReadMs);
    }

    void setSpeed(uint16_t read_kbps) override {
        // SET CD SPEED 0xBB — fire-and-forget, best-effort, no error reporting
        // (the IOCTL_CDROM_SET_SPEED contract). Result ignored.
        auto cdb = cdbSetCdSpeed(read_kbps);
        std::size_t got = 0;
        (void)doScsi(cdb.data(), (unsigned)cdb.size(), SG_DXFER_NONE,
                     nullptr, 0, &got, kTimeoutMiscMs);
    }

    bool mediaPresent() override {
        // TEST UNIT READY 0x00 — the IOCTL_CDROM_CHECK_VERIFY twin. GOOD = ready/
        // present → true; CHECK CONDITION (not ready / no media) → doScsi false.
        auto cdb = cdbTestUnitReady();
        std::size_t got = 0;
        return doScsi(cdb.data(), (unsigned)cdb.size(), SG_DXFER_NONE,
                      nullptr, 0, &got, kTimeoutMiscMs);
    }

    std::string model() override {
        // INQUIRY 0x12 — the IOCTL_STORAGE_QUERY_PROPERTY twin. Standard data:
        // bytes 8-15 = Vendor ID, 16-31 = Product ID; trim + join "VENDOR PRODUCT"
        // with the SAME whitespace trim as CdIoWin.cpp:118-130. The AR drive-offset
        // lookup keys on this string (design §3 risk 1 — the +6 pre-check validates it).
        uint8_t buf[kInquiryAllocLen];                   // 96
        std::memset(buf, 0, sizeof(buf));
        auto cdb = cdbInquiry(kInquiryAllocLen);
        std::size_t got = 0;
        if (!doScsi(cdb.data(), (unsigned)cdb.size(), SG_DXFER_FROM_DEV,
                    buf, sizeof(buf), &got, kTimeoutMiscMs))
            return {};
        if (got < 32) return {};                         // need through Product ID
        auto field = [&](int off, int len) -> std::string {
            int n = 0;                                   // strnlen-style, like Windows getString
            while (n < len && buf[off + n] != 0) ++n;
            std::string s(reinterpret_cast<const char*>(buf) + off, (size_t)n);
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            auto last = s.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) s.erase(last + 1);
            return s;
        };
        std::string vendor  = field(8, 8);
        std::string product = field(16, 16);
        return vendor.empty() ? product : vendor + " " + product;
    }

private:
    // One SCSI command through SG_IO. dxfer_dir = SG_DXFER_FROM_DEV / SG_DXFER_NONE.
    // Returns true only on a clean transfer (SG_INFO_OK: SCSI GOOD, no host/driver
    // error, no sense). got (when non-null) = bytes actually delivered = len - resid.
    bool doScsi(const uint8_t* cdb, unsigned cdb_len, int dxfer_dir,
                void* data, unsigned data_len, std::size_t* got, unsigned timeout_ms) {
        sg_io_hdr_t hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        unsigned char sense[32];
        hdr.interface_id    = 'S';
        hdr.dxfer_direction = dxfer_dir;
        hdr.cmd_len         = (unsigned char)cdb_len;
        hdr.cmdp            = const_cast<unsigned char*>(cdb);
        hdr.mx_sb_len       = sizeof(sense);
        hdr.sbp             = sense;
        hdr.dxfer_len       = data_len;
        hdr.dxferp          = data;
        hdr.timeout         = timeout_ms;

        int rc;
        // Retry on EINTR: ncurses' SIGWINCH (resize) must not read as a device
        // error — the slice-4 signal-is-not-a-broken-channel discipline.
        do { rc = ::ioctl(fd_, SG_IO, &hdr); } while (rc < 0 && errno == EINTR);
        if (rc < 0) { if (got) *got = 0; return false; }

        if (got) {
            int xfer = (int)data_len - hdr.resid;        // resid = requested - delivered
            *got = (xfer > 0) ? (std::size_t)xfer : 0;
        }
        // SG_INFO_OK = GOOD status, no host/driver error, no sense. A recovered
        // error (valid data + sense) reads as CHECK here → a benign extra consumer
        // retry that re-reads the SAME bytes; never returns wrong bytes. The clean
        // gate disc exercises neither branch.
        return (hdr.info & SG_INFO_OK_MASK) == SG_INFO_OK;
    }

    int fd_;
};

class SgIoCdIo final : public core::ICdIo {
public:
    std::unique_ptr<core::ICdDevice> open(const std::string& drive) override {
        // Spec -> device node. Accept "sr0", "/dev/sr0", or any absolute device
        // path (the impl owns the mapping, as \\.\D: is Windows-owned). The
        // UIManager Linux drive-list passes "srN".
        std::string path;
        if (!drive.empty() && drive[0] == '/') path = drive;         // absolute path as-is
        else                                   path = "/dev/" + drive;
        // O_RDONLY|O_NONBLOCK: don't block on a spinning-up / empty drive (SG_IO
        // still works on the fd). O_CLOEXEC: don't leak the drive fd across the
        // notify-send / xdg-open fork+exec.
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return nullptr;
        return std::make_unique<SgIoCdDevice>(fd);
    }
};

} // namespace platform::lnx

namespace core {

// Production default transport (see ICdIo.h — reached by name because core code
// can't include this TU's headers). Moved here from SeamStubs.cpp, which dies with
// this slice. Function-local static: thread-safe init; SgIoCdIo is stateless,
// devices own their fds.
ICdIo& cdio() {
    static platform::lnx::SgIoCdIo instance;
    return instance;
}

} // namespace core

#endif // __linux__
