#ifdef _WIN32
#include "CDSource.h"
#include "drive_offsets.h"   // full AccurateRip drive-offset DB (4885 drives)
#include <cstring>
#include <cmath>
#include <algorithm>

// ─── Open / Close ─────────────────────────────────────────────────────────────

bool CDSource::open(const std::string& drive_letter) {
    close();
    drive_letter_ = drive_letter;

    // Open raw device handle
    std::wstring path = L"\\\\.\\" + std::wstring(drive_letter.begin(), drive_letter.end()) + L":";
    hCD_ = CreateFileW(path.c_str(),
                       GENERIC_READ,
                       FILE_SHARE_READ,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr);

    if (hCD_ == INVALID_HANDLE_VALUE) return false;

    // Query drive model and look up AccurateRip read offset
    drive_model_          = queryDriveModel();
    drive_offset_samples_ = lookupDriveOffset(drive_model_);

    // Read Table of Contents
    CDROM_TOC toc {};
    DWORD bytes = 0;
    if (!DeviceIoControl(hCD_, IOCTL_CDROM_READ_TOC,
                         nullptr, 0, &toc, sizeof(toc), &bytes, nullptr)) {
        CloseHandle(hCD_);
        hCD_ = INVALID_HANDLE_VALUE;
        return false;
    }

    tracks_.clear();
    data_track_lbas_.clear();
    full_leadout_lba_ = 0;
    int first = toc.FirstTrack;
    int last  = toc.LastTrack;

    // Guard against a malformed TOC: TrackData has MAXIMUM_NUMBER_TRACKS (100)
    // slots, and the lead-out for an N-track disc sits at index N, so N must be
    // <= 99. FirstTrack/LastTrack are UCHARs, so a corrupt disc could report
    // values that would over-read the array below.
    static_assert(MAXIMUM_NUMBER_TRACKS >= 100, "CDROM_TOC TrackData too small");
    if (first < 1)  first = 1;
    if (last  > 99) last  = 99;
    if (last < first) {
        CloseHandle(hCD_);
        hCD_ = INVALID_HANDLE_VALUE;
        return false;
    }

    // Helper: convert MSF address to LBA (sectors from disc start)
    // TOC Address[] = {0, minutes, seconds, frames}
    auto msf_to_lba = [](const UCHAR addr[4]) -> DWORD {
        return (DWORD)(addr[1]) * 60 * 75
             + (DWORD)(addr[2]) * 75
             + (DWORD)(addr[3]);
        // Note: do NOT subtract 150 here — Windows TOC addresses
        // are already absolute (include the 2s lead-in).
        // We subtract 150 when computing the read offset to get
        // the actual audio data start.
    };

    // ── Capture data tracks from TOC (for CDDB computation and disc ID) ──
    for (int t = first; t <= last; ++t) {
        auto& te = toc.TrackData[t - 1];
        if ((te.Control & 0x04) != 0)
            data_track_lbas_.push_back(msf_to_lba(te.Address));
    }

    for (int t = first; t <= last; ++t) {
        auto& te = toc.TrackData[t - 1];
        // Skip data tracks (control bit 4 set = data)
        if ((te.Control & 0x04) != 0) continue;

        DWORD start = msf_to_lba(te.Address);
        DWORD end   = msf_to_lba(toc.TrackData[t].Address); // next track or leadout

        DWORD len = (end > start) ? (end - start) : 0;

        CDTrack ct;
        ct.number       = t;
        ct.start_lba    = start;
        ct.length_lba   = len;
        ct.duration_sec = (int)(len / 75);
        tracks_.push_back(ct);
    }

    if (tracks_.empty()) {
        CloseHandle(hCD_);
        hCD_ = INVALID_HANDLE_VALUE;
        return false;
    }



    // Standard TOC leadout = toc.TrackData[last] (first track after last)
    full_leadout_lba_ = msf_to_lba(toc.TrackData[last].Address);

    // For multi-session discs (Enhanced CD / CD Extra), get the last session
    // leadout via IOCTL_CDROM_GET_LAST_SESSION — more reliable than FULL_TOC.
    CDROM_TOC_SESSION_DATA last_sess {};
    DWORD ls_bytes = 0;
    if (DeviceIoControl(hCD_, IOCTL_CDROM_GET_LAST_SESSION,
                        nullptr, 0, &last_sess, sizeof(last_sess), &ls_bytes, nullptr)
        && last_sess.TrackData[0].TrackNumber > 1) {
        // There is a second session — read its TOC to get the real leadout
        CDROM_TOC toc2 {};
        DWORD bytes2 = 0;
        if (DeviceIoControl(hCD_, IOCTL_CDROM_READ_TOC,
                            nullptr, 0, &toc2, sizeof(toc2), &bytes2, nullptr)) {
            // toc2 may return same as toc1 on some drives; take the larger leadout
            int last2 = toc2.LastTrack;
            DWORD lo2 = msf_to_lba(toc2.TrackData[last2].Address);
            if (lo2 > full_leadout_lba_)
                full_leadout_lba_ = lo2;
            // Also capture any data tracks from session 2 not in session 1
            for (int t = toc2.FirstTrack; t <= last2; ++t) {
                auto& te2 = toc2.TrackData[t - 1];
                DWORD lba2 = msf_to_lba(te2.Address);
                if ((te2.Control & 0x04) != 0) {
                    // Only add if not already in list
                    bool already = false;
                    for (DWORD d : data_track_lbas_) if (d == lba2) { already = true; break; }
                    if (!already) data_track_lbas_.push_back(lba2);
                }
            }
        }
    }

    // Reset speed to max after any disc scanning done in open()
    // Enhanced CD track length correction is done lazily in CDRipper at rip time.
    {
        struct { WORD RequestType; WORD ReadSpeed; WORD WriteSpeed; } speed {};
        speed.RequestType = 0;       // immediate
        speed.ReadSpeed   = 0xFFFF;  // maximum
        speed.WriteSpeed  = 0xFFFF;
        DWORD sp_bytes = 0;
        DeviceIoControl(hCD_, IOCTL_CDROM_SET_SPEED,
                        &speed, sizeof(speed), nullptr, 0, &sp_bytes, nullptr);
    }

    ring_.resize(RING_SIZE, 0);
    return true;
}

void CDSource::close() {
    data_track_lbas_.clear();
    full_leadout_lba_ = 0;
    stop();
    if (hCD_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hCD_);
        hCD_ = INVALID_HANDLE_VALUE;
    }
    tracks_.clear();
    media_removed_.store(false);
    ring_write_.store(0);
    ring_read_.store(0);
}

// ─── Playback ─────────────────────────────────────────────────────────────────

bool CDSource::playTrack(int track_number) {
    const CDTrack* ct = nullptr;
    for (auto& t : tracks_)
        if (t.number == track_number) { ct = &t; break; }
    if (!ct) return false;

    // Signal reader to stop and wait for it
    reader_stop_.store(true);
    if (reader_thread_.joinable()) reader_thread_.join();

    // Now safe to reset everything — audio callback outputs silence
    // while current_track_ is 0 (between stop and start)
    current_track_.store(0);  // briefly zero so callback outputs silence
    ring_write_.store(0);
    ring_read_.store(0);

    // Set new track params
    current_track_.store(track_number);
    current_lba_.store(ct->start_lba);
    track_end_lba_.store(ct->start_lba + ct->length_lba);

    // Start fresh
    playing_.store(true);
    paused_.store(false);
    reader_stop_.store(false);
    media_removed_.store(false);

    reader_thread_ = std::thread(&CDSource::readerWorker, this);
    return true;
}

void CDSource::stop() {
    current_track_.store(0);  // zero first so callback stops signalling track_ended
    reader_stop_.store(true);
    playing_.store(false);
    if (reader_thread_.joinable()) reader_thread_.join();
}

// ─── Background reader thread ─────────────────────────────────────────────────

void CDSource::readerWorker() {
    static constexpr int BUF_SECTORS = SECTORS_PER_READ;
    static constexpr int BUF_BYTES   = SECTOR_BYTES * BUF_SECTORS;
    std::vector<uint8_t> buf(BUF_BYTES);
    int consecutive_errors = 0;

    while (!reader_stop_.load()) {
        if (paused_.load()) { Sleep(20); continue; }

        DWORD lba = current_lba_.load();
        if (lba >= track_end_lba_.load()) {
            playing_.store(false);
            break;
        }

        // Throttle if ring is more than half full
        int avail = ringAvailable();
        if (avail > RING_SIZE / 2) { Sleep(10); continue; }

        DWORD remaining = track_end_lba_.load() - lba;
        int   count     = std::min((DWORD)BUF_SECTORS, remaining);
        if (count == 0) break;

        if (!readSectors(lba, count, buf.data())) {
            ++consecutive_errors;

            // Check if media is still present
            DWORD dummy = 0;
            bool media_ok = DeviceIoControl(hCD_, IOCTL_CDROM_CHECK_VERIFY,
                                            nullptr, 0, nullptr, 0, &dummy, nullptr) != 0;
            if (!media_ok || consecutive_errors >= 10) {
                // Drive ejected or unreadable — hard stop
                // Flush ring so callback outputs silence immediately
                ring_write_.store(ring_read_.load());
                current_track_.store(0);
                playing_.store(false);
                media_removed_.store(true);
                break;
            }

            // Transient read error — output silence and skip sector
            int silence_samples = count * SECTOR_SAMPLES * 2;
            std::vector<int16_t> silence(silence_samples, 0);
            ringWrite(silence.data(), silence_samples);
            current_lba_.store(lba + count);
            Sleep(50);  // brief back-off before retry
            continue;
        }

        consecutive_errors = 0;
        int samples = count * SECTOR_SAMPLES * 2;
        ringWrite(reinterpret_cast<const int16_t*>(buf.data()), samples);
        current_lba_.store(lba + count);
    }
}

void CDSource::seekTo(int seconds) {
    if (current_track_.load() == 0) return;
    // Find track start LBA
    DWORD track_start = 0;
    for (auto& t : tracks_)
        if (t.number == current_track_.load()) { track_start = t.start_lba; break; }

    // Calculate target LBA: 75 sectors per second
    DWORD target_lba = track_start + (DWORD)(seconds * 75);
    DWORD track_end = track_end_lba_.load();
    if (track_end > 0 && target_lba >= track_end) target_lba = track_end - 1;

    // Stop reader, flush ring, seek, restart
    reader_stop_.store(true);
    if (reader_thread_.joinable()) reader_thread_.join();

    // Flush ring buffer — output silence during seek
    ring_write_.store(0);
    ring_read_.store(0);

    current_lba_.store(target_lba);
    reader_stop_.store(false);
    reader_thread_ = std::thread(&CDSource::readerWorker, this);
}

bool CDSource::readSectors(DWORD lba, int count, uint8_t* buf) {
    RAW_READ_INFO info {};
    // Windows quirk: DiskOffset uses 2048-byte sector scale even for audio
    info.DiskOffset.QuadPart = (ULONGLONG)lba * 2048;
    info.SectorCount         = (ULONG)count;
    info.TrackMode           = CDDA;

    DWORD bytes = 0;
    return DeviceIoControl(hCD_, IOCTL_CDROM_RAW_READ,
                           &info, sizeof(info),
                           buf, SECTOR_BYTES * count,
                           &bytes, nullptr) != 0;
}

// ─── Audio callback interface ─────────────────────────────────────────────────

uint32_t CDSource::readFrames(float* dst, uint32_t frame_count) {
    if (current_track_.load() == 0 || paused_.load()) {
        std::memset(dst, 0, frame_count * 2 * sizeof(float));
        return frame_count;
    }

    int samples_needed = (int)frame_count * 2;
    std::vector<int16_t> tmp(samples_needed, 0);
    ringRead(tmp.data(), samples_needed);

    for (int i = 0; i < samples_needed; ++i)
        dst[i] = (float)tmp[i] / 32768.0f;

    return frame_count;
}

// ─── Position ─────────────────────────────────────────────────────────────────

int CDSource::positionSec() const {
    if (!current_track_.load()) return 0;
    for (auto& t : tracks_)
        if (t.number == current_track_.load()) {
            DWORD played = current_lba_.load() - t.start_lba;
            return (int)(played / 75);
        }
    return 0;
}

int CDSource::durationSec() const {
    for (auto& t : tracks_)
        if (t.number == current_track_.load())
            return t.duration_sec;
    return 0;
}

// ─── Ring buffer ──────────────────────────────────────────────────────────────

int CDSource::ringAvailable() const {
    int w = ring_write_.load(std::memory_order_acquire);
    int r = ring_read_.load(std::memory_order_acquire);
    return (w >= r) ? (w - r) : (RING_SIZE - r + w);
}

void CDSource::ringWrite(const int16_t* data, int samples) {
    // Only called from reader thread — no concurrent writers
    int w = ring_write_.load(std::memory_order_relaxed);
    for (int i = 0; i < samples; ++i) {
        ring_[w] = data[i];
        w = (w + 1) % RING_SIZE;
    }
    ring_write_.store(w, std::memory_order_release);
}

int CDSource::ringRead(int16_t* dst, int samples) {
    // Called from audio callback — no mutex, lock-free
    int avail = ringAvailable();
    int n = std::min(samples, avail);
    int r = ring_read_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        dst[i] = ring_[r];
        r = (r + 1) % RING_SIZE;
    }
    ring_read_.store(r, std::memory_order_release);
    return n;
}

// ─── Drive model query and offset lookup ──────────────────────────────────────
#include <ntddstor.h>
#include <cctype>

std::string CDSource::queryDriveModel() {
    if (hCD_ == INVALID_HANDLE_VALUE) return {};
    std::vector<uint8_t> buf(512, 0);
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;
    DWORD bytes = 0;
    if (!DeviceIoControl(hCD_, IOCTL_STORAGE_QUERY_PROPERTY,
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

int CDSource::lookupDriveOffset(const std::string& model) {
    if (model.empty()) return 0;
    std::string m = model;
    std::transform(m.begin(), m.end(), m.begin(),
                   [](unsigned char c){ return (char)std::toupper(c); });

    static const struct { const char* key; int offset; } kTable[] = {
        { "ASUS BW-16D1HT",          +6   },
        { "ASUS BC-12D2HT",          +6   },
        { "ASUS DRW-24D5MT",         +6   },
        { "ASUS DRW-24F1ST",         +6   },
        { "ASUS DRW-24B1ST",         +6   },
        { "LG BH16NS55",             +6   },
        { "LG BH16NS40",             +6   },
        { "LG WH16NS60",             +6   },
        { "LG WH16NS40",             +6   },
        { "LG GH24NSB0",             +6   },
        { "LG GH24NSD1",             +6   },
        { "LG GH22NS",               +6   },
        { "LG GSA-H",                +6   },
        { "PIONEER BD-RW BDR-209",   +30  },
        { "PIONEER BD-RW BDR-212",   +30  },
        { "PIONEER DVD-RW DVR-",     +30  },
        { "PIONEER DVD-ROM DVD-",    +30  },
        { "PLDS DVD-RW DH-16ABS",    +667 },
        { "PLDS DVD-RW DU-8A5LH",   +48  },
        { "LITE-ON DVDRW",           +6   },
        { "LITE-ON DVD",             +6   },
        { "LITEON",                  +6   },
        { "TSSTcorp CDDVDW SH-",     +6   },
        { "TSSTcorp CDDVDW SN-",     +6   },
        { "SAMSUNG DVD",             +6   },
        { "PLEXTOR PX-B940SA",       +30  },
        { "PLEXTOR PX-",             +30  },
        { "SONY DVD RW DRU-",        +6   },
        { "SONY DVD RW DW-",         +6   },
        { "OPTIARC DVD RW AD-",      +6   },
        { "MATSHITA DVD-RAM UJ",     +667 },
        { "MATSHITA BD-MLT UJ",      +667 },
        { "HL-DT-ST DVDRW GHD3N",    +6   },  // your specific drive - AR DB offset
        { "HL-DT-ST DVDRAM GH24",    +6   },
        { "HL-DT-ST DVDRAM GH22",    +6   },
        { "HL-DT-ST DVD+-RW",        +6   },
        { "HL-DT-ST BD-RE",          +6   },
        { "HL-DT-ST",                +6   },  // catch-all for all LG/HLDS laptop drives
        { "VIRTUAL",                 +0   },
    };

    for (const auto& e : kTable) {
        std::string key = e.key;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c){ return (char)std::toupper(c); });
        if (m.find(key) != std::string::npos)
            return e.offset;
    }

    // Not in the hand-tuned overrides above — fall back to the full AccurateRip
    // drive-offset database (4885 drives) in drive_offsets.h. The :: qualifier
    // calls the free function there, NOT this member (same name, different scope).
    return ::lookupDriveOffset(model);
}

#endif // _WIN32
