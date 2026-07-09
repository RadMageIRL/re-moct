// ICdIo.h — platform-neutral CD-device seam (Phase 1 slice 8, the LAST platform seam).
//
// `core::ICdIo` is the interface; `platform::win::WinCdIo` (src/platform/win/CdIoWin.cpp)
// is the Windows IOCTL implementation. The Linux sibling (Phase 3, src/platform/linux/)
// is SG_IO on /dev/srN: READ CD (0xBE) for readRaw ± C2, READ TOC (0x43, TIME=1 → MSF;
// format 01 → session info), TEST UNIT READY for mediaPresent, INQUIRY for model,
// SET CD SPEED (0xBB — same KB/s units) for setSpeed. Every primitive maps one-to-one.
//
// The abstraction is "raw transport to an optical drive", modeled on what the two real
// consumers (CDSource playback, CDRipper rip/AR) need — NOT a general MMC framework.
// ALL rip and disc logic stays consumer-side: AR CRC math, the 150-sector preamble,
// offset handling (normalizeSkip / arPreambleReadable), disc-ID normalization, the
// Enhanced-CD silence search and +174 correction, C2 de-interleave and the C2 probe's
// interpretation, retry/silence-fill policy, cache-flush policy, MSF→LBA conversion,
// and the drive-offset table. Only "issue this device request" lives behind the seam —
// the same protocol/transport split as IHttp / IIpc / INotify.
//
// This header must NOT include <windows.h> or any platform header — that is the
// "does core stay portable" seam test.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace core {

// One TOC slot, as the drive delivers it (SCSI READ TOC response shape — not a
// Windows invention): track number (0xAA = lead-out), the ADR/control byte
// (bit 2 set = data track), and the raw MSF address {min, sec, frame}. MSF→LBA
// math deliberately stays consumer-side (CDSource's msf_to_lba, with its
// "do NOT subtract 150" contract).
struct CdTocEntry {
    uint8_t track   = 0;
    uint8_t control = 0;
    uint8_t msf[3]  = {0, 0, 0};
};

// Table of contents. Slot layout mirrors the drive's response: entries[t-1] for
// track t in [first, last], entries[last] = the lead-out descriptor. 100 slots is
// the contract (Red Book: tracks 1..99 + lead-out); consumers must clamp
// first/last before indexing — a corrupt disc can report anything.
struct CdToc {
    static constexpr int SLOTS = 100;
    uint8_t first = 0;
    uint8_t last  = 0;
    std::array<CdTocEntry, SLOTS> entries{};
};

// One open optical device. Destroying it closes the OS handle — and nothing more.
// Error reporting is bool-only, no OS error codes (the IHttp/IIpc/INotify contract).
// Concurrent opens of the same drive are supported and load-bearing: CDSource holds
// its device for playback while CDRipper opens a second one for every rip.
class ICdDevice {
public:
    virtual ~ICdDevice() = default;

    // Read the disc TOC. false = no disc / device error.
    virtual bool readToc(CdToc& out) = 0;

    // First track number of the LAST session (multi-session / Enhanced CD detect:
    // > 1 means a later session exists). false = no session info available.
    virtual bool lastSessionFirstTrack(uint8_t& out) = 0;

    // Read `sectors` raw CDDA sectors starting at `lba` into out (out_size bytes),
    // reporting the byte count actually delivered in `got`. want_c2 requests
    // 2352 audio + 294 C2 error bytes per sector, interleaved [audio][c2] — the
    // SCSI READ CD layout. The flag is advisory on Windows (IOCTL_CDROM_RAW_READ
    // returns C2 iff the drive supports it and out_size is 2646/sector — the
    // baseline's exact call shape, buffer passed through untouched); it exists so
    // the SG_IO impl can set the CDB error-field bits, which buffer size alone
    // cannot express. Callers detect C2 delivery from `got`, as the baseline did.
    virtual bool readRaw(uint32_t lba, uint32_t sectors, bool want_c2,
                         void* out, std::size_t out_size, std::size_t& got) = 0;

    // Set the drive read speed in KB/s (0xFFFF = max, 176 = 1x). Fire-and-forget,
    // best-effort, no error reporting — the rip path uses it to force real platter
    // re-reads between verification passes.
    virtual void setSpeed(uint16_t read_kbps) = 0;

    // Non-blocking media-present check (drive door / disc gone detection).
    virtual bool mediaPresent() = 0;

    // Eject the tray (TUI Shift+E). Best-effort: true = the drive accepted the
    // command. Defaulted (not pure) so existing test fakes keep compiling — a
    // fake that never ejects is the correct default.
    virtual bool eject() { return false; }

    // Drive identification, "VENDOR PRODUCT" (feeds the AccurateRip offset table
    // lookup, which stays consumer-side). Empty string = unknown.
    virtual std::string model() = 0;
};

// Device factory. open() takes a LOGICAL drive spec ("D"); the platform impl owns
// the spec→path mapping (\\.\D: on Windows, /dev/srN when the Linux impl exists) —
// the path shape is exactly as platform-specific as the transport itself.
// Returns nullptr when the drive can't be opened.
class ICdIo {
public:
    virtual ~ICdIo() = default;
    virtual std::unique_ptr<ICdDevice> open(const std::string& drive) = 0;
};

// Link-time bridge to the platform transport (defined in the impl TU): core code
// cannot #include a platform header, so the production default is reached by name.
// Deliberately NOT the http()/setHttp() transitional-global pattern — there is no
// setCdio(): both consumers (CDSource, CDRipper) take an ICdIo* by CONSTRUCTOR
// INJECTION (tests pass fakes there), and this accessor only supplies their
// production default. Endgame unchanged: the host hands consumers/plugins their
// services (docs/architecture.md).
ICdIo& cdio();

} // namespace core
