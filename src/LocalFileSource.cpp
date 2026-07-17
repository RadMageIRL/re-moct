// LocalFileSource.cpp — Phase 2 slice B. The three file-path helpers below
// (populate_track_info / open_decoder / prime_decoder) are the AudioManager.cpp
// baselines MOVED VERBATIM (file-static, original indentation — the slice-7
// auditable-move discipline); the class methods are thin adapters over them plus
// the seek-prime and cursor logic moved from AudioManager::seekTo/positionSec.
#include "LocalFileSource.h"
#include "StringUtils.h"
#include "CustomBackends.h" // the shared AAC/Opus/WavPack backend array
#include "R128Gain.h"       // dbFromR128 — the one home for the R128<->RG dialect
#include "Mp4Chapters.h"    // mp4AacChannelCount: true ASC channel count

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

// ─── helpers (moved verbatim from AudioManager.cpp) ──────────────────────────
static void populate_track_info(TrackInfo& info, const std::string& path) {
    info = {};
    info.path  = path;
    info.title = fs::path(path).stem().string();
#ifdef _WIN32
    auto wpath = utf8_to_wide(path);
    TagLib::FileRef ref(wpath.c_str(), true, TagLib::AudioProperties::Fast);
#else
    TagLib::FileRef ref(path.c_str(), true, TagLib::AudioProperties::Fast);
#endif
    if (ref.isNull()) return;

    // Helper: TagLib string -> UTF-8. Use to8Bit(true) which is correct for UTF-8 tags.
    // For Latin-1 encoded ID3v2.3 tags, strip non-ASCII garbage gracefully.
    if (auto* tag = ref.tag(); tag) {
        std::string title  = sanitizeForDisplay(tag->title().to8Bit(true));
        std::string artist = sanitizeForDisplay(tag->artist().to8Bit(true));
        if (!title.empty())  info.title  = title;
        if (!artist.empty()) info.artist = artist;
        if (!tag->album().isEmpty())   info.album   = sanitizeForDisplay(tag->album().to8Bit(true));
        if (!tag->genre().isEmpty())   info.genre   = sanitizeForDisplay(tag->genre().to8Bit(true));
        if (!tag->comment().isEmpty()) {
            std::string c = sanitizeForDisplay(tag->comment().to8Bit(true));
            if (c.size() > 80) c = c.substr(0, 77) + "...";
            info.comment = c;
        }
        info.year      = (int)tag->year();
        info.track_num = (int)tag->track();

        // Read ReplayGain track gain tag (try common tag names)
        // Returns dB value, e.g. "-6.5 dB" or "+1.2 dB"
        auto tryRG = [&](const char* key) -> float {
            auto s = tag->properties()[key].toString().to8Bit(true);
            if (s.empty()) return 0.0f;
            try { return std::stof(s); } catch (...) { return 0.0f; }
        };
        float rg_db = 0.0f;
        std::string rg_ext = fs::path(path).extension().string();
        std::transform(rg_ext.begin(), rg_ext.end(), rg_ext.begin(), ::tolower);
        if (rg_ext == ".opus") {
            // Opus (RFC 7845) carries gain as R128_TRACK_GAIN: an INTEGER in
            // Q7.8 fixed-point dB. Reading it as plain dB (the old fallback)
            // turned an ordinary tag into hundreds of negative dB and muted
            // the track. The conversion (and its -23 -> -18 LUFS rebase) lives
            // in R128Gain.h, SHARED with the rip encoder's tag-write direction
            // so the dialect cannot drift. The tag is relative to the OpusHead
            // output gain, which libopusfile already applies (OP_HEADER_GAIN
            // default), so there is no header-gain term here.
            if (float q78 = tryRG("R128_TRACK_GAIN"); q78 != 0.0f)
                rg_db = dbFromR128((int)q78);
        }
        if (rg_db == 0.0f) {
            rg_db = tryRG("REPLAYGAIN_TRACK_GAIN");
            if (rg_db == 0.0f) rg_db = tryRG("replaygain_track_gain");
        }
        info.replaygain_db = rg_db;
    }
    if (auto* ap = ref.audioProperties(); ap) {
        info.duration_sec = ap->lengthInSeconds();
        info.bitrate_kbps = ap->bitrate();
        info.sample_rate  = ap->sampleRate();
        info.channels     = ap->channels();
    }
    // TagLib reports the legacy stsd channelcount for AAC (often hardcoded to 2
    // even for mono); prefer the true count from the AudioSpecificConfig.
    if (int real = mp4AacChannelCount(path); real > 0)
        info.channels = real;
    // File size for live VBR bitrate calculation
    try { info.file_size_bytes = fs::file_size(path); } catch (...) {}
}

static bool open_decoder(const std::string& path, ma_decoder& dec,
                         ma_uint32 hint_channels = 0, ma_uint32 hint_rate = 0) {
    (void)hint_channels; (void)hint_rate;
    // Force a fixed 44100/2 output, exactly like the stream and CD paths. ma_decoder
    // upmixes/resamples the source to it, so the playback device is ALWAYS 44100/2 and
    // a file's odd native format (e.g. a 24000 Hz mono audiobook) never becomes the
    // device format. Opening the device at a file's quirky native rate/channels was
    // the cause of the chirp-then-silence on a cold first play of such a file. The
    // non-zero channels/rate also keep miniaudio off the format-sniffer path that can
    // crash on files with bad/dual ID3 tags (previously done via TagLib hints).
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
    // Plug in the custom backends (FDK-AAC, Opus, WavPack — CustomBackends.cpp)
    // so .aac/.m4a/.mp4/.opus/.wv decode through the same pipeline. Each fails
    // fast for non-matching input, so flac/mp3/wav still use miniaudio's built-ins.
    size_t nbackends = 0;
    cfg.ppCustomBackendVTables = remoct_custom_backends(&nbackends);
    cfg.customBackendCount     = (ma_uint32)nbackends;
    cfg.pCustomBackendUserData = nullptr;
#ifdef _WIN32
    auto wpath = utf8_to_wide(path);
    return ma_decoder_init_file_w(wpath.c_str(), &cfg, &dec) == MA_SUCCESS;
#else
    return ma_decoder_init_file(path.c_str(), &cfg, &dec) == MA_SUCCESS;
#endif
}

// Prime the decoder — attempt a small read to flush any bad initial frame.
// Some dual-tag MP3s (ID3v2.3+ID3v1.1) have a zero-byte first frame that
// causes the decoder to stall. A seek back to 0 after the probe resets it.
static void prime_decoder(ma_decoder& dec) {
    // Read a small chunk to warm up the decoder's bit reservoir
    float tmp[256 * 2];
    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&dec, tmp, 256, &frames_read);
    ma_decoder_seek_to_pcm_frame(&dec, 0);
}

// ─── LocalFileSource ──────────────────────────────────────────────────────────

bool LocalFileSource::open(const std::string& path) {
    close();
    if (!open_decoder(path, decoder_)) return false;
    if (decoder_.outputChannels == 0 || decoder_.outputSampleRate == 0) {
        ma_decoder_uninit(&decoder_);
        return false;
    }
    prime_decoder(decoder_);
    populate_track_info(info_, path);
    path_  = path;
    ready_ = true;
    return true;
}

void LocalFileSource::close() {
    if (ready_) {
        ma_decoder_uninit(&decoder_);
        std::memset(&decoder_, 0, sizeof(decoder_));
        ready_ = false;
    }
}

uint32_t LocalFileSource::readFrames(float* dst, uint32_t frame_count) {
    if (!ready_) return 0;
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(&decoder_, dst, frame_count, &got);
    return (uint32_t)got;
}

double LocalFileSource::positionSec() const {
    if (!ready_ || decoder_.outputSampleRate == 0) return 0.0;
    ma_uint64 cursor = 0;
    ma_decoder_get_cursor_in_pcm_frames(const_cast<ma_decoder*>(&decoder_), &cursor);
    return (double)cursor / (double)decoder_.outputSampleRate;
}

void LocalFileSource::seekToFrame(uint64_t frame) {
    if (!ready_) return;
    ma_decoder_seek_to_pcm_frame(&decoder_, (ma_uint64)frame);
}

bool LocalFileSource::seekTo(double seconds) {
    if (!ready_ || decoder_.outputSampleRate == 0) return false;
    const ma_uint32 rate = decoder_.outputSampleRate;
    const ma_uint32 ch   = decoder_.outputChannels ? decoder_.outputChannels : 2;
    ma_uint64 frame = (ma_uint64)(seconds * (double)rate);

    // Prime the decoder: land a little before the target and decode-discard up to it,
    // so a lossy codec's inter-frame state (the MP3 bit reservoir) is warm before
    // audio resumes. Without this the first ~50-150ms after a seek decode without
    // their reservoir context and come out garbled. Harmless for FLAC (it just
    // discards a few already-clean frames). Runs while the device is stopped, so the
    // few-ms extra decode is inaudible.
    const ma_uint64 prime = (ma_uint64)(0.18 * (double)rate);   // ~180ms of context
    ma_uint64 land    = (frame > prime) ? (frame - prime) : 0;
    ma_uint64 to_skip = frame - land;

    ma_decoder_seek_to_pcm_frame(&decoder_, land);
    if (to_skip > 0) {
        float scratch[2048];
        const ma_uint64 cap = (ch > 0) ? (2048u / ch) : 1024u;   // frames/read for this channel count
        while (to_skip > 0) {
            ma_uint64 want = std::min<ma_uint64>(to_skip, cap);
            ma_uint64 got  = 0;
            if (ma_decoder_read_pcm_frames(&decoder_, scratch, want, &got) != MA_SUCCESS || got == 0)
                break;                                            // EOF/short read — stop priming
            to_skip -= got;
        }
    }
    return true;
}
