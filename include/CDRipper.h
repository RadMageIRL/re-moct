#pragma once

#include "CDSource.h"
#include "MBLookup.h"
#include "StringUtils.h"
#include "AudioManager.h"
#include "RipFormats.h"   // RipFormat/RipOptions — the format-selection surface
#include "core/ICdIo.h"   // device transport seam (slice 8) — no windows.h here

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include "RipSelection.h"   // CD-S1: ripsel::Item, the disc/selection split
#include <filesystem>
#include <cstdint>
#include <memory>

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
// CD-S4: the enum and its labels moved to ArStatus.h so the labels are pure and
// testable, and so every label site is an exhaustive switch that -Wswitch will
// break if a status is ever added without being spelled. Included here so every
// existing user of ARStatus compiles unchanged.
#include "ArStatus.h"

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
    // CD-S2: is there an ALBUM figure at all? 0.0 dB is a legitimate gain, so
    // the numbers above cannot say "absent" by themselves - the same conflation
    // CD-S1 closed in the summary counter, one layer out. False on a partial
    // rip, where an album value measured over a subset would silently claim to
    // describe the album; the tag pair is then omitted entirely rather than
    // written as a sentinel, because ReplayGain has no absent value and every
    // scanner reads a MISSING album tag as "not scanned yet".
    bool   album_valid = false;
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

    // Ctor injection (slice-6/7 pattern): tests pass a fake; nullptr = production
    // default core::cdio(), resolved in openDrive() (link-time bridge, no setCdio()).
    explicit CDRipper(core::ICdIo* io = nullptr) : io_(io) {}
    ~CDRipper() { cancel(); }

    // One rip output target: a format and its resolved path. The worker builds
    // the ordered list per track from RipOptions.formats (table order = the
    // pre-selection FLAC-then-MP3 operation order for the default set); a
    // deselected format is simply ABSENT — never instantiated, no path built.
    struct RipOutput {
        RipFormat   fmt;
        std::string path;
    };

    // `tracks` is ALWAYS THE FULL TOC. It is what the AccurateRip disc ID, the
    // response chunk filter, the result indexing and the multi-disc pick are
    // computed from, and every one of them is wrong given anything shorter -
    // so a subset is expressed by `selected_toc`, never by passing fewer tracks.
    //
    // `selected_toc` holds TOC INDICES (0-based) to extract. EMPTY MEANS ALL,
    // which is the shipped behaviour byte-for-byte: ripsel::planAll reproduces
    // exactly the arguments this worker passed before a selection existed.
    bool start(AudioManager&               audio,
               const std::vector<CDTrack>& tracks,
               const std::string&          out_dir,
               const MBRelease&            rel,
               RipMode                     mode,
               RipOptions                  opt,
               ProgressCb                  cb,
               const std::vector<int>&     selected_toc = {});

    void cancel();
    bool     isActive() const { return active_.load(); }
    RipState state()    const { return state_.load(); }

    static std::string buildOutputDir(const MBRelease& rel);
    // The user's music root (extracted verbatim from buildOutputDir,
    // stream-record R1) and the stream-capture sibling of the rip output dir:
    // <music>/re-moct/recordings. One root — relocating Music moves both.
    static std::string musicRoot();
    static std::string recordingsDir();

private:
    core::ICdIo*          io_ = nullptr;  // injected; nullptr = core::cdio()
    std::atomic<bool>     active_ { false };
    std::atomic<bool>     cancel_ { false };
    std::atomic<RipState> state_  { RipState::Idle };
    std::thread           thread_;

    void worker(std::string          drive_letter,
                std::vector<CDTrack> tracks,
                std::vector<ripsel::Item> plan,
                std::string          out_dir,
                MBRelease            rel,
                RipMode              mode,
                RipOptions           opt,
                ProgressCb           cb,
                std::unique_ptr<core::ICdDevice> dev,
                int                  drive_offset,
                std::string          drive_model,
                uint32_t             full_leadout_lba = 0,
                std::vector<uint32_t> data_track_lbas = {});

    ARTrackResult ripTrack(core::ICdDevice&   dev,
                           const CDTrack&     track,
                           int                track_idx,
                           int                total_tracks,
                           bool               is_first,
                           bool               is_last,
                           bool               use_c2,
                           const std::vector<RipOutput>& outs,
                           const RipOptions&  opt,
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

    // ctdb_status / ctdb_disc_id are DISC-scope (CUETools mode only): one verdict
    // and one disc ID for the whole rip, written identically into every track's
    // tags the way album ReplayGain already is. Empty in every other mode, and on
    // a cancelled or failed rip, in which case no CTDB tag is written at all.
    static void tagFile(const std::string&          path,
                        const MBRelease&             rel,
                        const MBTrack*               mt,
                        int                          track_num,
                        const std::vector<uint8_t>&  art,
                        const ARTrackResult&         ar,
                        const RGResult&              rg,
                        RipMode                      mode,
                        const std::string&           ctdb_status,
                        const std::string&           ctdb_disc_id);

    // AccurateRip
    static uint32_t computeCDDB(const std::vector<CDTrack>& tracks,
                                uint32_t full_leadout_lba = 0,
                                const std::vector<uint32_t>& data_track_lbas = {});

    // Fetch AR binary, save .bin and manifest to ar_cache_dir.
    // Returns true even on 404 (disc not found); returns false on network error.
    static bool fetchARData(const std::vector<CDTrack>&                        tracks,
                            const std::string&                                  log_path,
                            const std::string&                                  ar_cache_dir,
                            std::vector<std::vector<std::pair<uint32_t,int>>>&  out_v1,
                            std::vector<std::vector<std::pair<uint32_t,int>>>&  out_v2,
                            uint32_t                                            full_leadout_lba = 0,
                            const std::vector<uint32_t>&                        data_track_lbas = {});

    // CTDB (CUETools Database) — global CRC32 verification
    // Returns CTDB ID string and whether disc is verified
    static std::string computeCTDBId(const std::vector<uint8_t>& disc_audio);
    static bool fetchCTDBData(const std::string& ctdb_id,
                              const std::string& log_path,
                              int ntracks,
                              std::string& out_status);

    // C2 probe
    static bool probeC2(core::ICdDevice& dev);

    static std::string sanitizePath(const std::string& s);
    // Non-static: resolves io_ (injected or core::cdio()) to open the device.
    std::unique_ptr<core::ICdDevice> openDrive(const std::string& drive_letter);
};

