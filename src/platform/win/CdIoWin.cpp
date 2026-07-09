// CdIoWin.cpp — Windows IOCTL implementation of core::ICdIo (Phase 1 slice 8).
//
// Every method is one baseline DeviceIoControl, moved here parameter-identical from
// CDSource.cpp / CDRipper.cpp. No policy lives in this file: retry, C2 interpretation,
// de-interleave, TOC parsing, and all rip math stay in the consumers.
#ifdef _WIN32
#include "core/ICdIo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddstor.h>

#include <cstring>
#include <vector>

namespace platform { namespace win {

// The interface promises 100 TOC slots (core::CdToc::SLOTS); Windows must provide
// at least that many to copy from. (Guard moved from CDSource.cpp, where it
// protected the same indexing.)
static_assert(MAXIMUM_NUMBER_TRACKS >= core::CdToc::SLOTS,
              "CDROM_TOC TrackData too small for the CdToc contract");

class WinCdDevice final : public core::ICdDevice {
public:
    explicit WinCdDevice(HANDLE h) : h_(h) {}
    ~WinCdDevice() override {
        if (h_ != INVALID_HANDLE_VALUE) {
            // Explicitly clear any soft media-removal lock before closing. Some
            // drives (HL-DT-ST/LG/HLDS family) hold the tray after raw reads
            // until told to release; a bare CloseHandle leaves the physical
            // eject button dead for several seconds ("push it repeatedly").
            // Best-effort by design: the return value is ignored - a drive that
            // doesn't need or support this errors harmlessly, and we are closing
            // either way. Fires on every dev_.reset() (stop/closeCD/eject), i.e.
            // whenever RE-MOCT lets go of the drive - exactly when the tray
            // should become the user's again.
            PREVENT_MEDIA_REMOVAL pmr {};
            pmr.PreventMediaRemoval = FALSE;
            DWORD bytes = 0;
            DeviceIoControl(h_, IOCTL_STORAGE_MEDIA_REMOVAL,
                            &pmr, sizeof(pmr), nullptr, 0, &bytes, nullptr);
            CloseHandle(h_);
        }
    }

    bool readToc(core::CdToc& out) override {
        CDROM_TOC toc {};
        DWORD bytes = 0;
        if (!DeviceIoControl(h_, IOCTL_CDROM_READ_TOC,
                             nullptr, 0, &toc, sizeof(toc), &bytes, nullptr))
            return false;
        out.first = toc.FirstTrack;
        out.last  = toc.LastTrack;
        // Slot-for-slot copy: entries[0..last-1] = tracks, entries[last] = lead-out.
        // Address[] = {reserved, M, S, F}; MSF handed over raw — LBA math is the
        // consumer's (CDSource's msf_to_lba, "do NOT subtract 150").
        for (int i = 0; i < core::CdToc::SLOTS; ++i) {
            const TRACK_DATA& td = toc.TrackData[i];
            out.entries[i].track   = td.TrackNumber;
            out.entries[i].control = td.Control;
            out.entries[i].msf[0]  = td.Address[1];
            out.entries[i].msf[1]  = td.Address[2];
            out.entries[i].msf[2]  = td.Address[3];
        }
        return true;
    }

    bool lastSessionFirstTrack(uint8_t& out) override {
        CDROM_TOC_SESSION_DATA last_sess {};
        DWORD ls_bytes = 0;
        if (!DeviceIoControl(h_, IOCTL_CDROM_GET_LAST_SESSION,
                             nullptr, 0, &last_sess, sizeof(last_sess), &ls_bytes, nullptr))
            return false;
        out = last_sess.TrackData[0].TrackNumber;
        return true;
    }

    bool readRaw(uint32_t lba, uint32_t sectors, bool /*want_c2*/,
                 void* out, std::size_t out_size, std::size_t& got) override {
        // want_c2 has no Windows expression: IOCTL_CDROM_RAW_READ returns C2 iff the
        // drive supports it AND the output buffer is sized 2646/sector — the caller's
        // buffer size, passed through untouched, IS the request (the baseline's exact
        // shape). The flag exists for the SG_IO impl (Phase 3), where C2 must be
        // requested via CDB bits. Callers detect C2 delivery from `got`.
        RAW_READ_INFO info {};
        // Windows quirk: DiskOffset uses 2048-byte sector scale even for audio.
        info.DiskOffset.QuadPart = (ULONGLONG)lba * 2048;
        info.SectorCount         = (ULONG)sectors;
        info.TrackMode           = CDDA;
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(h_, IOCTL_CDROM_RAW_READ,
                                  &info, sizeof(info),
                                  out, (DWORD)out_size, &bytes, nullptr);
        got = bytes;
        return ok != 0;
    }

    void setSpeed(uint16_t read_kbps) override {
        // Canonical CDROM_SET_SPEED — CDRipper's baseline shape, byte-identical
        // (WriteSpeed/rotation zero-init, exactly as its {} init did). CDSource's old
        // hand-rolled 6-byte variant was probe-confirmed rejected by cdrom.sys with
        // ERROR_BAD_LENGTH (a lifelong no-op) and was dropped, not migrated.
        CDROM_SET_SPEED s = {};
        s.RequestType = CdromSetSpeed;
        s.ReadSpeed   = read_kbps;
        DWORD dummy = 0;
        DeviceIoControl(h_, IOCTL_CDROM_SET_SPEED,
                        &s, sizeof(s), nullptr, 0, &dummy, nullptr);
    }

    bool mediaPresent() override {
        DWORD dummy = 0;
        return DeviceIoControl(h_, IOCTL_CDROM_CHECK_VERIFY,
                               nullptr, 0, nullptr, 0, &dummy, nullptr) != 0;
    }

    bool eject() override {
        // Sequence: unlock -> eject -> on FAILURE unlock again. Invariant
        // (HL-DT regression, found on hardware): a failed
        // IOCTL_STORAGE_EJECT_MEDIA must never leave the tray locked - on this
        // family the failed eject itself leaves the soft lock ASSERTED, and the
        // physical button goes dead. The destructor's unconditional unlock still
        // runs at close as the last line of defence. On this firmware even that
        // may not release a post-failed-eject wedge (only a real play->stop
        // cycle does); recovery attempts beyond the unlocks were tried and
        // reverted - see ejectDrive in UIManager.cpp for the history.
        PREVENT_MEDIA_REMOVAL pmr {};
        pmr.PreventMediaRemoval = FALSE;
        DWORD bytes = 0;
        DeviceIoControl(h_, IOCTL_STORAGE_MEDIA_REMOVAL,
                        &pmr, sizeof(pmr), nullptr, 0, &bytes, nullptr);
        bool ok = DeviceIoControl(h_, IOCTL_STORAGE_EJECT_MEDIA,
                                  nullptr, 0, nullptr, 0, &bytes, nullptr) != 0;
        if (!ok)
            DeviceIoControl(h_, IOCTL_STORAGE_MEDIA_REMOVAL,
                            &pmr, sizeof(pmr), nullptr, 0, &bytes, nullptr);
        return ok;
    }

    std::string model() override {
        // Moved byte-identical from CDSource::queryDriveModel (the
        // STORAGE_DEVICE_DESCRIPTOR parse is Windows shape; the offset-table
        // lookup the string feeds stays consumer-side).
        std::vector<uint8_t> buf(512, 0);
        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType  = PropertyStandardQuery;
        DWORD bytes = 0;
        if (!DeviceIoControl(h_, IOCTL_STORAGE_QUERY_PROPERTY,
                             &query, sizeof(query),
                             buf.data(), (DWORD)buf.size(), &bytes, nullptr))
            return {};
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        auto getString = [&](DWORD offset) -> std::string {
            if (offset == 0 || offset >= bytes) return {};
            const char* p = reinterpret_cast<const char*>(buf.data()) + offset;
            size_t len = strnlen(p, bytes - offset);
            std::string s(p, len);
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            auto last = s.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) s.erase(last + 1);
            return s;
        };
        std::string vendor  = getString(desc->VendorIdOffset);
        std::string product = getString(desc->ProductIdOffset);
        return vendor.empty() ? product : vendor + " " + product;
    }

private:
    HANDLE h_;
};

class WinCdIo final : public core::ICdIo {
public:
    std::unique_ptr<core::ICdDevice> open(const std::string& drive) override {
        // Drive spec "D" -> \\.\D: (ASCII drive letters; widen byte-wise).
        // Unified open flags (slice-8 survey, F2): GENERIC_READ + OPEN_EXISTING as
        // both baselines; FILE_SHARE_READ|FILE_SHARE_WRITE (CDRipper's baseline —
        // wider than CDSource's READ-only share, inert: nothing ever opens the
        // device for write); FILE_ATTRIBUTE_NORMAL (CDSource's SEQUENTIAL_SCAN
        // dropped — a ReadFile read-ahead hint, inert under DeviceIoControl-only
        // access). Concurrent opens of one drive are contract: CDSource holds its
        // device for playback while CDRipper opens a second one per rip.
        std::wstring path = L"\\\\.\\";
        for (unsigned char c : drive) path += (wchar_t)c;
        path += L":";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        return std::make_unique<WinCdDevice>(h);
    }
};

}} // namespace platform::win

namespace core {

// Production default transport (see ICdIo.h — reached by name because core code
// can't include this TU's headers). Function-local static: thread-safe init;
// WinCdIo is stateless, devices own their handles.
ICdIo& cdio() {
    static platform::win::WinCdIo instance;
    return instance;
}

} // namespace core

#endif // _WIN32
