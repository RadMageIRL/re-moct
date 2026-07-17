#include "CDRipper.h"
#include "ar_crc.h"
#include "Version.h"        // REMOCT_VERSION (single source) for the CTDB UA + rip tags
#include "core/IHttp.h"     // core::IHttp seam (AR/CTDB fetch); WinINet lives behind it
#include "core/ICdIo.h"     // core::ICdIo seam (slice 8); the CD IOCTLs live behind it

// Portable since Phase 3 slice 1: the non-device Win32 this TU used became
// port:: helpers whose Windows expansion is the baseline call verbatim
// (port::fopenUtf8 == _wfopen(utf8_to_wide(p)); port::sleepMs == ::Sleep).
// windows.h/shlobj.h remain ONLY for buildOutputDir's Windows branch
// (SHGetKnownFolderPath → the user Music folder). Device access goes through
// core::ICdIo (Windows IOCTL impl; Linux SG_IO sibling at slice 6) —
// winioctl.h/ntddcdrm.h have been gone since slice 8.
#include "PortUtil.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

// Rip-output encoders (rip-encoder-seam): the FLAC/LAME code that lived
// inline in ripTrack moved verbatim behind IEncoder — this TU no longer
// touches either C API directly.
#include "IEncoder.h"
#include "FlacEncoder.h"
#include "Mp3Encoder.h"
#include "WavEncoder.h"
#include "OpusRipEncoder.h"
#include "R128Gain.h"       // r128FromDb — shared with the decode side (LocalFileSource)

// libebur128 for ReplayGain
#include <ebur128.h>

// TagLib
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tstring.h>
#include <taglib/tbytevector.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/mpegfile.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/xiphcomment.h>
// TagLib's Ogg Opus reader/writer. The taglib/ prefix matters doubly here:
// the CODEC header opus/opusfile.h shares the basename (the decode-slice
// collision), and this TU must get TagLib's C++ class, not libopusfile's C API.
#include <taglib/opusfile.h>

#include <filesystem>
#include <cstring>
#include <ctime>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>

#include "json.hpp"   // nlohmann single-header (already vendored for MBLookup)
#include "CoverArt.h"

namespace fs = std::filesystem;

static constexpr int SECTOR_BYTES     = 2352;
static constexpr int SECTOR_SAMPLES   = 588;
static constexpr int SECTORS_PER_READ = 27;
static constexpr int SAMPLE_RATE      = 44100;
static constexpr int CHANNELS         = 2;
// (BIT_DEPTH / FLAC_COMPRESSION moved with the FLAC code into FlacEncoder.cpp)
// C2 read: 2352 audio bytes + 294 C2 bytes = 2646 bytes per sector
static constexpr int SECTOR_BYTES_C2  = 2352 + 294;

// Separator for user-visible output paths (rip folders, cache files, and the
// paths echoed into rip logs). Per-platform on purpose: Windows rip logs print
// backslash paths and the slice-6 CD gate diffs logs line-by-line against
// Windows baselines — "/" would work for the OS but change the logs.
#ifdef _WIN32
static const std::string kSep = "\\";
#else
static const std::string kSep = "/";
#endif

// Local-family modes do no network/AR verification. [B] (LocalVerify) additionally
// runs a two-pass determinism check; [Y] (Local) is single-pass best-effort/fast.
static inline bool isLocal(RipMode m) {
    return m == RipMode::Local || m == RipMode::LocalVerify;
}
// AccurateRip frame 450: sector 450 of track 1, used for pressing offset detection
// (AR frame-450 constants moved to ar_crc.h: ar::FRAME450_START / ar::FRAME450_END.)
// Maximum pressing offset to scan (samples). 700 covers all known pressings.
static constexpr int MAX_PRESSING_OFFSET = 700;

// ─── Path helpers ─────────────────────────────────────────────────────────────
std::string CDRipper::sanitizePath(const std::string& s) {
    static const std::string ill = R"(\/:*?"<>|)";
    std::string o; o.reserve(s.size());
    for (unsigned char c : s)
        o += (c < 32 || ill.find((char)c) != std::string::npos) ? '_' : (char)c;
    while (!o.empty() && (o.back()=='.'||o.back()==' ')) o.pop_back();
    return o.empty() ? "Unknown" : o;
}

std::string CDRipper::buildOutputDir(const MBRelease& rel) {
    std::string music;
#ifdef _WIN32
    // The baseline block, verbatim: the user's known Music folder, USERPROFILE
    // fallback.
    PWSTR wp = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &wp))) {
        int n = WideCharToMultiByte(CP_UTF8,0,wp,-1,nullptr,0,nullptr,nullptr);
        std::string u(n,'\0');
        WideCharToMultiByte(CP_UTF8,0,wp,-1,u.data(),n,nullptr,nullptr);
        if (!u.empty()&&u.back()=='\0') u.pop_back();
        music = u; CoTaskMemFree(wp);
    } else {
        char buf[MAX_PATH];
        music = GetEnvironmentVariableA("USERPROFILE",buf,MAX_PATH)
              ? std::string(buf)+"\\Music" : "C:\\Music";
    }
#else
    // XDG twin (readiness doc §7.3): the user-dirs MUSIC entry when set,
    // ~/Music otherwise. Parsing user-dirs.dirs keeps this dependency-free
    // (xdg-user-dir is a shell-out; the file is the same source of truth).
    const char* home = std::getenv("HOME");
    std::string h = (home && *home) ? home : ".";
    music = h + "/Music";
    std::string cfg;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) cfg = x;
    else cfg = h + "/.config";
    if (FILE* f = std::fopen((cfg + "/user-dirs.dirs").c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s(line);
            auto p = s.find("XDG_MUSIC_DIR=\"");
            if (p == std::string::npos) continue;
            s = s.substr(p + 15);
            if (auto q = s.find('"'); q != std::string::npos) s = s.substr(0, q);
            if (auto hp = s.find("$HOME"); hp != std::string::npos)
                s = s.substr(0, hp) + h + s.substr(hp + 5);
            if (!s.empty()) music = s;
            break;
        }
        std::fclose(f);
    }
#endif
    std::string folder;
    if (!rel.title.empty()) {
        std::string yr = rel.date.size()>=4 ? " ("+rel.date.substr(0,4)+")" : "";
        folder = sanitizePath(rel.artist.empty() ? rel.title
                              : rel.artist+" - "+rel.title) + yr;
    } else {
        std::time_t t = std::time(nullptr); std::tm tmbuf{}; localtimeSafe(t, tmbuf); std::tm* tm = &tmbuf;
        char buf[32]; std::strftime(buf,sizeof(buf),"CD_Rip_%Y-%m-%d",tm);
        folder = buf;
    }
    return music + kSep + "re-moct" + kSep + folder;
}

std::unique_ptr<core::ICdDevice> CDRipper::openDrive(const std::string& dl) {
    // Spec→\\.\X: mapping and open flags live in the seam impl (CdIoWin.cpp) —
    // this baseline's exact share flags are the ones preserved there.
    core::ICdIo& io = io_ ? *io_ : core::cdio();
    return io.open(dl);
}

// ─── C2 Error Pointer probe ───────────────────────────────────────────────────
// Attempt a single-sector read in C2+data mode.
// Returns true if drive supports C2 (non-zero C2 bytes returned cleanly).
bool CDRipper::probeC2(core::ICdDevice& dev) {
    // The raw-read TrackMode doesn't have a C2 mode — C2 comes back iff the drive
    // supports it and the buffer is sized for it. Simplest probe: read sector 0
    // (always readable) with a C2-sized buffer and check if exactly
    // SECTOR_BYTES_C2 bytes come back; a non-C2 drive returns SECTOR_BYTES.
    uint8_t buf[SECTOR_BYTES_C2] = {};
    std::size_t got = 0;
    bool ok = dev.readRaw(0, 1, /*want_c2=*/true, buf, SECTOR_BYTES_C2, got);
    // C2 supported if the read succeeded and returned the larger buffer
    return ok && got == SECTOR_BYTES_C2;
}

// ─── Sector read with retry + optional C2 awareness ──────────────────────────
static bool readSectors(core::ICdDevice& dev, uint32_t lba, int count,
                        uint8_t* pcm_buf,  // SECTOR_BYTES*count output
                        bool use_c2,
                        int* c2_error_count = nullptr) {
    if (c2_error_count) *c2_error_count = 0;

    // Per-read temp buffer (may be larger if C2)
    const int buf_size = use_c2 ? SECTOR_BYTES_C2 * count
                                : SECTOR_BYTES     * count;
    std::vector<uint8_t> tmp(buf_size);

    std::size_t bytes = 0;
    bool ok = dev.readRaw((uint32_t)lba, (uint32_t)count, use_c2,
                          tmp.data(), tmp.size(), bytes);
    if (!ok) return false;

    if (use_c2 && bytes == (std::size_t)SECTOR_BYTES_C2 * count) {
        // C2 mode returned — de-interleave: each sector is [2352 audio][294 c2]
        for (int s = 0; s < count; ++s) {
            const uint8_t* src = tmp.data() + s * SECTOR_BYTES_C2;
            std::memcpy(pcm_buf + s * SECTOR_BYTES, src, SECTOR_BYTES);
            if (c2_error_count) {
                // Any non-zero C2 byte = error in that sector
                for (int b = 0; b < 294; ++b)
                    if (src[SECTOR_BYTES + b]) { ++(*c2_error_count); break; }
            }
        }
    } else {
        // Standard CDDA mode
        std::memcpy(pcm_buf, tmp.data(), std::min((int)bytes, SECTOR_BYTES*count));
    }
    return true;
}

// Retry wrapper — falls back to single-sector on block failure
static bool readSectorsWithRetry(core::ICdDevice& dev, uint32_t lba, int count,
                                 uint8_t* pcm_buf, bool use_c2,
                                 int* c2_errors = nullptr) {
    for (int attempt = 0; attempt <= 4; ++attempt) {
        if (readSectors(dev, lba, count, pcm_buf, use_c2, c2_errors))
            return true;
        port::sleepMs(100 * (attempt+1));
        if (count > 1 && attempt == 1) {
            // Fall back to single-sector reads
            if (c2_errors) *c2_errors = 0;
            for (int s = 0; s < count; ++s) {
                int c2e = 0;
                if (!readSectors(dev, lba+s, 1,
                                 pcm_buf + s*SECTOR_BYTES, use_c2, &c2e))
                    std::memset(pcm_buf + s*SECTOR_BYTES, 0, SECTOR_BYTES);
                if (c2_errors) *c2_errors += c2e;
            }
            return true; // silence > abort
        }
    }
    return false;
}

// ─── Drive speed control ──────────────────────────────────────────────────────
// Sets the drive's read speed via IOCTL_CDROM_SET_SPEED.
// speed is in KB/sec: 0xFFFF = max, 176 = 1x, 1764 = 10x, 3528 = 20x.
// Used to force a hardware-level cache eviction before Pass 2 by making the
// drive physically shift its laser/spindle parameters.
static void setDriveSpeed(core::ICdDevice& dev, uint16_t speed_kbps) {
    // Canonical CDROM_SET_SPEED shape lives in the seam impl — byte-identical to
    // this site's baseline (RequestType=CdromSetSpeed, WriteSpeed zero-init).
    dev.setSpeed(speed_kbps);
}

// ─── Drive cache defeat ───────────────────────────────────────────────────────
// Read more than the drive's read-ahead cache (typ. 512KB-4MB) from a region
// that does NOT overlap the target's read-ahead zone, forcing the target's
// sectors out of RAM so a re-read actually hits the platter. Without this, a
// damaged disc can read identically twice and masquerade as deterministic.
static void flushDriveCache(core::ICdDevice& dev, uint32_t target_lba, uint32_t leadout_lba,
                            bool use_c2, int cache_kb = 4096) {
    const int need = (cache_kb * 1024) / SECTOR_BYTES + 16; // sectors > cache
    const uint32_t end = leadout_lba ? leadout_lba
                                  : target_lba + (uint32_t)need * 8;
    // If the target is in the low half, flush from the tail; else flush the
    // lead-in side. Guarantees the flush window is far from the target.
    uint32_t lo;
    if (target_lba < end / 2)
        lo = (end > (uint32_t)need + 32) ? end - (uint32_t)need - 16 : 16;
    else
        lo = 16;

    std::vector<uint8_t> buf((size_t)SECTOR_BYTES * SECTORS_PER_READ);
    int c2 = 0;
    for (int done = 0; done < need; done += SECTORS_PER_READ) {
        int n = std::min(SECTORS_PER_READ, need - done);
        if (lo + (uint32_t)done + (uint32_t)n >= end) break;
        readSectorsWithRetry(dev, lo + (uint32_t)done, n, buf.data(), use_c2, &c2);
    }
}

// CDDB disc ID — start_lba is already absolute (includes 150-sector pre-gap)
uint32_t CDRipper::computeCDDB(const std::vector<CDTrack>& tracks,
                               uint32_t full_leadout_lba,
                               const std::vector<uint32_t>& data_track_lbas) {
    auto digit_sum = [](uint32_t n) {
        uint32_t s = 0;
        while (n > 0) { s += n % 10; n /= 10; }
        return s;
    };
    uint32_t checksum = 0;
    for (const auto& t : tracks)
        checksum += digit_sum(t.start_lba / 75);
    for (uint32_t lba : data_track_lbas)
        checksum += digit_sum(lba / 75);
    uint32_t leadout   = full_leadout_lba ? full_leadout_lba
                       : tracks.back().start_lba + tracks.back().length_lba;
    uint32_t total_sec = (leadout / 75) - (tracks.front().start_lba / 75);
    uint32_t ntracks   = (uint32_t)(tracks.size() + data_track_lbas.size());
    return ((checksum % 0xFF) << 24) | (total_sec << 8) | ntracks;
}

// ─── frame450 pressing-offset auto-detection (PRIMARY method) ─────────────────
// Per Spoon's AccurateRip spec, bytes 5-8 of each track record are the
// OffsetFindCRC: an AR CRC over ONE frame (588 samples) at sector 450 of track 1,
// provided specifically for offset discovery. We read a window around that frame
// ONCE, then sweep the total read offset and compare the frame450 CRC (both the
// local-mul and global-mul forms — whichever the submitter used) against the DB
// entries. The shift that matches gives the total offset; the returned pressing
// offset is (total - drive_offset), matching ripTrack's total_skip convention.
// One short read replaces a list of full-track probe rips. Returns true and sets
// out_pressing_offset on a match; false (no change) if the DB record has no
// OffsetFindCRC, the read fails, or nothing matches — caller then falls back to
// the candidate-offset probe and ultimately the drive_offsets.h baseline.
static bool detectPressingOffsetFrame450(
        core::ICdDevice& dev, const CDTrack& track1, int drive_offset, bool use_c2,
        const std::vector<std::pair<uint32_t,int>>& db_frame450,  // = ar_db_v2[0]
        const std::string& log_path, int& out_pressing_offset)
{
    out_pressing_offset = 0;
    if (db_frame450.empty()) return false;       // this pressing has no OffsetFindCRC

    static constexpr int FR_FIRST = 450 * SECTOR_SAMPLES;   // 264600, track-relative
    static constexpr int FR_LEN   = SECTOR_SAMPLES;         // 588 (one frame)
    static constexpr int SWEEP    = 3000;                   // samples each side (>5 frames)

    // total read offset O = drive_offset + pressing; sweep O around the baseline.
    const int O_lo = drive_offset - SWEEP;
    const int O_hi = drive_offset + SWEEP;

    // Track-relative sample window we must have buffered, with 2-sector margins.
    const int first_rel = FR_FIRST + O_lo;
    const int last_rel  = FR_FIRST + O_hi + FR_LEN;
    if (first_rel < 0) return false;
    int read_start_sec = first_rel / SECTOR_SAMPLES - 2;
    if (read_start_sec < 0) read_start_sec = 0;
    const int read_end_sec = last_rel / SECTOR_SAMPLES + 2;
    const int read_count   = read_end_sec - read_start_sec + 1;
    if (read_start_sec + read_count > (int)track1.length_lba) return false;

    std::vector<uint8_t> raw((size_t)read_count * SECTOR_BYTES);
    int c2e = 0;
    if (!readSectorsWithRetry(dev, track1.start_lba + (uint32_t)read_start_sec,
                              read_count, raw.data(), use_c2, &c2e))
        return false;

    const int16_t* pcm = reinterpret_cast<const int16_t*>(raw.data());
    const int buf_frames = read_count * SECTOR_SAMPLES;
    const int base_idx   = FR_FIRST - read_start_sec * SECTOR_SAMPLES;

    // (frame-450 packing now lives in ar::frame450Crcs.)

    for (int O = O_lo; O <= O_hi; ++O) {
        const int start = base_idx + O;
        if (start < 0 || start + FR_LEN > buf_frames) continue;
        const auto [crc_local, crc_global] = ar::frame450Crcs(pcm, start, FR_LEN);
        for (const auto& pr : db_frame450) {
            if (pr.first == crc_local || pr.first == crc_global) {
                out_pressing_offset = O - drive_offset;
                FILE* lf = port::fopenUtf8(log_path, "a");
                if (lf) {
                    fprintf(lf, "  frame450 auto-detect: pressing %+d (total_skip=%d) "
                                "matched OffsetFindCRC %08x conf=%d\n",
                            out_pressing_offset, O, pr.first, pr.second);
                    fclose(lf);
                }
                return true;
            }
        }
    }

    FILE* lf = port::fopenUtf8(log_path, "a");
    if (lf) {
        fprintf(lf, "  frame450 auto-detect: no match in sweep — "
                    "falling back to candidate-offset probe\n");
        fclose(lf);
    }
    return false;
}

bool CDRipper::fetchARData(
        const std::vector<CDTrack>&                        tracks,
        const std::string&                                  log_path,
        const std::string&                                  ar_cache_dir,
        std::vector<std::vector<std::pair<uint32_t,int>>>& out_v1,
        std::vector<std::vector<std::pair<uint32_t,int>>>& out_v2,
        uint32_t                                           full_leadout_lba,
        const std::vector<uint32_t>&                       data_track_lbas) {

    if (tracks.empty()) return false;
    int ntracks = (int)tracks.size();

    // AccurateRip disc ID formula (definitively confirmed against live AR database):
    //
    //   LBAs stored in CDTrack::start_lba are absolute (include the 150-sector
    //   lead-in pregap that the drive adds).  The AR formula expects RELATIVE
    //   LBAs — i.e. the pregap stripped — so we subtract 150 from every value.
    //
    //   disc_id1 = simple sum of relative track LBAs + relative leadout
    //   disc_id2 = Σ max(rel_lba[i], 1) × (i+1)  +  rel_leadout × (ntracks+1)
    //
    // Both truncated to 32 bits.
    // AccurateRip offsets are LSN: subtract a FIXED 150-frame lead-in from every
    // track start AND the leadout. Do NOT subtract track 1's own start -- that
    // zeroes the pregap and 404s on non-standard-pregap discs (e.g. Relish, T1
    // at LBA 182 -> rel 32, which must remain in the ID). Verified against the
    // live AR DB: Relish + enhanced CDs both hit with fixed-150.
    static constexpr uint32_t AR_PREGAP = 150;
    uint32_t disc_leadout = full_leadout_lba ? full_leadout_lba
                       : tracks.back().start_lba + tracks.back().length_lba;
    uint32_t rel_leadout = disc_leadout - AR_PREGAP;
    uint32_t disc_id1 = 0, disc_id2 = 0;
    for (int i = 0; i < ntracks; ++i) {
        uint32_t rel_lba = tracks[i].start_lba - AR_PREGAP;
        disc_id1 += rel_lba;
        disc_id2 += std::max(rel_lba, 1u) * (uint32_t)(i + 1);
    }
    disc_id1 += rel_leadout;
    disc_id2 += rel_leadout * (uint32_t)(ntracks + 1);

    struct ARVariant { uint32_t id1, id2; };
    std::vector<ARVariant> variants = {{ disc_id1, disc_id2 }};

    uint32_t cddb_id = computeCDDB(tracks, full_leadout_lba, data_track_lbas);

    // Open log — append to the per-rip log file
    FILE* dbg = port::fopenUtf8(log_path, "a");
    if (dbg) {
        fprintf(dbg, "\n=== AccurateRip ===\n");
        fprintf(dbg, "ntracks=%d cddb=%08x disc_id1=%08x disc_id2=%08x\n",
                ntracks, cddb_id, disc_id1, disc_id2);
        for (int i=0;i<ntracks;++i)
            fprintf(dbg,"  track[%02d] lba=%lu  rel=%lu\n",
                i+1, (unsigned long)tracks[i].start_lba,
                (unsigned long)(tracks[i].start_lba - AR_PREGAP));
        fprintf(dbg,"  t1_lba=%lu  leadout_rel=%lu\n",
                (unsigned long)tracks[0].start_lba, (unsigned long)rel_leadout);
    }

    std::vector<uint8_t> ar_data;

    for (auto& v : variants) {
        char id1_str[9];
        snprintf(id1_str, sizeof(id1_str), "%08x", v.id1);
        char url[512];
        snprintf(url, sizeof(url),
            "http://www.accuraterip.com/accuraterip/%c/%c/%c/dbar-%03d-%08x-%08x-%08x.bin",
            id1_str[7], id1_str[6], id1_str[5],
            ntracks, v.id1, v.id2, cddb_id);

        if (dbg) fprintf(dbg, "TRY %08x/%08x -> HTTP ", v.id1, v.id2);
        core::HttpRequest req;
        req.url = url;   // http:// -> no SECURE (urlIsSecureScheme); default (full) UA; no cap
        core::HttpResponse r = core::http().fetch(req);
        uint32_t status = (uint32_t)r.status;
        if (dbg) fprintf(dbg, "%lu\n", status);

        if (status == 200) {
            ar_data.insert(ar_data.end(), r.body.begin(), r.body.end());
            disc_id1 = v.id1; disc_id2 = v.id2;
            if (dbg) fprintf(dbg, "HIT! disc_id1=%08x disc_id2=%08x size=%zu\n",
                             disc_id1, disc_id2, ar_data.size());

            // ── Save raw .bin and human-readable manifest to ar_cache_dir ──
            if (!ar_cache_dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(ar_cache_dir, ec);

                // Raw .bin file named by disc IDs
                char bin_name[64];
                snprintf(bin_name, sizeof(bin_name), "%08x-%08x-%08x.bin",
                         disc_id1, disc_id2, cddb_id);
                std::string bin_path = ar_cache_dir + kSep + bin_name;
                FILE* bf = port::fopenUtf8(bin_path, "wb");
                if (bf) { fwrite(ar_data.data(), 1, ar_data.size(), bf); fclose(bf); }

                // Human-readable manifest
                std::string mf_path = ar_cache_dir + kSep + "accuraterip-id.txt";
                FILE* mf = port::fopenUtf8(mf_path, "w");
                if (mf) {
                    std::time_t t = std::time(nullptr); std::tm tmbuf{}; localtimeSafe(t, tmbuf); std::tm* tm = &tmbuf;
                    char ts[32]; std::strftime(ts, sizeof(ts), "%Y-%m-%d", tm);
                    fprintf(mf, "[AccurateRip Metadata Manifest]\n");
                    fprintf(mf, "DiscID1      : %08x\n", disc_id1);
                    fprintf(mf, "DiscID2      : %08x\n", disc_id2);
                    fprintf(mf, "CDDB/DiscID3 : %08x\n", cddb_id);
                    fprintf(mf, "Tracks       : %d\n", ntracks);
                    fprintf(mf, "Rip Date     : %s\n", ts);
                    fprintf(mf, "Bin File     : %s\n", bin_name);
                    fprintf(mf, "Payload Size : %zu bytes\n", ar_data.size());
                    fclose(mf);
                }
            }
            break;
        }
    }
    if (dbg) fclose(dbg);

    if (ar_data.empty()) {
        out_v1.assign(ntracks, {}); out_v2.assign(ntracks, {});
        return true;  // not in database
    }

    // Parse AR binary format
    auto& data = ar_data;

    // Parse AR binary format:
    //   Header per chunk (13 bytes): [1 track_count][4 disc_id1][4 disc_id2][4 cddb]
    //   Per track (9 bytes): [1 confidence][4 main_crc][4 frame450_crc]
    //
    // main_crc = the track checksum (may be v1 or v2 depending on chunk generation)
    // frame450_crc = checksum of frame 450, used for drive offset detection
    //
    // We store all main_crcs in out_v1 and all frame450_crcs in out_v2.
    // checkAR() will compare our computed crc_v1 against out_v1 entries.
    // crc_v2 = crc_v1 + csum_hi (whipper formula), so a v2 match would be
    // against a chunk whose main_crc was stored as a v2 checksum -- but since
    // we don't know which chunks are v1 vs v2, we try matching both our values.
    out_v1.assign(ntracks, {});
    out_v2.assign(ntracks, {});

    size_t pos = 0;
    while (pos + 13 <= data.size()) {
        uint8_t tc = data[pos];
        uint32_t chunk_id1, chunk_id2, chunk_cddb;
        std::memcpy(&chunk_id1,  data.data()+pos+1, 4);
        std::memcpy(&chunk_id2,  data.data()+pos+5, 4);
        std::memcpy(&chunk_cddb, data.data()+pos+9, 4);
        pos += 13;

        if (tc != (uint8_t)ntracks) { pos += (size_t)tc * 9; continue; }

        for (int t = 0; t < tc && pos+9 <= data.size(); ++t, pos+=9) {
            uint8_t  conf = data[pos];
            uint32_t main_crc, frame450;
            std::memcpy(&main_crc, data.data()+pos+1, 4);
            std::memcpy(&frame450, data.data()+pos+5, 4);
            if (t < ntracks) {
                if (main_crc) out_v1[t].push_back({main_crc, conf});
                if (frame450) out_v2[t].push_back({frame450, conf});
            }
        }
    }
    return true;
}

// ─── CTDB (CUETools Database) ─────────────────────────────────────────────────
// Computes the CUETools Database ID: CRC32 of entire disc audio with first
// and last 10 sectors (10 * 2352 = 23520 bytes) trimmed for offset immunity.
std::string CDRipper::computeCTDBId(const std::vector<uint8_t>& disc_audio) {
    static constexpr size_t TRIM_BYTES = 10 * 2352;

    if (disc_audio.size() <= TRIM_BYTES * 2) return "00000000";

    // Standard IEEE 802.3 CRC32
    static uint32_t table[256];
    static bool     init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }

    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p   = disc_audio.data() + TRIM_BYTES;
    size_t         len = disc_audio.size() - TRIM_BYTES * 2;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    crc ^= 0xFFFFFFFFu;

    char buf[9]; snprintf(buf, sizeof(buf), "%08x", crc);
    return buf;
}

// CTDB server lookup — queries cue.tools for the given CTDB ID.
bool CDRipper::fetchCTDBData(const std::string& ctdb_id,
                              const std::string& log_path,
                              int ntracks,
                              std::string& out_status) {
    out_status = "Unknown";
    if (ctdb_id == "00000000") {
        FILE* lf = port::fopenUtf8(log_path, "a");
        if (lf) { fprintf(lf, "CTDB ID : 00000000 (disc too short to compute)\n"); fclose(lf); }
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url),
        "http://db.cuetools.net/lookup2.php?version=3&ctdb=1&disc=%s&tracks=%d",
        ctdb_id.c_str(), ntracks);

    // Log the request before firing it
    {
        FILE* lf = port::fopenUtf8(log_path, "a");
        if (lf) {
            fprintf(lf, "\n=== CUETools DB ===\n");
            fprintf(lf, "CTDB ID : %s\n", ctdb_id.c_str());
            fprintf(lf, "Tracks  : %d\n", ntracks);
            fprintf(lf, "URL     : %s\n", url);
            fclose(lf);
        }
    }

    core::HttpRequest req;
    req.url        = url;                 // http:// -> no SECURE (urlIsSecureScheme)
    req.user_agent = "RE-MOCT/" REMOCT_VERSION; // CTDB's short UA (not the seam default)
    core::HttpResponse r = core::http().fetch(req);
    uint32_t status = (uint32_t)r.status;
    std::string body = (status == 200) ? r.body : std::string();

    // Parse response — CTDB returns XML or JSON depending on version
    if (body.find("\"status\":\"ok\"")   != std::string::npos ||
        body.find("status=\"ok\"")       != std::string::npos ||
        body.find("<status>ok</status>") != std::string::npos)
        out_status = "Correct";
    else if (body.find("correctable") != std::string::npos)
        out_status = "Correctable";
    else if (status == 404)
        out_status = "Not in database";
    else if (!body.empty())
        out_status = "Unknown";

    // Log full result
    {
        FILE* lf = port::fopenUtf8(log_path, "a");
        if (lf) {
            fprintf(lf, "HTTP    : %lu\n", status);
            fprintf(lf, "Status  : %s\n", out_status.c_str());
            if (!body.empty())
                fprintf(lf, "Response:\n%.2048s\n", body.c_str());
            else
                fprintf(lf, "Response: (empty)\n");
            fclose(lf);
        }
    }
    return status == 200;
}


// ─── Tag writing ──────────────────────────────────────────────────────────────
static std::string arStatusStr(ARStatus s, int conf) {
    switch(s) {
    case ARStatus::Matched_v2: return "AR v2 OK (conf "+std::to_string(conf)+")";
    case ARStatus::Matched_v1: return "AR v1 OK (conf "+std::to_string(conf)+")";
    case ARStatus::NotFound:   return "AR: not in database";
    case ARStatus::NetworkError: return "AR: network error";
    case ARStatus::ReadError:    return "AR: inconclusive (preamble read error)";
    default: return "";
    }
}

void CDRipper::tagFile(const std::string&         path,
                       const MBRelease&            rel,
                       const MBTrack*              mt,
                       int                         track_num,
                       const std::vector<uint8_t>& art,
                       const ARTrackResult&        ar,
                       const RGResult&             rg,
                       RipMode                     mode) {
    try {
#ifdef _WIN32
        auto wp = utf8_to_wide(path);        // TagLib::FileName is wide on Windows
#else
        const std::string& wp = path;        // and UTF-8 bytes elsewhere
#endif
        bool is_mp3  = path.size()>4 && path.substr(path.size()-4)==".mp3";
        bool is_flac = path.size()>5 && path.substr(path.size()-5)==".flac";
        bool is_opus = path.size()>5 && path.substr(path.size()-5)==".opus";

        std::string title_str;
        if (mt && !mt->title.empty()) title_str = mt->title;
        else {
            std::ostringstream ss;
            ss << "Track " << std::setw(2) << std::setfill('0') << track_num;
            title_str = ss.str();
        }
        std::string artist_str = (mt&&!mt->artist.empty()) ? mt->artist : rel.artist;
        std::string ar_str = (isLocal(mode)) ? "" : arStatusStr(ar.status, ar.confidence);
        // CUETools-compatible tag names when available
        char ar_crc_str[9]  = {}; snprintf(ar_crc_str, 9, "%08x", ar.crc_v2);
        char ar_conf_str[8] = {}; snprintf(ar_conf_str, 8, "%d", ar.confidence);

        auto rg_str = [](double gain) {
            std::ostringstream ss; ss << std::fixed << std::setprecision(2) << gain << " dB";
            return ss.str();
        };
        auto rg_peak_str = [](double peak) {
            std::ostringstream ss; ss << std::fixed << std::setprecision(6) << peak;
            return ss.str();
        };

        if (is_mp3) {
            TagLib::MPEG::File f(wp.c_str(), false);
            auto* tag = f.ID3v2Tag(true); if (!tag) return;
            tag->setTitle (TagLib::String(title_str,  TagLib::String::UTF8));
            tag->setArtist(TagLib::String(artist_str, TagLib::String::UTF8));
            tag->setAlbum (TagLib::String(rel.title,  TagLib::String::UTF8));
            tag->setTrack ((unsigned int)track_num);
            if (rel.date.size()>=4) try { tag->setYear((unsigned)std::stoi(rel.date.substr(0,4))); } catch(...){}

            auto addTxt = [&](const char* id, const std::string& val) {
                auto* f2 = new TagLib::ID3v2::TextIdentificationFrame(id, TagLib::String::UTF8);
                f2->setText(TagLib::String(val, TagLib::String::UTF8));
                tag->addFrame(f2);
            };
            addTxt("TENC", "RE-MOCT v" REMOCT_VERSION);
            if (!ar_str.empty()) {
                addTxt("TXXX", "AccurateRip="      + ar_str);
                addTxt("TXXX", "ACCURATERIPCRC="   + std::string(ar_crc_str));
                addTxt("TXXX", "ACCURATERIPCOUNT=" + std::string(ar_conf_str));
            }
            if (rg.valid) {
                addTxt("TXXX", "REPLAYGAIN_TRACK_GAIN="+rg_str(rg.track_gain));
                addTxt("TXXX", "REPLAYGAIN_TRACK_PEAK="+rg_peak_str(rg.track_peak));
                addTxt("TXXX", "REPLAYGAIN_ALBUM_GAIN="+rg_str(rg.album_gain));
                addTxt("TXXX", "REPLAYGAIN_ALBUM_PEAK="+rg_peak_str(rg.album_peak));
            }
            if (!art.empty()) {
                auto* ap = new TagLib::ID3v2::AttachedPictureFrame();
                ap->setMimeType("image/jpeg");
                ap->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
                ap->setDescription("Cover");
                ap->setPicture(TagLib::ByteVector((const char*)art.data(),(unsigned)art.size()));
                tag->addFrame(ap);
            }
            f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);

        } else if (is_flac) {
            TagLib::FLAC::File f(wp.c_str(), false);
            auto* tag = f.xiphComment(true); if (!tag) return;
            tag->setTitle (TagLib::String(title_str,  TagLib::String::UTF8));
            tag->setArtist(TagLib::String(artist_str, TagLib::String::UTF8));
            tag->setAlbum (TagLib::String(rel.title,  TagLib::String::UTF8));
            tag->setTrack ((unsigned int)track_num);
            if (rel.date.size()>=4) try { tag->setYear((unsigned)std::stoi(rel.date.substr(0,4))); } catch(...){}
            tag->addField("ENCODER",TagLib::String("RE-MOCT v" REMOCT_VERSION,TagLib::String::UTF8),true);
            if (!ar_str.empty()) {
                tag->addField("ACCURATERIP",    TagLib::String(ar_str,       TagLib::String::UTF8),true);
                tag->addField("ACCURATERIPCRC", TagLib::String(ar_crc_str,   TagLib::String::UTF8),true);
                tag->addField("ACCURATERIPCOUNT",TagLib::String(ar_conf_str, TagLib::String::UTF8),true);
            }
            if (rg.valid) {
                tag->addField("REPLAYGAIN_TRACK_GAIN",TagLib::String(rg_str(rg.track_gain),TagLib::String::UTF8),true);
                tag->addField("REPLAYGAIN_TRACK_PEAK",TagLib::String(rg_peak_str(rg.track_peak),TagLib::String::UTF8),true);
                tag->addField("REPLAYGAIN_ALBUM_GAIN",TagLib::String(rg_str(rg.album_gain),TagLib::String::UTF8),true);
                tag->addField("REPLAYGAIN_ALBUM_PEAK",TagLib::String(rg_peak_str(rg.album_peak),TagLib::String::UTF8),true);
            }
            if (!art.empty()) {
                auto* pic = new TagLib::FLAC::Picture();
                pic->setMimeType("image/jpeg");
                pic->setType(TagLib::FLAC::Picture::FrontCover);
                pic->setDescription(TagLib::String("Cover"));
                pic->setData(TagLib::ByteVector((const char*)art.data(),(unsigned)art.size()));
                f.addPicture(pic);
            }
            f.save();

        } else if (is_opus) {
            // Opus (rip-opus-encoder): Xiph comments like FLAC — standard
            // fields, AR fields, and art (XiphComment carries pictures
            // natively) are the FLAC branch's calls verbatim. ReplayGain is
            // the R128 DIALECT ONLY (RFC 7845): Q7.8 integers via the shared
            // R128Gain.h conversion — the exact inverse of the decode side.
            // No REPLAYGAIN_* keys, no peaks (R128 defines none). The header
            // output gain is 0 (OpusRipEncoder pins it), so these tags are
            // the only gain.
            TagLib::Ogg::Opus::File f(wp.c_str(), false);
            auto* tag = f.tag(); if (!tag) return;
            tag->setTitle (TagLib::String(title_str,  TagLib::String::UTF8));
            tag->setArtist(TagLib::String(artist_str, TagLib::String::UTF8));
            tag->setAlbum (TagLib::String(rel.title,  TagLib::String::UTF8));
            tag->setTrack ((unsigned int)track_num);
            if (rel.date.size()>=4) try { tag->setYear((unsigned)std::stoi(rel.date.substr(0,4))); } catch(...){}
            tag->addField("ENCODER",TagLib::String("RE-MOCT v" REMOCT_VERSION,TagLib::String::UTF8),true);
            if (!ar_str.empty()) {
                tag->addField("ACCURATERIP",    TagLib::String(ar_str,       TagLib::String::UTF8),true);
                tag->addField("ACCURATERIPCRC", TagLib::String(ar_crc_str,   TagLib::String::UTF8),true);
                tag->addField("ACCURATERIPCOUNT",TagLib::String(ar_conf_str, TagLib::String::UTF8),true);
            }
            if (rg.valid) {
                tag->addField("R128_TRACK_GAIN",
                    TagLib::String(std::to_string(r128FromDb(rg.track_gain)),TagLib::String::UTF8),true);
                tag->addField("R128_ALBUM_GAIN",
                    TagLib::String(std::to_string(r128FromDb(rg.album_gain)),TagLib::String::UTF8),true);
            }
            if (!art.empty()) {
                auto* pic = new TagLib::FLAC::Picture();
                pic->setMimeType("image/jpeg");
                pic->setType(TagLib::FLAC::Picture::FrontCover);
                pic->setDescription(TagLib::String("Cover"));
                pic->setData(TagLib::ByteVector((const char*)art.data(),(unsigned)art.size()));
                tag->addPicture(pic);
            }
            f.save();
        }
    } catch(...) {}
}

// ─── Core track rip ───────────────────────────────────────────────────────────
// ── Format-selection helpers (rip-format-select) ─────────────────────────────
// The one place a RipFormat becomes a concrete IEncoder. Quality values come
// from RipOptions (config-fed; defaults == the pre-config literals, pinned by
// the seam oracle's argument-free constructions).
static std::unique_ptr<IEncoder> makeEncoder(RipFormat f, const RipOptions& opt) {
    switch (f) {
        case RipFormat::Flac: return std::make_unique<FlacEncoder>(opt.flac_level);
        case RipFormat::Mp3:  return std::make_unique<Mp3Encoder>(opt.mp3_vbr_q);
        case RipFormat::Wav:  return std::make_unique<WavEncoder>();
        case RipFormat::Opus: return std::make_unique<OpusRipEncoder>(opt.opus_bitrate);
    }
    return nullptr;
}

// Per-track output list from the selection: table-ordered, absent formats
// simply not present (no path, no encoder, no temp siblings — subset
// correctness by construction).
static std::vector<CDRipper::RipOutput> buildOuts(const RipOptions& opt,
                                                  const std::string& out_dir,
                                                  const std::string& prefix) {
    std::vector<CDRipper::RipOutput> v;
    for (RipFormat f : opt.formats)
        if (const RipFormatRow* r = ripFormatRow(f))
            v.push_back({ f, out_dir + kSep + prefix + r->ext });
    return v;
}

// The .tmp/.det/.probe sibling lists the re-rip paths use.
static std::vector<CDRipper::RipOutput> withSuffix(std::vector<CDRipper::RipOutput> outs,
                                                   const char* sfx) {
    for (auto& o : outs) o.path += sfx;
    return outs;
}

ARTrackResult CDRipper::ripTrack(core::ICdDevice&   dev,
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
                                 int                drive_offset,
                                 uint32_t           ctdb_crc_in,
                                 size_t             ctdb_bytes_in,
                                 int                pressing_offset,
                                 size_t             ctdb_total_bytes,
                                 ebur128_state**    out_ebur) {

    ARTrackResult ar_result;
    ar_result.status = ARStatus::NotQueried;
    rg_out = {};

    // CTDB seed from caller (accumulated across tracks)
    uint32_t ctdb_crc_seed   = ctdb_crc_in;
    size_t   ctdb_bytes_seed = ctdb_bytes_in;

    const uint32_t total_secs = (uint32_t)track.length_lba;
    if (total_secs == 0) return ar_result;

    // ── Encoder fan-out (rip-encoder-seam / rip-format-select) ────────────
    // One IEncoder per SELECTED output, in list order (table order — the
    // pre-selection FLAC-then-MP3 sequence for the default set). Each is a
    // pure PCM consumer of the read loop's blocks; tags/art/RG happen later
    // in tagFile, unchanged. On an open failure, unwind the already-opened
    // encoders exactly as the inline code did: finalize (a lossless
    // encoder's finish flushes a valid-but-empty file, deliberately LEFT on
    // disk — the worker's zero-CRC heuristic catches it) and return.
    if (outs.empty()) return ar_result;   // UI guarantees >= 1; belt-and-suspenders
    std::vector<std::unique_ptr<IEncoder>> encoders;
    for (const auto& o : outs) encoders.push_back(makeEncoder(o.fmt, opt));
    for (size_t ei = 0; ei < encoders.size(); ++ei) {
        if (!encoders[ei] ||
            !encoders[ei]->open(outs[ei].path,
                                (uint64_t)total_secs * SECTOR_SAMPLES)) {
            for (size_t j = 0; j < ei; ++j) encoders[j]->finalize(false);
            return ar_result;
        }
    }

    // ── libebur128 for ReplayGain ─────────────────────────────────────────
    ebur128_state* ebur = ebur128_init(CHANNELS, SAMPLE_RATE,
                                       EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);

    // ── AccurateRip CRC state ─────────────────────────────────────────────
    // From whipper accuraterip-checksum.c (authoritative reference):
    //   MulBy increments for EVERY sample (including skipped ones at boundaries)
    //   Skip implemented via range check: only accumulate if MulBy in valid range
    //   audio_data is uint32_t* -- each element = one stereo frame (L+R packed)
    //   v1 = csum_lo  (lower 32 bits of each product, summed)
    //   v2 = csum_lo + csum_hi  (sum of lower + sum of upper 32 bits)
    static constexpr uint32_t AR_SKIP = 5u * 588u;  // 2940 samples to skip front/back
    const uint32_t total_samples = (uint32_t)track.length_lba * SECTOR_SAMPLES;
    // AR uses disc-absolute mul_by (pregap-inclusive, 150 sectors before our track start).
    // mul_by starts at track.start_lba*588+1 for our first audio sample.
    // ar_check_from = 2940 for track 1 (disc-absolute, well before our audio start).
    // ar_check_to   = total_samples (= dBpoweramp DataDWORDSize); with our offset mul_by
    //   this naturally excludes the last 150 sectors of our rip (which belong to the
    //   next track in AccurateRip's flat-disc scheme).
    const uint32_t ar_check_from = is_first ? AR_SKIP : 1u;
    // Guard against underflow on pathologically short tracks (< 5 sectors).
    // total_samples - AR_SKIP would wrap to ~4 billion as uint32_t.
    const uint32_t ar_check_to   = is_last
        ? (total_samples > AR_SKIP ? total_samples - AR_SKIP : 0u)
        : total_samples;

    // AccurateRip uses disc-absolute positions. Each track's CRC starts mul_by=1 at
    // the disc-relative track start (= track.start_lba - 150 sectors from disc LBA 0).
    // We read from track.start_lba so must first feed the 150 preceding sectors as a
    // "preamble" with mul_by 1..88200.  For track 1 this area contains real disc data
    // (not silence) that contributes to the CRC.
    static constexpr uint32_t PREGAP_SAMPLES = 150u * 588u;  // 88200
    // Pure AR CRC accumulator (see ar_crc.h); fed at the three sites below.
    ar::TrackCrc arc(ar_check_from, ar_check_to, is_first, !isLocal(mode));

    // ── CTDB CRC32 state (incremental across entire disc) ─────────────────
    // CTDB trims first/last 10 sectors of the disc (not per track).
    // We pass in the running state from previous tracks via ar_result.ctdb_crc,
    // accumulate this track's bytes, and pass it back for the next track.
    // The caller finalizes with XOR 0xFFFFFFFF after the last track.
    // Build CRC32 lookup table once
    static uint32_t crc32_table[256] = {};
    static bool     crc32_init = false;
    if (!crc32_init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            crc32_table[i] = c;
        }
        crc32_init = true;
    }
    // Running CRC state — seed from previous track's state
    uint32_t ctdb_crc   = ctdb_crc_seed;
    size_t   ctdb_bytes = ctdb_bytes_seed;
    size_t ctdb_disc_offset = ctdb_bytes;

    static constexpr size_t CTDB_TRIM = 10 * (size_t)SECTOR_BYTES;  // 23520 bytes
    // CTDB trims the first AND last 10 sectors of the disc audio image for offset
    // immunity. The start trim is gated by abs_pos >= CTDB_TRIM below; the end trim
    // needs the disc total up front (CRC32 can't be un-fed), so the worker passes
    // ctdb_total_bytes. 0 = caller didn't supply it -> start-trim only (legacy).
    const size_t ctdb_end_trim_at = (ctdb_total_bytes > CTDB_TRIM)
                                  ? (ctdb_total_bytes - CTDB_TRIM) : 0;

    // Per-track PCM CRC32: CRC32 of the raw audio bytes fed to FLAC/AR.
    // Compared against dBpoweramp's "CRC32:" field to verify audio content.
    uint32_t pcm_crc32 = 0xFFFFFFFFu;

    // ── Streaming read + encode loop ──────────────────────────────────────
    static constexpr int BUF_SECTORS = SECTORS_PER_READ;
    static constexpr int BUF_BYTES   = BUF_SECTORS * SECTOR_BYTES;

    uint8_t raw_buf[BUF_BYTES];
    float   ebur_interleaved[BUF_SECTORS * SECTOR_SAMPLES * 2]; // interleaved L+R
    // (the per-format staging buffers moved into FlacEncoder/Mp3Encoder)

    // ── Drive + pressing offset correction ───────────────────────────────────
    // total_skip = drive_offset + pressing_offset. May span multiple sectors.
    // We advance start LBA by whole sectors; sub-sector remainder skipped from
    // first chunk; tail borrows corr_sub_skip samples from beyond the track end.
    //
    // remaining = track.length_lba: always produce exactly track.length_lba * SECTOR_SAMPLES:
    //   (track.length_lba sectors × 588) - corr_sub_skip [loop] + corr_sub_skip [tail]
    //
    // The tail reads from LBA (track.start_lba + corr_lba_adv + track.length_lba).
    // The drive delivers that sector starting corr_sub_skip samples before the
    // disc position we need — NO additional skip within the sector is needed.
    // (The lba advance already accounts for the drive's early-read margin.)
    const int total_skip    = drive_offset + pressing_offset;
    // Floored decomposition (ar::normalizeSkip): keeps corr_sub_skip in [0,588) for
    // EITHER sign. Truncating / and % gave a NEGATIVE corr_sub_skip on negative-offset
    // drives -> preamble ptr underflow (OOB) + dropped main-path skip (wrong CRC phase).
    // For non-negative total_skip this is byte-identical to the old / and %.
    const ar::SkipParts skip = ar::normalizeSkip(total_skip);
    const int corr_lba_adv  = skip.lba_adv;
    const int corr_sub_skip = skip.sub_skip;
    bool corr_first_done    = false;

    uint32_t lba       = (uint32_t)((long long)track.start_lba + corr_lba_adv);  // signed: adv may be < 0
    uint32_t remaining = (uint32_t)track.length_lba;   // always full track length
    bool  ok        = true;
    uint32_t done_secs = 0;
    int   total_c2_errors = 0;
    auto  rip_start = std::chrono::steady_clock::now();

    // ── AccurateRip preamble ───────────────────────────────────────────────
    // AccurateRip CRCs start mul_by=1 at (track.start_lba - 150) = track.rel_lba.
    // We read from track.start_lba, so must first feed the 150 preceding sectors
    // through the AR CRC accumulator (not FLAC/LAME).  These sectors contain real
    // disc data (including any disc lead-in content before the nominal track start).
    // Apply the same total_skip offset correction used for the main rip.
    // Set true if the AR preamble can't be read — its CRC contribution would be
    // silent zeros, so we mark the whole track's AR result indeterminate downstream
    // rather than letting a poisoned checksum masquerade as a real AR mismatch.
    bool preamble_failed = false;

    if (!isLocal(mode) && !cancel_.load() &&
        ar::arPreambleReadable(track.start_lba, corr_lba_adv)) {
        // preamble_lba: 150 sectors before our effective read start (signed: adv may be < 0)
        const uint32_t preamble_lba =
            (uint32_t)((long long)track.start_lba - 150 + corr_lba_adv);
        // Need enough sectors to produce PREGAP_SAMPLES after the sub-sector skip.
        // ceil((PREGAP_SAMPLES + corr_sub_skip) / SECTOR_SAMPLES) + 1 for safety.
        const int preamble_secs = (int)((PREGAP_SAMPLES + corr_sub_skip + SECTOR_SAMPLES - 1)
                                        / SECTOR_SAMPLES) + 1;
        std::vector<int16_t> pbuf((size_t)preamble_secs * SECTOR_SAMPLES * 2);
        {
            std::vector<uint8_t> raw((size_t)preamble_secs * SECTOR_BYTES);
            int c2d = 0;
            if (readSectorsWithRetry(dev, preamble_lba, preamble_secs,
                                     raw.data(), use_c2, &c2d)) {
                std::memcpy(pbuf.data(), raw.data(),
                            (size_t)preamble_secs * SECTOR_SAMPLES * 2 * sizeof(int16_t));
            } else {
                // Read failed: pbuf stays zero. Feeding those zeros into the AR
                // accumulator would yield a checksum that silently never matches
                // (a good rip reported as "not in database"). Flag it instead.
                preamble_failed = true;
                FILE* lf = port::fopenUtf8(log_path, "a");
                if (lf) {
                    fprintf(lf, "  WARNING T%d: AR preamble read FAILED at LBA %lu "
                                "(%d sectors). AR CRC marked indeterminate; "
                                "audio still ripped and kept.\n",
                            track.number, (unsigned long)preamble_lba, preamble_secs);
                    fclose(lf);
                }
            }
        }
        // Skip corr_sub_skip frames (same sub-sector offset as main rip)
        const int16_t* ps = pbuf.data() + corr_sub_skip * 2;
        for (uint32_t pi = 0; pi < PREGAP_SAMPLES && !cancel_.load(); ++pi) {
            arc.sample(ps[pi * 2], ps[pi * 2 + 1]);
        }
    } else {
        // Preamble is before disc start (shouldn't happen on normal CDs) — advance mul_by
        arc.skip(PREGAP_SAMPLES);
    }

    while (remaining > 0 && !cancel_.load()) {
        uint32_t this_read = std::min((uint32_t)BUF_SECTORS, remaining);
        int   c2_errs   = 0;

        if (!readSectorsWithRetry(dev, lba, (int)this_read, raw_buf,
                                  use_c2, &c2_errs)) {
            ok = false; break;
        }
        total_c2_errors += c2_errs;

        int samples = (int)this_read * SECTOR_SAMPLES;
        const int16_t* src = reinterpret_cast<const int16_t*>(raw_buf);

        // On the first chunk, skip the sub-sector remainder. corr_sub_skip is now
        // always >= 0 (ar::normalizeSkip), so no `> 0` gate: a negative skip used to
        // be silently dropped here, misaligning the track's mul_by (wrong CRC phase).
        if (!corr_first_done) {
            src     += corr_sub_skip * 2;
            samples -= corr_sub_skip;
            corr_first_done = true;
        }

        // Hex dump first 16 samples of track 1 for CRC verification
        if (is_first && done_secs == 0 && track_idx == 0) {
            FILE* lf = port::fopenUtf8(log_path, "a");
            if (lf) {
                fprintf(lf, "  [Track 1 sector 0 first 16 samples (L R pairs)]:\n  ");
                for (int d = 0; d < 16 && d < samples; ++d) {
                    int16_t dl = src[d*2], dr = src[d*2+1];
                    uint32_t ds = (uint32_t)(uint16_t)dl | ((uint32_t)(uint16_t)dr << 16);
                    fprintf(lf, " %08x", ds);
                    if (d == 7) fprintf(lf, "\n  ");
                }
                fprintf(lf, "\n  ar_check_from=%u ar_check_to=%u total_samples=%u\n",
                        ar_check_from, ar_check_to, (uint32_t)track.length_lba * SECTOR_SAMPLES);
                fclose(lf);
            }
        }

        // Track sample offset within the track for AR boundary logic
        uint32_t samp_offset = done_secs * SECTOR_SAMPLES;
        (void)samp_offset;

        for (int i = 0; i < samples; ++i) {
            int16_t l = src[i*2], r = src[i*2+1];
            ebur_interleaved[i*2]    = (float)l / 32768.0f;
            ebur_interleaved[i*2+1]  = (float)r / 32768.0f;

            const uint64_t accBefore = arc.nAccumulated();
            arc.sample(l, r);
            if (is_first && accBefore == 0 && arc.nAccumulated() == 1) {
                FILE* lf = port::fopenUtf8(log_path, "a");
                if (lf) {
                    fprintf(lf, "  first_accum: mul_by=%u samp32=%08x "
                            "contrib_lo=%08x total_skip=%d\n",
                            arc.firstMulBy(), arc.firstSamp32(), arc.firstContribLo(), total_skip);
                    fclose(lf);
                }
            }
        }

        // PCM CRC32 over this chunk (same bytes fed to FLAC encoder)
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(src);
            for (int n = 0; n < samples * 4; ++n)
                pcm_crc32 = crc32_table[(pcm_crc32 ^ b[n]) & 0xFF] ^ (pcm_crc32 >> 8);
        }

        // Encoders (FLAC then MP3 — the inline order). A failure breaks the
        // rip exactly as the inline `ok=false; break` pairs did: later
        // encoders in the list, ReplayGain, and CTDB are skipped this block.
        {
            bool enc_ok = true;
            for (auto& e : encoders)
                if (!e->writeFrames(src, (size_t)samples)) { enc_ok = false; break; }
            if (!enc_ok) { ok = false; break; }
        }

        // ReplayGain -- interleaved stereo. `samples` is already the stereo-frame
        // count (same value passed to FLAC/LAME), which is exactly what
        // ebur128_add_frames_float expects. Do NOT divide by CHANNELS.
        if (ebur)
            ebur128_add_frames_float(ebur, ebur_interleaved, (size_t)samples);

        // ── CTDB CRC32 — accumulate raw sector bytes (disc-absolute, trimmed) ──
        if (mode == RipMode::CUETools) {
            size_t block_bytes = (size_t)this_read * SECTOR_BYTES;
            for (size_t bi = 0; bi < block_bytes; ++bi) {
                size_t abs_pos = ctdb_disc_offset + bi;
                // Skip the disc-start trim (first 10 sectors) and, when the disc
                // total is known, the disc-end trim (last 10 sectors) as well.
                if (abs_pos >= CTDB_TRIM &&
                    (ctdb_total_bytes == 0 || abs_pos < ctdb_end_trim_at)) {
                    ctdb_crc = crc32_table[(ctdb_crc ^ raw_buf[bi]) & 0xFF] ^ (ctdb_crc >> 8);
                }
            }
            ctdb_disc_offset += block_bytes;
            ctdb_bytes        = ctdb_disc_offset;
        }

        lba       += this_read;
        remaining -= this_read;
        done_secs += this_read;

        // ── Progress + rip speed ────────────────────────────────────────────
        int pct = (int)(100.0 * done_secs / total_secs);
        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - rip_start).count();
        double speed_x = elapsed_sec > 0.1
                       ? (double)done_secs / (elapsed_sec * 75.0) : 0.0;

        if (cb) {
            RipProgress p;
            p.state    = RipState::Ripping;
            p.track    = track_idx + 1;
            p.total    = total_tracks;
            p.pct      = pct;
            p.using_c2 = use_c2;
            std::ostringstream ss;
            ss << "Track " << (track_idx+1) << "/" << total_tracks
               << "  [" << pct << "%]";
            if (speed_x >= 0.1)
                ss << "  " << std::fixed << std::setprecision(1) << speed_x << "x";
            if (use_c2 && c2_errs > 0) ss << "  C2!";
            else if (use_c2)            ss << "  C2";
            p.status_msg = ss.str();
            cb(p);
        }
    }

    // ── Offset correction tail ─────────────────────────────────────────────
    // Borrow corr_sub_skip samples from the sector just past the track end.
    // The lba advance already positions us exactly: delivery starts at the
    // disc sample we need, no additional skip within the sector required.
    // For the last track, zero-pad (falls inside AR_SKIP exclusion zone).
    if (ok && !cancel_.load() && corr_sub_skip > 0) {
        int16_t tail[SECTOR_SAMPLES * 2] = {};
        if (!is_last) {
            uint8_t extra_raw[SECTOR_BYTES]; int c2d = 0;
            readSectorsWithRetry(dev, lba, 1, extra_raw, use_c2, &c2d);
            // The delivery starts exactly at the disc position we need — no skip
            std::memcpy(tail, extra_raw, (size_t)corr_sub_skip * 2 * sizeof(int16_t));
        }
        const int16_t* src = tail;
        int samples = corr_sub_skip;
        for (int i = 0; i < samples; ++i) {
            int16_t l = src[i*2], r = src[i*2+1];
            ebur_interleaved[i*2]   = (float)l / 32768.0f;
            ebur_interleaved[i*2+1] = (float)r / 32768.0f;
            arc.sample(l, r);
        }
        // PCM CRC32 over the tail bytes
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(tail);
            for (int n = 0; n < samples * 4; ++n)
                pcm_crc32 = crc32_table[(pcm_crc32 ^ b[n]) & 0xFF] ^ (pcm_crc32 >> 8);
        }
        // Encoders on the borrowed tail samples — FLAC-fail skips MP3, as the
        // inline `if (ok)` gate did; ebur still runs after, also as before.
        for (auto& e : encoders)
            if (!e->writeFrames(src, (size_t)samples)) { ok = false; break; }
        if (ebur) ebur128_add_frames_float(ebur, ebur_interleaved,
                                           (size_t)samples);
    }

    // ── Flush encoders ────────────────────────────────────────────────────
    // FLAC first (list order = the inline finish order). ok=false skips the
    // LAME flush/Xing rewrite but still closes handles, exactly as before.
    for (auto& e : encoders) e->finalize(ok);

    // ── ReplayGain result ─────────────────────────────────────────────────
    if (ebur && ok) {
        double loudness = 0;
        if (ebur128_loudness_global(ebur, &loudness) == EBUR128_SUCCESS) {
            // EBU R128: target = -23 LUFS; RG reference = -18 LUFS
            rg_out.track_gain = -18.0 - loudness;
            double peak = 0;
            ebur128_true_peak(ebur, 0, &peak);
            double peak_r = 0;
            ebur128_true_peak(ebur, 1, &peak_r);
            rg_out.track_peak = std::max(peak, peak_r);
            rg_out.valid = true;
        }
    }
    // Hand the integrated-loudness state back to the worker so it can compute
    // true album gain across every kept track (ebur128_loudness_global_multiple).
    // Ownership transfers ONLY on a clean, kept rip; probe/determinism/discarded
    // passes (out_ebur == nullptr, or a failed rip) destroy it here as before.
    if (out_ebur && ebur && ok && !cancel_.load()) {
        *out_ebur = ebur;
        ebur      = nullptr;
    }
    if (ebur) ebur128_destroy(&ebur);

    if (!ok || cancel_.load()) {
        for (const auto& o : outs) fs::remove(o.path);
        return ar_result;
    }

    ar_result.crc_v1     = arc.v1();
    ar_result.crc_v2     = arc.v2();
    ar_result.ctdb_crc   = ctdb_crc;
    ar_result.ctdb_bytes = ctdb_bytes;
    ar_result.frame450_local = arc.frame450Local();

    // Preamble read failure -> the AR CRC accumulated silent zeros for the pregap
    // region and cannot be trusted. Keep the audio (already written above), but
    // mark AR inconclusive so verification + Pass 2 are skipped and the user sees
    // it in the log. frame450 (sector 450 of the main rip) is unaffected, so the
    // drive-offset self-check below remains valid.
    if (preamble_failed) ar_result.status = ARStatus::ReadError;

    // Log diagnostic directly here (before returning)
    if (!isLocal(mode) && !log_path.empty()) {
        FILE* lf = port::fopenUtf8(log_path, "a");
        if (lf) {
            // expected_n: preamble samples in range + main rip samples in range
            const uint32_t preamble_from = std::max(ar_check_from, 1u);
            const uint32_t preamble_to   = std::min((uint32_t)PREGAP_SAMPLES, ar_check_to);
            const uint32_t main_from     = std::max(ar_check_from,
                                                     (uint32_t)(PREGAP_SAMPLES + 1u));
            uint64_t expected_n = 0;
            if (preamble_to >= preamble_from)
                expected_n += preamble_to - preamble_from + 1;
            if (ar_check_to >= main_from)
                expected_n += ar_check_to - main_from + 1;
            fprintf(lf, "  AR diag T%d: csum_lo=%08x csum_hi=%08x n_accum=%llu expected=%llu %s\n",
                track.number,
                arc.csumLo(), arc.csumHi(),
                (unsigned long long)arc.nAccumulated(),
                (unsigned long long)expected_n,
                arc.nAccumulated() == expected_n ? "COUNT OK" : "COUNT MISMATCH!");
            fprintf(lf, "  pcm_crc32=%08x  (dBpoweramp CRC32 for T1=b02e7e1a)\n",
                    pcm_crc32 ^ 0xFFFFFFFFu);
            if (is_first)
                fprintf(lf, "  frame450: global=%08x local=%08x  (total_skip=%d)\n",
                        arc.frame450Global(), arc.frame450Local(), total_skip);
                // Drive offset self-check on track 1: compare frame450_local
                // against DB frame450 entries. A match confirms offset is correct;
                // a miss with an unknown drive warns the user loudly.
                // (Only meaningful for track 1 — frame450 is defined for track 1 only.)
            fclose(lf);
        }
    }
    return ar_result;
}

// ─── Worker thread ────────────────────────────────────────────────────────────
void CDRipper::worker(std::string          drive_letter,
                      std::vector<CDTrack> tracks,
                      std::string          out_dir,
                      MBRelease            rel,
                      RipMode              mode,
                      RipOptions           opt,
                      ProgressCb           cb,
                      std::unique_ptr<core::ICdDevice> dev,
                      int                  drive_offset,
                      std::string          drive_model,
                      uint32_t             full_leadout_lba,
                      std::vector<uint32_t> data_track_lbas) {

    state_.store(RipState::Ripping);

    // ── Multi-disc layout ─────────────────────────────────────────────────
    // rel carries EVERY disc's tracks (tagged with .disc). Select the disc in
    // the drive by matching the physical track count to a medium, and — only
    // for true multi-disc releases — nest this disc's output in "...\Disc N\"
    // so each disc folder is self-contained. Single-disc releases stay flat.
    const int current_disc = pickDiscForTrackCount(rel, (int)tracks.size());
    int total_discs = 1;
    for (const auto& t : rel.tracks) if (t.disc > total_discs) total_discs = t.disc;
    if (total_discs > 1)
        out_dir += kSep + "Disc " + std::to_string(current_disc);

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        if (cb) { RipProgress p; p.state=RipState::Error;
                  p.status_msg="Rip failed: could not create output dir"; cb(p); }
        state_.store(RipState::Error); active_.store(false); return;
    }

    if (!dev) {
        // Codeless message: the seam surfaces no OS error codes (IHttp/IIpc/INotify
        // contract). The baseline's GetLastError here was read on the wrong thread
        // (openDrive ran on the caller's) — it never reported the real code anyway.
        if (cb) { RipProgress p; p.state=RipState::Error;
                  p.status_msg="Rip failed: could not open drive"; cb(p); }
        state_.store(RipState::Error); active_.store(false); return;
    }

    // ── Per-rip log + AR cache dir ────────────────────────────────────────
    std::string log_dir      = out_dir + kSep + "logs";
    std::string ar_cache_dir = out_dir + kSep + ".ar-db-info";
    std::string log_path;
    {
        fs::create_directories(log_dir, ec);
        std::time_t t = std::time(nullptr);
        std::tm tmbuf{}; localtimeSafe(t, tmbuf); std::tm* tm = &tmbuf;
        char ts[32]; std::strftime(ts, sizeof(ts), "rip_%Y%m%d_%H%M%S", tm);
        log_path = log_dir + kSep + ts + ".log";
        FILE* lf = port::fopenUtf8(log_path, "w");
        if (lf) {
            fprintf(lf, "RE-MOCT CD Rip Log\n");
            fprintf(lf, "==================\n");
            fprintf(lf, "Album  : %s\n", rel.title.c_str());
            fprintf(lf, "Artist : %s\n", rel.artist.c_str());
            fprintf(lf, "Year   : %s\n", rel.date.c_str());
            fprintf(lf, "Drive  : %s  (offset %+d samples)\n",
                    drive_letter.c_str(), drive_offset);
            fprintf(lf, "Model  : %s\n", drive_model.empty() ? "unknown" : drive_model.c_str());
            fprintf(lf, "Tracks : %d\n", (int)tracks.size());
            fprintf(lf, "Mode   : %s\n",
                mode==RipMode::AccurateRip ? "AccurateRip"    :
                mode==RipMode::CUETools    ? "CUETools"       :
                mode==RipMode::LocalVerify ? "Local (2-pass)" : "Local");
            fprintf(lf, "Output : %s\n\n", out_dir.c_str());
            fclose(lf);
        }
    }
    bool use_c2 = probeC2(*dev);
    {
        FILE* lf = port::fopenUtf8(log_path, "a");
        if (lf) {
            fprintf(lf, "C2 support  : %s\n", use_c2 ? "yes" : "no");
            fprintf(lf, "Drive cache : defeated via 4 MB seek-flush before re-reads\n");
            fprintf(lf, "            (read-ahead cache evicted between passes so dual-pass\n");
            fprintf(lf, "            determinism reflects the platter, not the RAM cache.)\n\n");
            fclose(lf);
        }
    }
    if (cb) {
        RipProgress p; p.state=RipState::Ripping; p.using_c2=use_c2;
        p.status_msg = use_c2
            ? "Drive supports C2 error pointers — using C2-assisted rip"
            : "C2 not supported by drive — using standard rip with retry";
        cb(p);
    }
    port::sleepMs(800);

    // ── Drive read offset ─────────────────────────────────────────────────
    // The drive's read offset (in samples) corrects for the hardware timing
    // difference between where the drive thinks a sector starts and where
    // the audio actually begins. Without this correction the AR CRC will
    // never match the database regardless of pressing.
    //
    // The offset is drive-model-specific and must come from the AccurateRip
    // drive offset database (http://www.accuraterip.com/driveoffsets.htm) or
    // be detected by ripping a known disc. We query it from CDSource, which
    // is expected to expose it (looked up by drive model string at init time).
    // If CDSource doesn't provide it yet, it defaults to 0 (no correction),
    // which will still work for the small number of drives with offset 0.
    // drive_offset (samples, signed) is passed in from start() via worker() parameter.

    // ── Cover art ─────────────────────────────────────────────────────────
    std::vector<uint8_t> art;
    {
        if (cb) { RipProgress p; p.state=RipState::Ripping;
                  p.status_msg="Fetching cover art..."; cb(p); }
        // 1) Cover Art Archive by MBID — unchanged primary path for MB releases.
        if (!rel.mb_id.empty())
            art = CoverArt::bytesByMbid(rel.mb_id);
        // 2) Fallback ONLY if CAA gave nothing: Discogs releases (no MBID) and
        //    the occasional MB release with no CAA front cover. Open, no-auth,
        //    keyed by artist+album (iTunes -> Deezer). Never runs when step 1
        //    already returned art, so existing MB behaviour is untouched.
        if (art.empty())
            art = CoverArt::bytesByText(rel.artist, rel.title);
        if (!art.empty()) {
            FILE* f = port::fopenUtf8(out_dir+kSep+"folder.jpg", "wb");
            if (f) { fwrite(art.data(),1,art.size(),f); fclose(f); }
        }
    }

    // ── AccurateRip prefetch (AR mode only) ───────────────────────────────
    std::vector<std::vector<std::pair<uint32_t,int>>> ar_db_v1, ar_db_v2;
    bool ar_db_loaded = false;
    if (mode == RipMode::AccurateRip) {
        if (cb) { RipProgress p; p.state=RipState::Ripping;
                  p.status_msg="Fetching AccurateRip database..."; cb(p); }
        ar_db_loaded = fetchARData(tracks, log_path, ar_cache_dir, ar_db_v1, ar_db_v2, full_leadout_lba, data_track_lbas);
        if (!ar_db_loaded && cb) {
            RipProgress p; p.state=RipState::Ripping;
            p.status_msg="AccurateRip: network error -- AR verification skipped";
            cb(p); port::sleepMs(600);
        } else if (ar_db_loaded && ar_db_v1.empty() && cb) {
            RipProgress p; p.state=RipState::Ripping;
            p.status_msg="AccurateRip: this pressing not yet in AR database";
            cb(p); port::sleepMs(600);
        }
    } else {
        // Give the drive a moment to become ready (AR mode gets this naturally
        // from the network fetch delay; Local/CUETools mode need an explicit wait)
        port::sleepMs(500);
    }

    // ── Enhanced CD: find true audio end for last track ──────────────────
    // Binary search is done here (not in CDSource::open) to avoid seeking
    // the disc before ripping which drops drive speed from 20-30x to 8x.
    // For Enhanced CDs the last audio track's TOC end points to the data
    // track start which includes silence + hidden content in its pregap.
    // We find the first silent sector and cap the last track's length_lba.
    if (!data_track_lbas.empty() && !tracks.empty()) {
        uint32_t data_start  = data_track_lbas[0];
        uint32_t track_start = tracks.back().start_lba;
        // Clamp search range to within the last audio track
        // so we never accidentally find silence in a previous track.
        uint32_t search_lo = std::max(track_start,
                              data_start > 15000 ? data_start - 15000 : 0u);
        uint32_t search_hi = data_start;
        static constexpr int SB = CDSource::SECTOR_BYTES;
        static constexpr int SS = CDSource::SECTOR_SAMPLES;
        std::vector<uint8_t> probe(SB);
        // Single-sector plain-CDDA probes — no C2 even when use_c2 (baseline shape)
        std::size_t bpr = 0;
        bool lo_ok = dev->readRaw(search_lo, 1, false, probe.data(), SB, bpr);
        bool lo_silent = true;
        if (lo_ok) {
            const int16_t* s = reinterpret_cast<const int16_t*>(probe.data());
            for (int i = 0; i < SS * 2; ++i)
                if (s[i] != 0) { lo_silent = false; break; }
        }
        if (lo_ok && !lo_silent) {
            while (search_hi > search_lo + 1) {
                uint32_t mid = (search_lo + search_hi) / 2;
                bool ok = dev->readRaw(mid, 1, false, probe.data(), SB, bpr);
                bool silent = true;
                if (ok) {
                    const int16_t* s = reinterpret_cast<const int16_t*>(probe.data());
                    for (int i = 0; i < SS * 2; ++i)
                        if (s[i] != 0) { silent = false; break; }
                }
                if (silent) search_hi = mid;
                else         search_lo = mid;
            }
            // search_hi = first fully-zero sector; +174 = AR-consistent track end.
            // Cap at data_start to never read into the data session.
            uint32_t audio_end = std::min(search_hi + 174, data_start);
            // Apply to last audio track
            auto& last_trk = tracks.back();
            uint32_t toc_end = last_trk.start_lba + last_trk.length_lba;
            if (toc_end > audio_end) {
                last_trk.length_lba   = audio_end - last_trk.start_lba;
                last_trk.duration_sec = (int)(last_trk.length_lba / 75);
            }
        }
    }

    // ── Per-track rip ─────────────────────────────────────────────────────
    setDriveSpeed(*dev, 0xFFFF);
    int total = (int)tracks.size();

    // CTDB end-trim needs the disc audio total up front. The byte stream CTDB
    // accumulates is exactly each track's length_lba*SECTOR_BYTES in order, so
    // sum it here — AFTER the Enhanced-CD last-track cap above so the total
    // reflects the real audio end, not the TOC's data-session boundary.
    size_t ctdb_total_bytes = 0;
    for (const auto& t : tracks)
        ctdb_total_bytes += (size_t)t.length_lba * SECTOR_BYTES;

    // Pressing offset: disc-specific additional sample shift on top of drive offset.
    // Detected from track 1's frame450_crc after Pass 1 fails.
    // Once detected, used for all subsequent tracks' Pass 1 and Pass 2.
    int  pressing_offset          = 0;
    bool pressing_offset_detected = false;
    std::vector<ARTrackResult> ar_results(total);
    std::vector<RGResult>      rg_results(total);
    // Integrated-loudness state for each KEPT track's audio, handed back from
    // ripTrack. Combined after the loop for true album gain, then destroyed.
    std::vector<ebur128_state*> album_states(total, nullptr);
    bool any_error = false;

    // CTDB CRC state threads across all tracks (initialized with seed 0xFFFFFFFF)
    ARTrackResult ctdb_state;
    ctdb_state.ctdb_crc   = 0xFFFFFFFFu;  // CRC32 seed
    ctdb_state.ctdb_bytes = 0;

    for (int i = 0; i < total; ++i) {
        if (cancel_.load()) break;

        const CDTrack& trk = tracks[i];
        int tnum = trk.number;
        const MBTrack* mt = nullptr;
        for (const auto& m : rel.tracks) if (m.number==tnum && m.disc==current_disc) { mt=&m; break; }

        std::ostringstream nn;
        nn << std::setw(2) << std::setfill('0') << tnum;
        std::string prefix = nn.str();
        if (mt && !mt->title.empty()) prefix += " - " + sanitizePath(mt->title);

        const std::vector<RipOutput> outs = buildOuts(opt, out_dir, prefix);

        // For track 1: only drive_offset (pressing not yet detected).
        // For all subsequent tracks: drive_offset + pressing_offset (once detected).
        const int eff_pressing = pressing_offset_detected && i > 0 ? pressing_offset : 0;

        RGResult rg;
        ebur128_state* ebur_kept = nullptr;   // Pass 1 loudness state (kept unless Pass 2 wins)
        ARTrackResult ar = ripTrack(*dev, trk, i, total,
                                    i==0, i==total-1,
                                    use_c2,
                                    outs, opt, rg, cb,
                                    log_path, mode, drive_offset,
                                    ctdb_state.ctdb_crc,
                                    ctdb_state.ctdb_bytes,
                                    eff_pressing,
                                    ctdb_total_bytes,
                                    &ebur_kept);

        // Carry CTDB state forward to next track
        ctdb_state.ctdb_crc   = ar.ctdb_crc;
        ctdb_state.ctdb_bytes = ar.ctdb_bytes;

        // Check for rip failure (empty CRC and not-queried = encoder init failed)
        if (!isLocal(mode) && ar.crc_v1 == 0 && ar.crc_v2 == 0 && !cancel_.load()) {
            any_error = true;
            if (cb) {
                RipProgress p; p.state=RipState::Error;
                p.status_msg = "Read error on track "+std::to_string(tnum)
                             + " -- disc may be scratched. Other tracks saved.";
                cb(p);
            }
            break;
        }

        // ── AccurateRip verification ───────────────────────────────────────
        // AR binary format per track: [conf][v1_or_v2_crc][v2_crc]
        // out_v1 = bytes 1-4 of each chunk track entry (v1 or v2 depending on era)
        // out_v2 = bytes 5-8 of each chunk track entry (v2 CRC, 0 in older submissions)
        // We compare our crc_v1 and crc_v2 against both out_v1 and out_v2 entries.
        auto checkAR = [&](const ARTrackResult& candidate) -> ARTrackResult {
            ARTrackResult result = candidate;
            // A preamble read failure already marked this indeterminate — don't
            // overwrite it with a (meaningless) match/no-match verdict.
            if (candidate.status == ARStatus::ReadError) return result;
            result.status = ARStatus::NotFound;
            if (!ar_db_loaded) { result.status = ARStatus::NetworkError; return result; }
            if (i >= (int)ar_db_v1.size()) return result;
            // Check all DB v1 field entries first (may contain v1 or v2 CRCs)
            for (const auto& [crc, conf] : ar_db_v1[i]) {
                if (crc == candidate.crc_v2) {
                    result.status = ARStatus::Matched_v2; result.confidence = conf; return result;
                }
            }
            for (const auto& [crc, conf] : ar_db_v1[i]) {
                if (crc == candidate.crc_v1) {
                    result.status = ARStatus::Matched_v1; result.confidence = conf; return result;
                }
            }
            // Check DB v2 field entries (v2 CRCs from newer submissions)
            if (i < (int)ar_db_v2.size()) {
                for (const auto& [crc, conf] : ar_db_v2[i]) {
                    if (crc == candidate.crc_v2) {
                        result.status = ARStatus::Matched_v2; result.confidence = conf; return result;
                    }
                }
                for (const auto& [crc, conf] : ar_db_v2[i]) {
                    if (crc == candidate.crc_v1) {
                        result.status = ARStatus::Matched_v1; result.confidence = conf; return result;
                    }
                }
            }
            return result;
        };

        ar = checkAR(ar);
        int pass_count = 1;

        // ── frame450 drive-offset self-check (track 1 only) ──────────────────
        // frame450_local was computed inside ripTrack and logged.
        // We check it here via ar.frame450_local (added to ARTrackResult).
        // This confirms the drive offset is correct, or warns if unknown.
        if (i == 0 && !isLocal(mode) && ar_db_loaded
            && !ar_db_v2[0].empty() && ar.frame450_local != 0) {
            bool frame450_matched = false;
            for (const auto& [crc, conf] : ar_db_v2[0])
                if (crc == ar.frame450_local) { frame450_matched = true; break; }
            FILE* lf = port::fopenUtf8(log_path, "a");
            if (lf) {
                if (frame450_matched)
                    fprintf(lf, "  frame450: drive offset %+d confirmed\n", drive_offset);
                else
                    fprintf(lf, "  frame450: WARNING — offset %+d not confirmed."
                            " Check accuraterip.com/driveoffsets.htm\n", drive_offset);
                fclose(lf);
            }
        }

        // CRC diagnostic — append to per-rip log
        {
            FILE* lf = port::fopenUtf8(log_path, "a");
            if (lf) {
                fprintf(lf, "\nTrack %d Pass 1: crc_v1(=csum_lo)=%08x  crc_v2(=csum_lo+hi)=%08x  status=%d (%s)\n",
                    tnum, ar.crc_v1, ar.crc_v2, (int)ar.status,
                    ar.status==ARStatus::Matched_v2 ? "AR v2 OK" :
                    ar.status==ARStatus::Matched_v1 ? "AR v1 OK" :
                    ar.status==ARStatus::ReadError  ? "preamble read error (inconclusive)" : "no match");
                if (i < (int)ar_db_v1.size()) {
                    fprintf(lf, "  DB main_crcs (%zu):\n", ar_db_v1[i].size());
                    for (auto& [crc,conf] : ar_db_v1[i])
                        fprintf(lf, "    crc=%08x conf=%d%s%s\n", crc, conf,
                            crc==ar.crc_v1 ? "  <-- MATCH v1" : "",
                            crc==ar.crc_v2 ? "  <-- MATCH v2" : "");
                }
                if (i < (int)ar_db_v2.size() && !ar_db_v2[i].empty()) {
                    fprintf(lf, "  DB frame450_crcs (%zu):\n", ar_db_v2[i].size());
                    for (auto& [crc,conf] : ar_db_v2[i])
                        fprintf(lf, "    crc=%08x conf=%d\n", crc, conf);
                }
                fclose(lf);
            }
        }

        // ── Re-rip if Pass 1 failed AR and the database is loaded ─────────
        // Mirrors dBpoweramp behaviour: "Pass 1 & 2, Re-Rip N Frames"
        // IMPORTANT: Pass 2 writes to temp paths so Pass 1 audio is never
        // truncated before we know whether Pass 2 actually verified.
        if (ar.status == ARStatus::NotFound && ar_db_loaded) {

            // ── Pressing offset detection (track 1 only, once) ─────────────
            // PRIMARY: frame450 OffsetFindCRC sweep (bytes 5-8 of each AR record
            // ARE the offset-finding CRC, per Spoon's spec — one short read finds
            // the offset directly). FALLBACK: if the sweep finds nothing, probe a
            // short list of known common pressing offsets with full-track rips.
            // BASELINE underneath both: drive_offset from drive_offsets.h.
            if (i == 0 && !pressing_offset_detected) {
                pressing_offset_detected = true;

                // (1) PRIMARY — frame450 sweep.
                int  fr450_pressing = 0;
                bool fr450_found    = false;
                if (!ar_db_v2.empty())
                    fr450_found = detectPressingOffsetFrame450(
                        *dev, trk, drive_offset, use_c2,
                        ar_db_v2[0], log_path, fr450_pressing);

                if (fr450_found) {
                    pressing_offset = fr450_pressing;
                } else {
                    // (2) FALLBACK — candidate-offset full-track probe.
                    static constexpr int CANDIDATE_OFFSETS[] = {
                        664, 654, 667, 594, 586, 30, -30, 618, 706, 48
                    };
                    FILE* lf = port::fopenUtf8(log_path, "a");
                    if (lf) {
                        fprintf(lf, "  Scanning pressing offsets:");
                        for (int p : CANDIDATE_OFFSETS) fprintf(lf, " %+d", p);
                        fprintf(lf, "\n");
                        fclose(lf);
                    }
                    for (int p : CANDIDATE_OFFSETS) {
                        if (cancel_.load()) break;
                        if (drive_offset + p < 0) continue;
                        const auto probe_outs = withSuffix(outs, ".probe");
                        RGResult      rg_probe;
                        ARTrackResult ar_probe = ripTrack(*dev, trk, i, total,
                                                          true, total == 1,
                                                          use_c2,
                                                          probe_outs, opt,
                                                          rg_probe, nullptr,
                                                          log_path, mode,
                                                          drive_offset,
                                                          ctdb_state.ctdb_crc,
                                                          ctdb_state.ctdb_bytes,
                                                          p);
                        std::error_code ec_probe;
                        for (const auto& o : probe_outs) fs::remove(o.path, ec_probe);
                        ar_probe = checkAR(ar_probe);
                        if (ar_probe.status == ARStatus::Matched_v1 ||
                            ar_probe.status == ARStatus::Matched_v2) {
                            pressing_offset = p;
                            lf = port::fopenUtf8(log_path, "a");
                            if (lf) {
                                fprintf(lf,
                                    "  Pressing offset detected: +%d (total_skip=%d conf=%d)\n",
                                    p, drive_offset + p, ar_probe.confidence);
                                fclose(lf);
                            }
                            break;
                        }
                    }
                    if (pressing_offset == 0) {
                        lf = port::fopenUtf8(log_path, "a");
                        if (lf) {
                            fprintf(lf, "  Pressing offset: none matched — using drive offset only\n");
                            fclose(lf);
                        }
                    }
                }
            }

            if (cb) {
                RipProgress p; p.state=RipState::Ripping;
                p.track=i+1; p.total=total; p.pct=0;
                p.status_msg = "Track "+std::to_string(tnum)
                             +" AR mismatch on Pass 1 -- re-ripping (Pass 2)...";
                cb(p);
            }

            // ── Cache flush + speed reduction before Pass 2 ───────────────
            // Modern drives serve re-reads from an internal RAM cache (512KB–2MB),
            // so without intervention Pass 2 reads the same broken data as Pass 1.
            //
            // Step 1: Drop to 10x (1764 KB/s). This forces the drive controller
            //   to recalibrate its laser tracking and spindle PLL, which has the
            //   side-effect of invalidating its sector cache.
            setDriveSpeed(*dev, 1764);
            //
            // Step 2: Evict the read-ahead cache with a far multi-MB read so
            //   Pass 2 reads the platter, not RAM. A single-sector read can't
            //   flush a 512KB-2MB cache; flushDriveCache reads >4MB from a
            //   region far from the target. full_leadout_lba is in worker scope.
            flushDriveCache(*dev, trk.start_lba, full_leadout_lba, use_c2);
            //
            // Step 3: Give the spindle 500 ms to fully settle at the new speed
            //   before we start reading again.
            port::sleepMs(500);

            const auto tmp_outs = withSuffix(outs, ".tmp");

            RGResult      rg2;
            ebur128_state* ebur_p2 = nullptr;
            ARTrackResult ar2 = ripTrack(*dev, trk, i, total,
                                         i==0, i==total-1,
                                         use_c2,
                                         tmp_outs, opt, rg2, cb,
                                         log_path, mode, drive_offset,
                                         ctdb_state.ctdb_crc,
                                         ctdb_state.ctdb_bytes,
                                         pressing_offset,
                                         ctdb_total_bytes,
                                         &ebur_p2);
            if (ar2.crc_v1 != 0 || ar2.crc_v2 != 0) {
                ar2 = checkAR(ar2);
                pass_count = 2;
                {
                    FILE* lf = port::fopenUtf8(log_path, "a");
                    if (lf) {
                        fprintf(lf, "Track %d Pass 2: crc_v1=%08x crc_v2=%08x status=%d (%s)\n",
                            tnum, ar2.crc_v1, ar2.crc_v2, (int)ar2.status,
                            ar2.status==ARStatus::Matched_v2 ? "AR v2 OK" :
                            ar2.status==ARStatus::Matched_v1 ? "AR v1 OK" :
                            ar2.crc_v1==ar.crc_v1 && ar2.crc_v2==ar.crc_v2
                                ? "same as pass1 (disc deterministic)" : "different from pass1 (read error)");
                        fclose(lf);
                    }
                }
                if (ar2.status == ARStatus::Matched_v1 ||
                    ar2.status == ARStatus::Matched_v2) {
                    // Pass 2 verified — atomically replace Pass 1 files
                    std::error_code ec2;
                    for (size_t k = 0; k < outs.size(); ++k) {
                        fs::remove(outs[k].path, ec2);
                        fs::rename(tmp_outs[k].path, outs[k].path, ec2);
                    }
                    ar = ar2;
                    rg = rg2;
                    // Pass 2 audio is now the kept audio: its loudness state is
                    // the one album gain must use. Drop Pass 1's.
                    if (ebur_kept) ebur128_destroy(&ebur_kept);
                    ebur_kept = ebur_p2;
                    ebur_p2   = nullptr;
                } else {
                    // Pass 2 also failed — discard its audio, keep Pass 1
                    std::error_code ec2;
                    for (const auto& o : tmp_outs) fs::remove(o.path, ec2);
                }
            } else {
                // Pass 2 hard read failure — clean up and keep Pass 1
                std::error_code ec2;
                for (const auto& o : tmp_outs) fs::remove(o.path, ec2);
            }
            // Pass 2's loudness state is discarded unless it won (handled above).
            if (ebur_p2) ebur128_destroy(&ebur_p2);
            // Restore max speed for remaining tracks regardless of Pass 2 outcome
            setDriveSpeed(*dev, 0xFFFF);
        }
        // ── Determinism check: [B] Local 2-pass (no AR oracle) ──────────────
        // No database to verify against, so the only safety net is reading the
        // track twice with a real cache flush in between and confirming the two
        // reads are identical. A caching drive would otherwise return the same
        // bytes from RAM and make a damaged rip look clean. Doubles rip time --
        // which is why it's a separate mode from fast best-effort [Y].
        else if (mode == RipMode::LocalVerify) {
            flushDriveCache(*dev, trk.start_lba, full_leadout_lba, use_c2);
            const auto det_outs = withSuffix(outs, ".det");
            RGResult      rgd;
            ARTrackResult ard = ripTrack(*dev, trk, i, total, i==0, i==total-1,
                                         use_c2, det_outs, opt, rgd, cb,
                                         log_path, mode, drive_offset,
                                         ctdb_state.ctdb_crc, ctdb_state.ctdb_bytes,
                                         eff_pressing);
            bool deterministic = (ard.crc_v1 == ar.crc_v1 && ard.crc_v2 == ar.crc_v2);
            FILE* lf = port::fopenUtf8(log_path, "a");
            if (lf) {
                fprintf(lf, "Track %d determinism: %s "
                            "(pass1 v1=%08x v2=%08x  pass2 v1=%08x v2=%08x)\n",
                        tnum, deterministic ? "OK (passes identical)"
                                            : "FAIL (passes differ -- bad rip)",
                        ar.crc_v1, ar.crc_v2, ard.crc_v1, ard.crc_v2);
                fclose(lf);
            }
            std::error_code ec;
            for (const auto& o : det_outs) fs::remove(o.path, ec);
        }

        ar_results[i]    = ar;
        rg_results[i]    = rg;
        album_states[i]  = ebur_kept;   // null if this track failed; owned until album calc

        // Report AR result with Pass label matching dBpoweramp style
        if (cb) {
            RipProgress p; p.state=RipState::Ripping;
            p.track=i+1; p.total=total; p.pct=100;
            std::string pass_tag = (pass_count == 1) ? " [Pass 1]" : " [Pass 1 & 2]";
            switch(ar.status) {
            case ARStatus::Matched_v2:
                p.status_msg = "Track "+std::to_string(tnum)
                             +" Accurate (conf "+std::to_string(ar.confidence)+")"
                             +pass_tag+" [AR v2]"; break;
            case ARStatus::Matched_v1:
                p.status_msg = "Track "+std::to_string(tnum)
                             +" Accurate (conf "+std::to_string(ar.confidence)+")"
                             +pass_tag+" [AR v1]"; break;
            case ARStatus::NotFound:
                p.status_msg = "Track "+std::to_string(tnum)
                             +(pass_count>1
                                ? " not in AccurateRip database (both passes)"
                                : " not in AccurateRip database"); break;
            case ARStatus::ReadError:
                p.status_msg = "Track "+std::to_string(tnum)
                             +" ripped & kept -- AR inconclusive (preamble read error, see log)"; break;
            default:
                p.status_msg = "Track "+std::to_string(tnum)+" ripped (AR skipped)"; break;
            }
            cb(p);
            port::sleepMs(300); // brief pause so user can read each track result
        }
    }

    // ── CTDB finalization (CUETools mode) ────────────────────────────────
    if (mode == RipMode::CUETools && !cancel_.load() && !any_error) {
        // The running CRC32 already excludes BOTH the first and last 10 sectors:
        // start-trim is gated inline, and the disc total (ctdb_total_bytes) was
        // passed into ripTrack so the end-trim boundary could be applied as bytes
        // were fed (CRC32 can't be un-fed after the fact). Finalize = XOR ones.
        uint32_t ctdb_final = ctdb_state.ctdb_crc ^ 0xFFFFFFFFu;
        char ctdb_id[9]; snprintf(ctdb_id, sizeof(ctdb_id), "%08x", ctdb_final);

        {
            FILE* lf = port::fopenUtf8(log_path, "a");
            if (lf) {
                fprintf(lf, "\n=== CUETools DB ===\n");
                fprintf(lf, "Disc audio bytes processed : %zu\n", ctdb_state.ctdb_bytes);
                fprintf(lf, "CTDB trim (start+end)      : %zu bytes each (10 sectors)\n", (size_t)(10*SECTOR_BYTES));
                fprintf(lf, "CTDB ID (start+end trimmed): %s\n", ctdb_id);
                fclose(lf);
            }
        }

        if (cb) {
            RipProgress p; p.state = RipState::Ripping;
            p.status_msg = "Querying CUETools database (CTDB ID: " + std::string(ctdb_id) + ")...";
            cb(p);
        }

        std::string ctdb_status;
        fetchCTDBData(ctdb_id, log_path, total, ctdb_status);

        if (cb) {
            RipProgress p; p.state = RipState::Ripping;
            p.status_msg = "CUETools: " + ctdb_status;
            cb(p); port::sleepMs(800);
        }
    }

    // ── Album ReplayGain ──────────────────────────────────────────────────
    // Album peak = loudest track true-peak (correct as-is).
    double album_peak = 0;
    for (auto& rg : rg_results) album_peak = std::max(album_peak, rg.track_peak);

    // Album gain — true EBU R128 integrated loudness over EVERY kept track's
    // gating blocks at once (ebur128_loudness_global_multiple), NOT a mean of
    // per-track gains. Each kept track handed its loudness state back from
    // ripTrack; combine the live ones here.
    std::vector<ebur128_state*> live_states;
    for (auto* st : album_states) if (st) live_states.push_back(st);

    double album_gain = 0.0;
    bool   album_gain_ok = false;
    if (!live_states.empty()) {
        double album_loudness = 0.0;
        if (ebur128_loudness_global_multiple(live_states.data(),
                                             live_states.size(),
                                             &album_loudness) == EBUR128_SUCCESS
            && album_loudness > -100.0) {   // guard the silence/no-block sentinel
            album_gain    = -18.0 - album_loudness;
            album_gain_ok = true;
        }
    }
    // Fallback only if the multiple-state measurement was unavailable (e.g. every
    // track failed to produce a state): mean of valid track gains beats nothing.
    if (!album_gain_ok) {
        double sum_gain = 0; int valid = 0;
        for (auto& rg : rg_results) if (rg.valid) { sum_gain += rg.track_gain; ++valid; }
        album_gain = valid > 0 ? sum_gain / valid : 0.0;
    }

    // States are no longer needed once the album figure is computed.
    for (auto*& st : album_states) if (st) { ebur128_destroy(&st); st = nullptr; }

    for (auto& rg : rg_results) {
        rg.album_gain = album_gain;
        rg.album_peak = album_peak;
    }

    // ── Tag all files ─────────────────────────────────────────────────────
    for (int i = 0; i < total && !cancel_.load(); ++i) {
        const CDTrack& trk = tracks[i];
        int tnum = trk.number;
        const MBTrack* mt = nullptr;
        for (const auto& m : rel.tracks) if (m.number==tnum && m.disc==current_disc) { mt=&m; break; }

        std::ostringstream nn;
        nn << std::setw(2) << std::setfill('0') << tnum;
        std::string prefix = nn.str();
        if (mt && !mt->title.empty()) prefix += " - " + sanitizePath(mt->title);

        for (const auto& o : buildOuts(opt, out_dir, prefix)) {
            // Untaggable formats (WAV: no tags/art/RG by format) are skipped
            // here — tagFile itself and the taggable formats' calls are
            // unchanged (the guard is false for FLAC/MP3).
            if (const RipFormatRow* r = ripFormatRow(o.fmt); r && !r->taggable)
                continue;
            tagFile(o.path, rel, mt, tnum, art, ar_results[i], rg_results[i], mode);
        }
    }

    // CUE/M3U8 entries must reference files that exist: point them at the
    // MASTER — the first selected lossless format, else the first selected
    // (lossy-only rips get a playlist over the lossy files). Default
    // selection resolves to ".flac", byte-identical to before.
    const RipFormatRow* master_row = nullptr;
    for (RipFormat f : opt.formats)
        if (const RipFormatRow* r = ripFormatRow(f); r && r->lossless) { master_row = r; break; }
    if (!master_row && !opt.formats.empty()) master_row = ripFormatRow(opt.formats[0]);
    const char* list_ext = master_row ? master_row->ext : ".flac";

    // ── Write CUE sheet ───────────────────────────────────────────────────
    // Standard Red Book CUE sheet for FLAC rip — compatible with foobar2000,
    // EAC, whipper and any player that supports gapless via cue index points.
    if (!cancel_.load()) {
        std::string cue_path = out_dir + kSep + sanitizePath(
            rel.artist.empty() ? rel.title
            : rel.artist + " - " + rel.title) + ".cue";
        // Open binary: content is already UTF-8 and the lines carry explicit
        // \r\n. ccs=UTF-8 would make the stream wide-oriented (narrow fprintf
        // then writes nothing but the auto-BOM); plain "w" would double the CR.
        FILE* cf = port::fopenUtf8(cue_path, "wb");
        if (cf) {
            // Header
            if (!rel.artist.empty()) fprintf(cf, "PERFORMER \"%s\"\r\n", rel.artist.c_str());
            if (!rel.title.empty())  fprintf(cf, "TITLE \"%s\"\r\n",     rel.title.c_str());
            if (rel.date.size() >= 4) fprintf(cf, "REM DATE %s\r\n", rel.date.substr(0,4).c_str());
            fprintf(cf, "REM DISCID %08x\r\n", computeCDDB(tracks, full_leadout_lba, data_track_lbas));
            fprintf(cf, "REM COMMENT \"RE-MOCT v" REMOCT_VERSION "\"\r\n");

            for (int i = 0; i < total; ++i) {
                const CDTrack& trk = tracks[i];
                int tnum = trk.number;
                const MBTrack* mt = nullptr;
                for (const auto& m : rel.tracks) if (m.number==tnum && m.disc==current_disc) { mt=&m; break; }

                std::ostringstream nn;
                nn << std::setw(2) << std::setfill('0') << tnum;
                std::string prefix2 = nn.str();
                if (mt && !mt->title.empty()) prefix2 += " - " + sanitizePath(mt->title);

                // FILE per track (non-embedded CUE) — the master format (§ above)
                fprintf(cf, "FILE \"%s%s\" WAVE\r\n", prefix2.c_str(), list_ext);
                fprintf(cf, "  TRACK %02d AUDIO\r\n", tnum);
                if (mt && !mt->title.empty())
                    fprintf(cf, "    TITLE \"%s\"\r\n", mt->title.c_str());
                std::string trk_artist = (mt && !mt->artist.empty()) ? mt->artist : rel.artist;
                if (!trk_artist.empty())
                    fprintf(cf, "    PERFORMER \"%s\"\r\n", trk_artist.c_str());
                // AR status as REM
                if (ar_results[i].status == ARStatus::Matched_v2 ||
                    ar_results[i].status == ARStatus::Matched_v1)
                    fprintf(cf, "    REM ACCURATERIP [%08x] CONFIDENCE %d\r\n",
                            ar_results[i].crc_v2, ar_results[i].confidence);
                fprintf(cf, "    INDEX 01 00:00:00\r\n");
            }
            fclose(cf);
        }
    }

    // ── Write M3U playlist ────────────────────────────────────────────────
    if (!cancel_.load()) {
        std::string m3u_path = out_dir + kSep + sanitizePath(
            rel.artist.empty() ? rel.title
            : rel.artist + " - " + rel.title) + ".m3u8";
        // Open binary — same reasoning as the CUE writer above (UTF-8 bytes +
        // explicit \r\n; avoid ccs=UTF-8's BOM-only output and "w"'s doubled CR).
        FILE* mf = port::fopenUtf8(m3u_path, "wb");
        if (mf) {
            fprintf(mf, "#EXTM3U\r\n");
            for (int i = 0; i < total; ++i) {
                const CDTrack& trk = tracks[i];
                int tnum = trk.number;
                const MBTrack* mt = nullptr;
                for (const auto& m : rel.tracks) if (m.number==tnum && m.disc==current_disc) { mt=&m; break; }

                std::ostringstream nn;
                nn << std::setw(2) << std::setfill('0') << tnum;
                std::string prefix2 = nn.str();
                if (mt && !mt->title.empty()) prefix2 += " - " + sanitizePath(mt->title);

                int duration_sec = (int)trk.length_lba / 75;
                std::string display = rel.artist.empty()
                    ? (mt ? mt->title : prefix2)
                    : rel.artist + " - " + (mt ? mt->title : prefix2);
                fprintf(mf, "#EXTINF:%d,%s\r\n", duration_sec, display.c_str());
                fprintf(mf, "%s%s\r\n", prefix2.c_str(), list_ext);
            }
            fclose(mf);
        }
    }

    // ── Write TOC file ────────────────────────────────────────────────────
    // Raw disc geometry — useful for re-verification and debugging.
    if (!cancel_.load()) {
        std::string toc_path = out_dir + kSep + "disc.toc";
        FILE* tf = port::fopenUtf8(toc_path, "w");
        if (tf) {
            fprintf(tf, "# RE-MOCT disc TOC\r\n");
            fprintf(tf, "# Drive     : %s:\r\n", drive_letter.c_str());
            fprintf(tf, "# Drive off : %+d samples\r\n", drive_offset);
            fprintf(tf, "# CDDB ID   : %08x\r\n", computeCDDB(tracks, full_leadout_lba, data_track_lbas));
            if (!rel.mb_id.empty())
                fprintf(tf, "# MB ID     : %s\r\n", rel.mb_id.c_str());
            fprintf(tf, "#\r\n");
            fprintf(tf, "# Track  Start-LBA  Length-LBA  Start-MSF    Duration\r\n");
            for (int i = 0; i < total; ++i) {
                const CDTrack& t = tracks[i];
                uint32_t m = t.start_lba / 75 / 60;
                uint32_t s = (t.start_lba / 75) % 60;
                uint32_t f = t.start_lba % 75;
                int dur_s = t.length_lba / 75;
                fprintf(tf, "  %02d     %-10lu %-11lu %02u:%02u:%02u     %d:%02d\r\n",
                    t.number, (unsigned long)t.start_lba, (unsigned long)t.length_lba,
                    m, s, f, dur_s/60, dur_s%60);
            }
            // Lead-out
            uint32_t lo = full_leadout_lba ? (uint32_t)full_leadout_lba
                         : tracks.back().start_lba + tracks.back().length_lba;
            fprintf(tf, "  AA     %-10lu (lead-out)\r\n", (unsigned long)lo);
            fclose(tf);
        }

        // ── Machine-readable sidecar for tooling / regression diffs ──────────
        {
            using nlohmann::json;
            auto hx = [](uint32_t v){ char b[9]; snprintf(b,sizeof b,"%08x",v); return std::string(b); };

            // Recompute AR disc IDs here (same fixed-150 formula as fetchARData)
            // so the sidecar is self-contained. Non-standard pregap survives.
            const uint32_t AR_PREGAP = 150;
            uint32_t lo_j = full_leadout_lba ? (uint32_t)full_leadout_lba
                          : tracks.back().start_lba + tracks.back().length_lba;
            uint32_t id1 = 0, id2 = 0, rel_lo = lo_j - AR_PREGAP;
            for (int i = 0; i < total; ++i) {
                uint32_t rel = tracks[i].start_lba - AR_PREGAP;
                id1 += rel;
                id2 += std::max(rel, 1u) * (uint32_t)(i + 1);
            }
            id1 += rel_lo;
            id2 += rel_lo * (uint32_t)(total + 1);

            json j;
            j["schema_version"] = 1;
            j["disc"] = {
                {"artist", rel.artist}, {"album", rel.title},
                {"date", rel.date},     {"mb_id", rel.mb_id}
            };
            j["drive"] = {
                {"letter", drive_letter}, {"model", drive_model},
                {"read_offset_samples", drive_offset}
            };
            j["ids"] = {
                {"cddb", hx(computeCDDB(tracks, full_leadout_lba, data_track_lbas))},
                {"ar_disc_id1", hx(id1)}, {"ar_disc_id2", hx(id2)}
            };

            json toc;
            toc["pregap_frames"] = (int)tracks[0].start_lba - (int)AR_PREGAP; // 0 if standard
            toc["leadout_lba"]   = (uint32_t)lo_j;
            toc["data_tracks"]   = data_track_lbas;
            for (int i = 0; i < total; ++i) {
                const CDTrack& t = tracks[i];
                char msf[16];
                snprintf(msf, sizeof msf, "%02u:%02u:%02u",
                         (unsigned)(t.start_lba/75/60), (unsigned)((t.start_lba/75)%60),
                         (unsigned)(t.start_lba%75));
                toc["tracks"].push_back({
                    {"number", t.number}, {"start_lba", t.start_lba},
                    {"length_lba", t.length_lba}, {"start_msf", msf},
                    {"duration_sec", (int)(t.length_lba/75)}
                });
            }
            j["toc"] = toc;

            auto arStr = [](ARStatus s) -> const char* {
                switch (s) {
                    case ARStatus::Matched_v2:   return "matched_v2";
                    case ARStatus::Matched_v1:   return "matched_v1";
                    case ARStatus::NotFound:     return "not_found";
                    case ARStatus::NetworkError: return "network_error";
                    case ARStatus::ReadError:    return "read_error";
                    default:                     return "not_queried";
                }
            };
            json ar;
            ar["pressing_offset"] = pressing_offset;
            for (int i = 0; i < (int)ar_results.size(); ++i) {
                const auto& r = ar_results[i];
                ar["tracks"].push_back({
                    {"n", i + 1}, {"status", arStr(r.status)}, {"confidence", r.confidence},
                    {"crc_v1", hx(r.crc_v1)}, {"crc_v2", hx(r.crc_v2)},
                    {"frame450_local", hx(r.frame450_local)}
                });
            }
            j["accuraterip"] = ar;

            json rgj;
            rgj["album_gain"] = rg_results.empty() ? 0.0 : rg_results[0].album_gain;
            rgj["album_peak"] = rg_results.empty() ? 0.0 : rg_results[0].album_peak;
            for (int i = 0; i < (int)rg_results.size(); ++i)
                rgj["tracks"].push_back({
                    {"n", i + 1}, {"gain", rg_results[i].track_gain},
                    {"peak", rg_results[i].track_peak}
                });
            j["replaygain"] = rgj;

            std::string json_path = out_dir + kSep + "disc.json";
            FILE* jf = port::fopenUtf8(json_path, "w");
            if (jf) { std::string s = j.dump(2); fwrite(s.data(), 1, s.size(), jf); fclose(jf); }
        }
    }

    dev.reset();   // close the device here — the baseline CloseHandle position
                   // (before the summary/log tail), not at worker scope exit

    // ── Write rip summary to log ──────────────────────────────────────────
    {
        FILE* lf = port::fopenUtf8(log_path, "a");
        if (lf) {
            int ar_v2=0, ar_v1=0, ar_none=0;
            for (auto& r : ar_results) {
                if (r.status==ARStatus::Matched_v2) ++ar_v2;
                else if (r.status==ARStatus::Matched_v1) ++ar_v1;
                else ++ar_none;
            }
            fprintf(lf, "\n=== Summary ===\n");
            fprintf(lf, "AR: %d v2 + %d v1 matched, %d not found / %d total\n",
                    ar_v2, ar_v1, ar_none, (int)ar_results.size());
            fprintf(lf, "ReplayGain: album gain=%.2f dB peak=%.6f\n",
                    rg_results.empty() ? 0.0 : rg_results[0].album_gain,
                    rg_results.empty() ? 0.0 : rg_results[0].album_peak);
            for (int i = 0; i < (int)ar_results.size(); ++i) {
                const char* status =
                    ar_results[i].status==ARStatus::Matched_v2 ? "[AR v2 OK]" :
                    ar_results[i].status==ARStatus::Matched_v1 ? "[AR v1 OK]" :
                    ar_results[i].status==ARStatus::NetworkError ? "[AR net err]" :
                    ar_results[i].status==ARStatus::ReadError ? "[AR inconclusive: read err]" :
                    "[AR not found]";
                fprintf(lf, "  Track %02d: %s conf=%d  rg=%.2fdB [%08x] [%08x]\n",
                    i+1, status, ar_results[i].confidence,
                    i < (int)rg_results.size() ? rg_results[i].track_gain : 0.0,
                    ar_results[i].crc_v1, ar_results[i].crc_v2);
            }
            fclose(lf);
        }
    }

    if (cancel_.load()) {
        state_.store(RipState::Cancelled);
        if (cb) { RipProgress p; p.state=RipState::Cancelled;
                  p.status_msg="Rip cancelled."; cb(p); }
    } else if (any_error) {
        state_.store(RipState::Error);
        if (cb) { RipProgress p; p.state=RipState::Error;
                  p.status_msg="Rip error -- check disc."; cb(p); }
    } else {
        // Build summary
        int ar_v2=0, ar_v1=0, ar_none=0;
        for (auto& r : ar_results) {
            if (r.status==ARStatus::Matched_v2) ++ar_v2;
            else if (r.status==ARStatus::Matched_v1) ++ar_v1;
            else ++ar_none;
        }
        state_.store(RipState::Done);
        if (cb) {
            RipProgress p; p.state=RipState::Done; p.track=total; p.total=total; p.pct=100;
            std::ostringstream ss;
            ss << "Rip complete!  AR: " << ar_v2 << "v2 + " << ar_v1 << "v1 / "
               << total << " tracks  -> " << out_dir;
            p.status_msg = ss.str();
            cb(p);
        }
    }
    active_.store(false);
}

// ─── Public API ──────────────────────────────────────────────────────────────
bool CDRipper::start(AudioManager&               audio,
                     const std::vector<CDTrack>&  tracks,
                     const std::string&           out_dir,
                     const MBRelease&             rel,
                     RipMode                      mode,
                     RipOptions                   opt,
                     ProgressCb                   cb) {
    if (active_.load()) return false;
    active_.store(true);
    cancel_.store(false);
    state_.store(RipState::Idle);
    if (thread_.joinable()) thread_.join();
    audio.cdSource().stopReader();
    std::string        dl            = audio.cdSource().driveLetter();
    int                drv_offset    = audio.cdSource().driveOffset();
    std::string        drv_model     = audio.cdSource().driveModel();
    uint32_t              full_leadout  = audio.cdSource().fullLeadoutLba();
    std::vector<uint32_t> data_trk_lbas = audio.cdSource().dataTrackLbas();
    auto dev = openDrive(dl);   // may be null — worker reports the error (as baseline)
    thread_ = std::thread(&CDRipper::worker, this,
                          dl, tracks, out_dir, rel, mode, std::move(opt), cb,
                          std::move(dev), drv_offset, drv_model,
                          full_leadout, data_trk_lbas);
    return true;
}

void CDRipper::cancel() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
    active_.store(false);
    state_.store(RipState::Idle);
}

