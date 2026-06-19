#pragma once
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>   // CTL_CODE, DEVICE_TYPE etc — must come before ntddcdrm
#include <ntddcdrm.h>   // IOCTL_CDROM_RAW_READ, RAW_READ_INFO, CDDA
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

// ─── CD Track info ────────────────────────────────────────────────────────────
struct CDTrack {
    int    number;       // 1-based track number
    DWORD  start_lba;    // logical block address of track start
    DWORD  length_lba;   // length in sectors
    int    duration_sec; // approximate duration
};

// ─── CDSource ─────────────────────────────────────────────────────────────────
// Reads Red Book audio sectors from an optical drive via Win32 IOCTL,
// converts to float PCM, and feeds a ring buffer consumed by the audio callback.
// Completely isolated from the existing ma_decoder path.

class CDSource {
public:
    static constexpr int    SECTOR_BYTES   = 2352;   // raw Red Book sector
    static constexpr int    SECTOR_SAMPLES = 588;    // 2352 / 4 (16-bit stereo)
    static constexpr int    SECTORS_PER_READ = 20;   // read chunk size
    static constexpr int    RING_SECTORS   = 200;    // ~3.4s buffer

    CDSource() = default;
    ~CDSource() { close(); }

    // Open drive (e.g. "D"). Returns false if no audio CD found.
    bool open(const std::string& drive_letter);
    void close();

    bool isOpen()  const { return hCD_ != INVALID_HANDLE_VALUE; }
    bool isPlaying() const { return playing_.load(); }

    // TOC
    const std::vector<CDTrack>& tracks() const { return tracks_; }
    std::string driveLetter()    const { return drive_letter_; }
    DWORD       fullLeadoutLba()   const { return full_leadout_lba_; }
    const std::vector<DWORD>& dataTrackLbas() const { return data_track_lbas_; }

    // Returns sector offsets suitable for DiscID computation:
    // [track1_start, track2_start, ..., trackN_start, lead_out]
    std::vector<DWORD> tocOffsets() const {
        std::vector<DWORD> offs;
        offs.reserve(tracks_.size() + 1);
        for (const auto& t : tracks_) offs.push_back(t.start_lba);
        // Lead-out = last track start + length
        if (!tracks_.empty())
            offs.push_back(tracks_.back().start_lba + tracks_.back().length_lba);
        return offs;
    }

    // Start playing a track (1-based). Stops current playback first.
    bool playTrack(int track_number);
    void stop();
    void stopReader() {
        // Stop reader thread only — leaves hCD_ open for ripping
        reader_stop_.store(true);
        playing_.store(false);
        if (reader_thread_.joinable()) reader_thread_.join();
        current_track_.store(0);
        ring_write_.store(0);
        ring_read_.store(0);
    }
    void pause(bool p) { paused_.store(p); }
    bool paused() const { return paused_.load(); }
    void seekTo(int seconds);
    bool mediaRemoved() const { return media_removed_.load(); }
    void clearMediaRemoved()  { media_removed_.store(false); }
    bool checkMedia() {
        // Non-blocking check — returns false if disc is gone/tray open
        if (hCD_ == INVALID_HANDLE_VALUE) return false;
        DWORD dummy = 0;
        bool ok = DeviceIoControl(hCD_, IOCTL_CDROM_CHECK_VERIFY,
                                  nullptr, 0, nullptr, 0, &dummy, nullptr) != 0;
        if (!ok) media_removed_.store(true);
        return ok;
    }

    // Called by CDRipper — reads raw sectors using the existing open drive handle
    bool readSectorsForRip(DWORD lba, int count, uint8_t* buf) {
        return readSectors(lba, count, buf);
    }

    // Drive read offset in samples (from AccurateRip offset database).
    // Populated on open(). 0 = unknown/uncorrected.
    int         driveOffset() const { return drive_offset_samples_; }
    std::string driveModel()  const { return drive_model_; }
    // True only if the drive model was actually found in the offset table
    // (including explicit zero-offset entries like "VIRTUAL"). False means
    // the model wasn't recognized and driveOffset() is an unverified
    // fallback of 0, NOT a confirmed zero-offset drive.
    bool        driveOffsetKnown() const { return offset_known_; }

    // Called from audio callback — fills dst with float PCM frames.
    uint32_t readFrames(float* dst, uint32_t frame_count);

    int  currentTrack()   const { return current_track_.load(); }
    int  positionSec()    const;
    int  durationSec()    const;

private:
    HANDLE                  hCD_            = INVALID_HANDLE_VALUE;
    std::string             drive_letter_;
    std::string             drive_model_;
    int                     drive_offset_samples_ = 0;
    DWORD                   full_leadout_lba_     = 0;  // includes all sessions
    std::vector<DWORD>      data_track_lbas_;              // data tracks for CDDB
    bool                    offset_known_         = false;
    std::vector<CDTrack>    tracks_;

    // Playback state
    std::atomic<bool>       playing_        { false };
    std::atomic<bool>       paused_         { false };
    std::atomic<bool>       reader_stop_    { false };
    std::atomic<bool>       media_removed_  { false };
    std::atomic<int>        current_track_  { 0 };
    std::atomic<DWORD>      track_end_lba_  { 0 };
    std::atomic<DWORD>      current_lba_    { 0 };

    // Ring buffer (raw 16-bit stereo samples, interleaved)
    static constexpr int RING_SIZE = SECTOR_SAMPLES * RING_SECTORS * 2;
    std::vector<int16_t>    ring_;
    std::atomic<int>        ring_write_     { 0 };
    std::atomic<int>        ring_read_      { 0 };

    // Background reader thread
    std::thread             reader_thread_;

    void readerWorker();
    bool readSectors(DWORD lba, int count, uint8_t* buf);

    std::string queryDriveModel();
    static int  lookupDriveOffset(const std::string& model);

    int  ringAvailable() const;
    void ringWrite(const int16_t* data, int samples);
    int  ringRead(int16_t* dst, int samples);
};

#endif // _WIN32