#ifdef _WIN32
#include "CDSource.h"
#include "drive_offsets.h"
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
    int first = toc.FirstTrack;
    int last  = toc.LastTrack;

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

    for (int t = first; t <= last; ++t) {
        auto& te = toc.TrackData[t - 1];
        // Skip data tracks (control bit 4 set = data)
        if ((te.Control & 0x04) != 0) continue;

        DWORD start = msf_to_lba(te.Address);
        DWORD end   = msf_to_lba(toc.TrackData[t].Address); // [last] = lead-out

        DWORD len = (end > start) ? (end - start) : 0;

        CDTrack ct;
        ct.number       = t;
        ct.start_lba    = start;
        ct.length_lba   = len;
        ct.duration_sec = (int)(len / 75);  // 75 sectors/second
        tracks_.push_back(ct);
    }

    if (tracks_.empty()) {
        CloseHandle(hCD_);
        hCD_ = INVALID_HANDLE_VALUE;
        return false;
    }

    ring_.resize(RING_SIZE, 0);
    return true;
}

void CDSource::close() {
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
    if (target_lba >= track_end_lba_.load()) target_lba = track_end_lba_.load() - 1;

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

    // The AccurateRip database uses marketing vendor names, but Windows
    // STORAGE_DEVICE_DESCRIPTOR returns OEM strings. Normalise known aliases
    // before lookup. (AR database note: "HL-DT-ST as LG Electronics,
    // Matshita as Panasonic")
    std::string normalised = model;
    struct { const char* from; const char* to; } aliases[] = {
        { "HL-DT-ST", "LG Electronics" },
        { "MATSHITA", "Panasonic"       },
        { "JLMS",     "Lite-On"         },
    };
    for (auto& a : aliases) {
        // Case-insensitive prefix check
        std::string m = normalised, f = a.from;
        for (auto& c : m) c = (char)std::toupper((unsigned char)c);
        for (auto& c : f) c = (char)std::toupper((unsigned char)c);
        if (m.find(f) == 0) {
            // Replace prefix and reformat to match DB style "Vendor - Model"
            std::string rest = normalised.substr(strlen(a.from));
            // Trim leading spaces
            auto p = rest.find_first_not_of(' ');
            if (p != std::string::npos) rest = rest.substr(p);
            normalised = std::string(a.to) + " - " + rest;
            break;
        }
    }

    // 4878-entry database sourced from accuraterip.com/driveoffsets.htm
    return ::lookupDriveOffset(normalised);
}

#endif // _WIN32
