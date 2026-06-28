#pragma once
#ifdef _WIN32

#include "CDSource.h"
#include "MBLookup.h"
#include "StringUtils.h"
#include "AudioManager.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <filesystem>
#include <cstdint>

// libebur128 — needed for the ebur128_state* parameter on ripTrack, which hands
// each kept track's integrated-loudness state back to the worker for true album-
// gain aggregation (ebur128_loudness_global_multiple). The type is an anonymous
// struct typedef, so it can't be forward-declared — include the header directly.
#include <ebur128.h>

// ─── Rip mode ─────────────────────────────────────────────────────────────────
enum class RipMode {
    AccurateRip,   // [A] Full AR network handshake, CRC verification
    CUETools,      // [C] CTDB global CRC32 verification (no Reed-Solomon repair yet)
    Local,         // [Y] Best-effort local extraction, no network verification
    LocalVerify,   // [B] Local + two-pass determinism check (no network)
    None           // [N] Abort
};

// ─── AccurateRip result per track ─────────────────────────────────────────────
enum class ARStatus { NotQueried, Matched_v2, Matched_v1, NotFound, NetworkError, ReadError };

struct ARTrackResult {
    ARStatus status     = ARStatus::NotQueried;
    int      confidence = 0;
    uint32_t crc_v1     = 0;  // csum_lo
    uint32_t crc_v2     = 0;  // csum_lo + csum_hi
    uint32_t ctdb_crc      = 0;  // running CRC32 state for CTDB (finalized across all tracks)
    size_t   ctdb_bytes    = 0;  // bytes fed to CTDB CRC for this track
    uint32_t frame450_local = 0; // sector-450 track-relative CRC (drive offset self-check)
};

// ─── ReplayGain result ────────────────────────────────────────────────────────
struct RGResult {
    double track_gain = 0.0;
    double track_peak = 0.0;
    double album_gain = 0.0;
    double album_peak = 0.0;
    bool   valid      = false;
};

// ─── Rip state ────────────────────────────────────────────────────────────────
enum class RipState { Idle, Ripping, Done, Error, Cancelled };

struct RipProgress {
    RipState    state    = RipState::Idle;
    int         track    = 0;
    int         total    = 0;
    int         pct      = 0;
    bool        using_c2 = false;
    std::string status_msg;
};

class CDRipper {
public:
    using ProgressCb = std::function<void(const RipProgress&)>;

    CDRipper() = default;
    ~CDRipper() { cancel(); }

    bool start(AudioManager&               audio,
               const std::vector<CDTrack>& tracks,
               const std::string&          out_dir,
               const MBRelease&            rel,
               RipMode                     mode,
               ProgressCb                  cb);

    void cancel();
    bool     isActive() const { return active_.load(); }
    RipState state()    const { return state_.load(); }

    static std::string buildOutputDir(const MBRelease& rel);

private:
    std::atomic<bool>     active_ { false };
    std::atomic<bool>     cancel_ { false };
    std::atomic<RipState> state_  { RipState::Idle };
    std::thread           thread_;

    void worker(std::string          drive_letter,
                std::vector<CDTrack> tracks,
                std::string          out_dir,
                MBRelease            rel,
                RipMode              mode,
                ProgressCb           cb,
                HANDLE               hCD,
                int                  drive_offset,
                std::string          drive_model,
                DWORD                full_leadout_lba = 0,
                std::vector<DWORD>   data_track_lbas = {});

    ARTrackResult ripTrack(HANDLE             hCD,
                           const CDTrack&     track,
                           int                track_idx,
                           int                total_tracks,
                           bool               is_first,
                           bool               is_last,
                           bool               use_c2,
                           const std::string& flac_path,
                           const std::string& mp3_path,
                           RGResult&          rg_out,
                           const ProgressCb&  cb,
                           const std::string& log_path,
                           RipMode            mode,
                           int                drive_offset  = 0,
                           uint32_t           ctdb_crc_in   = 0xFFFFFFFFu,
                           size_t             ctdb_bytes_in = 0,
                           int                pressing_offset = 0,
                           size_t             ctdb_total_bytes = 0,
                           ebur128_state**    out_ebur = nullptr);

    static void tagFile(const std::string&          path,
                        const MBRelease&             rel,
                        const MBTrack*               mt,
                        int                          track_num,
                        const std::vector<uint8_t>&  art,
                        const ARTrackResult&         ar,
                        const RGResult&              rg,
                        RipMode                      mode);

    // AccurateRip
    static uint32_t computeCDDB(const std::vector<CDTrack>& tracks,
                                DWORD full_leadout_lba = 0,
                                const std::vector<DWORD>& data_track_lbas = {});

    // Fetch AR binary, save .bin and manifest to ar_cache_dir.
    // Returns true even on 404 (disc not found); returns false on network error.
    static bool fetchARData(const std::vector<CDTrack>&                        tracks,
                            const std::string&                                  log_path,
                            const std::string&                                  ar_cache_dir,
                            std::vector<std::vector<std::pair<uint32_t,int>>>&  out_v1,
                            std::vector<std::vector<std::pair<uint32_t,int>>>&  out_v2,
                            DWORD                                               full_leadout_lba = 0,
                            const std::vector<DWORD>&                           data_track_lbas = {});

    // CTDB (CUETools Database) — global CRC32 verification
    // Returns CTDB ID string and whether disc is verified
    static std::string computeCTDBId(const std::vector<uint8_t>& disc_audio);
    static bool fetchCTDBData(const std::string& ctdb_id,
                              const std::string& log_path,
                              int ntracks,
                              std::string& out_status);

    // C2 probe
    static bool probeC2(HANDLE hCD);

    static std::string sanitizePath(const std::string& s);
    static HANDLE      openDrive(const std::string& drive_letter);
};

#endif // _WIN32
