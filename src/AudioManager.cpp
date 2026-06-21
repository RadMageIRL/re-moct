#define MINIAUDIO_IMPLEMENTATION
#include "AudioManager.h"
#include "StringUtils.h"

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <taglib/tpropertymap.h>

#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace fs = std::filesystem;

// ─── helpers ────────────────────────────────────────────────────────────────
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
        float rg_db = tryRG("REPLAYGAIN_TRACK_GAIN");
        if (rg_db == 0.0f) rg_db = tryRG("replaygain_track_gain");
        if (rg_db == 0.0f) rg_db = tryRG("R128_TRACK_GAIN");
        // Convert dB to linear: gain = 10^(dB/20)
        info.replaygain_db = rg_db;
    }
    if (auto* ap = ref.audioProperties(); ap) {
        info.duration_sec = ap->lengthInSeconds();
        info.bitrate_kbps = ap->bitrate();
        info.sample_rate  = ap->sampleRate();
        info.channels     = ap->channels();
    }
    // File size for live VBR bitrate calculation
    try { info.file_size_bytes = fs::file_size(path); } catch (...) {}
}

static bool open_decoder(const std::string& path, ma_decoder& dec,
                         ma_uint32 hint_channels = 0, ma_uint32 hint_rate = 0) {
    // Use TagLib-provided hints when available to bypass miniaudio's format sniffer
    // which crashes on files with bad/dual ID3 tags
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32,
                                hint_channels, hint_rate);
#ifdef _WIN32
    auto wpath = utf8_to_wide(path);
    return ma_decoder_init_file_w(wpath.c_str(), &cfg, &dec) == MA_SUCCESS;
#else
    return ma_decoder_init_file(path.c_str(), &cfg, &dec) == MA_SUCCESS;
#endif
}

// ─── construction ────────────────────────────────────────────────────────────
AudioManager::AudioManager()  = default;
AudioManager::~AudioManager() {
    bpm_cancel_.store(true);
    if (bpm_thread_.joinable()) bpm_thread_.join();
    teardown();
    teardownNext();
}

void AudioManager::teardown() {
    // Cancel any running BPM detection
    bpm_cancel_.store(true);
    if (bpm_thread_.joinable()) bpm_thread_.join();
    bpm_cancel_.store(false);

    state_.store(PlaybackState::Stopped);
    cached_duration_.store(0.0);
    crossfading_.store(false);
    xfade_pos_.store(0.0f);
    if (device_initialised_) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        device_initialised_ = false;
    }
    if (decoder_initialised_) {
        ma_decoder_uninit(&decoder_);
        decoder_initialised_ = false;
    }
    viz_buf_.fill(0.0f);
    viz_write_pos_.store(0);
}

void AudioManager::clearNext() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    teardownNext();
}

void AudioManager::teardownNext() {
    if (next_decoder_initialised_.load()) {
        ma_decoder_uninit(&next_decoder_);
        next_decoder_initialised_.store(false);
    }
    next_path_.clear();
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

// ─── play ────────────────────────────────────────────────────────────────────
bool AudioManager::play(const std::string& path) {
    std::lock_guard<std::mutex> lock(state_mutex_);
#ifdef _WIN32
    // Exit CD mode if switching to file playback
    if (cd_mode_.load()) {
        cd_source_.stop();
        cd_source_.close();
        cd_mode_.store(false);
        if (device_initialised_) {
            ma_device_stop(&device_);
            ma_device_uninit(&device_);
            device_initialised_ = false;
        }
    }
#endif
    teardown();
    teardownNext();
    track_ended_flag_.store(false);

    // Pre-read audio properties via TagLib — use as hints to bypass
    // miniaudio's format sniffer which crashes on problematic tag combinations
    ma_uint32 hint_ch = 0, hint_rate = 0;
    {
#ifdef _WIN32
        auto wpath = utf8_to_wide(path);
        TagLib::FileRef ref(wpath.c_str(), true, TagLib::AudioProperties::Fast);
#else
        TagLib::FileRef ref(path.c_str(), true, TagLib::AudioProperties::Fast);
#endif
        if (!ref.isNull() && ref.audioProperties()) {
            hint_ch   = (ma_uint32)ref.audioProperties()->channels();
            hint_rate = (ma_uint32)ref.audioProperties()->sampleRate();
        }
    }

    if (!open_decoder(path, decoder_, hint_ch, hint_rate)) return false;
    if (decoder_.outputChannels == 0 || decoder_.outputSampleRate == 0) {
        ma_decoder_uninit(&decoder_);
        return false;
    }
    prime_decoder(decoder_);
    decoder_initialised_ = true;
    populate_track_info(current_track_, path);

    if (!initDevice()) { teardown(); return false; }
    ma_device_set_master_volume(&device_, volume_.load());
    ma_device_start(&device_);
    state_.store(PlaybackState::Playing);
    track_ended_flag_.store(false);

    // Seed duration from TagLib (already available in current_track_)
    // This avoids calling ma_decoder_get_length_in_pcm_frames from UI thread
    // which races with the audio callback on files without Xing/Info headers
    cached_duration_.store((double)current_track_.duration_sec);

    // Start background BPM detection
    startBpmDetection(path, (int)decoder_.outputSampleRate);
    return true;
}

bool AudioManager::initDevice() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = decoder_.outputChannels;
    cfg.sampleRate        = decoder_.outputSampleRate;
    cfg.dataCallback      = &AudioManager::maDataCallback;
    cfg.stopCallback      = &AudioManager::maStopCallback;
    cfg.pUserData         = this;
    if (has_selected_device_)
        cfg.playback.pDeviceID = &selected_device_id_;
    if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS) return false;
    device_initialised_ = true;
    return true;
}

// ─── output device selection ─────────────────────────────────────────────────
std::vector<AudioManager::DeviceInfo> AudioManager::enumerateDevices() const {
    std::vector<DeviceInfo> result;
    ma_context ctx {};
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return result;

    ma_device_info* pPlayback = nullptr;
    ma_uint32 playback_count  = 0;
    if (ma_context_get_devices(&ctx, &pPlayback, &playback_count,
                               nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < playback_count; ++i)
            result.push_back({ std::string(pPlayback[i].name), pPlayback[i].id });
    }
    ma_context_uninit(&ctx);
    return result;
}

void AudioManager::setDevice(const ma_device_id* id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (id) {
        selected_device_id_  = *id;
        has_selected_device_ = true;
    } else {
        has_selected_device_ = false;
        selected_device_idx_ = -1;
    }
    // If currently playing, restart on new device
    if (state_.load() != PlaybackState::Stopped && decoder_initialised_) {
        std::string path = current_track_.path;
        double pos       = positionSec();
        teardown();
        if (open_decoder(path, decoder_)) {
            decoder_initialised_ = true;
            if (initDevice()) {
                ma_device_set_master_volume(&device_, volume_.load());
                ma_uint64 frame = (ma_uint64)(pos * decoder_.outputSampleRate);
                ma_decoder_seek_to_pcm_frame(&decoder_, frame);
                state_.store(PlaybackState::Playing);
                ma_device_start(&device_);
            }
        }
    }
}
bool AudioManager::preloadNext(const std::string& path) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    teardownNext();
    ma_uint32 hint_ch = 0, hint_rate = 0;
    {
#ifdef _WIN32
        auto wpath = utf8_to_wide(path);
        TagLib::FileRef ref(wpath.c_str(), true, TagLib::AudioProperties::Fast);
#else
        TagLib::FileRef ref(path.c_str(), true, TagLib::AudioProperties::Fast);
#endif
        if (!ref.isNull() && ref.audioProperties()) {
            hint_ch   = (ma_uint32)ref.audioProperties()->channels();
            hint_rate = (ma_uint32)ref.audioProperties()->sampleRate();
        }
    }
    if (!open_decoder(path, next_decoder_, hint_ch, hint_rate)) return false;
    if (next_decoder_.outputChannels == 0 || next_decoder_.outputSampleRate == 0) {
        ma_decoder_uninit(&next_decoder_);
        return false;
    }
    prime_decoder(next_decoder_);
    next_path_ = path;
    populate_track_info(next_track_info_, path);
    // Store with release ordering so audio thread sees fully-written next_track_info_
    next_decoder_initialised_.store(true, std::memory_order_release);
    return true;
}

// Called from audio thread when crossfade should begin (or track ends gaplessly)
void AudioManager::initCrossfade() {
    // Acquire ordering on next_decoder_initialised_ ensures next_track_info_ is visible
    if (!next_decoder_initialised_.load(std::memory_order_acquire)) return;
    // If repeat-one is active, don't swap — just loop the current track
    if (repeat_one_.load()) {
        // Seek current decoder back to start instead
        ma_uint64 zero = 0;
        ma_decoder_seek_to_pcm_frame(&decoder_, zero);
        return;
    }

    // Swap decoders: next becomes current
    if (decoder_initialised_) ma_decoder_uninit(&decoder_);
    decoder_             = next_decoder_;
    decoder_initialised_ = true;
    next_decoder_initialised_.store(false);
    std::memset(&next_decoder_, 0, sizeof(next_decoder_));

    current_track_ = next_track_info_;  // safe: acquire on next_decoder_initialised_ above
    current_track_.bpm = 0;
    cached_duration_.store((double)current_track_.duration_sec);
    next_path_.clear();
    bpm_needed_flag_.store(true);
}

// ─── pause / resume / stop ───────────────────────────────────────────────────
void AudioManager::pause() {
    if (state_.load() != PlaybackState::Playing) return;
    state_.store(PlaybackState::Paused);
    ma_device_stop(&device_);
}

void AudioManager::resume() {
    if (state_.load() != PlaybackState::Paused) return;
    state_.store(PlaybackState::Playing);
    ma_device_start(&device_);
}

void AudioManager::togglePause() {
#ifdef _WIN32
    if (cd_mode_.load()) {
        cd_source_.pause(!cd_source_.paused());
        return;
    }
#endif
    if      (state_.load() == PlaybackState::Playing) pause();
    else if (state_.load() == PlaybackState::Paused)  resume();
}

void AudioManager::stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
#ifdef _WIN32
    if (cd_mode_.load()) {
        cd_source_.stop();
        cd_source_.close();
        cd_mode_.store(false);
        if (device_initialised_) {
            ma_device_stop(&device_);
            ma_device_uninit(&device_);
            device_initialised_ = false;
        }
        track_ended_flag_.store(false);  // don't advance playlist on manual stop
        return;
    }
#endif
    teardown();
    teardownNext();
    track_ended_flag_.store(false);  // don't advance playlist on manual stop
    current_track_ = {};
}

// ─── seek ────────────────────────────────────────────────────────────────────
void AudioManager::seekTo(double seconds) {
    if (!decoder_initialised_ || decoder_.outputSampleRate == 0) return;
    if (state_.load() == PlaybackState::Stopped) return;
    double dur = durationSec();
    if (seconds < 0.0) seconds = 0.0;
    if (dur > 0.0 && seconds > dur) seconds = dur;
    ma_uint64 frame = (ma_uint64)(seconds * (double)decoder_.outputSampleRate);
    bool was_playing = (state_.load() == PlaybackState::Playing);
    seeking_.store(true);
    if (was_playing && device_initialised_) ma_device_stop(&device_);
    ma_decoder_seek_to_pcm_frame(&decoder_, frame);
    if (was_playing && device_initialised_) ma_device_start(&device_);
    seeking_.store(false);
}

void AudioManager::seekBy(double delta) {
#ifdef _WIN32
    if (cd_mode_.load()) {
        int new_pos = std::clamp(cd_source_.positionSec() + (int)delta, 0, cd_source_.durationSec());
        cd_source_.seekTo(new_pos);
        return;
    }
#endif
    seekTo(positionSec() + delta);
}

// ─── volume ──────────────────────────────────────────────────────────────────
void AudioManager::setVolume(float v) {
    v = std::clamp(v, 0.0f, 2.0f);
    volume_.store(v);
    if (device_initialised_) ma_device_set_master_volume(&device_, v);
}

void AudioManager::adjustVolume(float delta) { setVolume(volume_.load() + delta); }

// ─── query ───────────────────────────────────────────────────────────────────
double AudioManager::positionSec() const {
    if (!decoder_initialised_ || decoder_.outputSampleRate == 0) return 0.0;
    ma_uint64 cursor = 0;
    ma_decoder_get_cursor_in_pcm_frames(const_cast<ma_decoder*>(&decoder_), &cursor);
    return (double)cursor / (double)decoder_.outputSampleRate;
}

double AudioManager::durationSec() const {
    return cached_duration_.load();
}

int AudioManager::liveBitrateKbps() const {
    if (!decoder_initialised_ || decoder_.outputSampleRate == 0)
        return current_track_.bitrate_kbps;
    if (current_track_.file_size_bytes == 0 || current_track_.duration_sec <= 0)
        return current_track_.bitrate_kbps;

    // Detect CBR: standard bitrates are exact round numbers
    // If TagLib reports a standard CBR value, trust it — don't estimate
    static const int cbr_rates[] = {
        32,40,48,56,64,80,96,112,128,160,192,224,256,320, 0
    };
    int taglib_br = current_track_.bitrate_kbps;
    for (int i = 0; cbr_rates[i]; ++i) {
        if (taglib_br == cbr_rates[i]) return taglib_br;  // CBR — use static value
    }

    // Lossless formats (FLAC, WAV, AIFF, ALAC) have naturally variable frame sizes
    // but aren't VBR in a meaningful sense — use TagLib static value
    auto& p = current_track_.path;
    auto dot = p.rfind('.');
    auto ext = dot != std::string::npos ? p.substr(dot) : std::string{};
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".flac" || ext == ".wav" || ext == ".aiff"
        || ext == ".aif" || ext == ".alac" || ext == ".ape"
        || ext == ".wv")
        return taglib_br;
    static double  prev_file_pos = 0.0;
    static auto    prev_time     = std::chrono::steady_clock::now();
    static int     cached_kbps   = 0;

    auto now     = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - prev_time).count();

    if (elapsed >= 0.5) {
        double pos_sec  = positionSec();
        double dur      = current_track_.duration_sec;
        double file_pos = (dur > 0) ? (pos_sec / dur) * (double)current_track_.file_size_bytes : 0.0;

        if (prev_file_pos > 0 && elapsed > 0) {
            double bytes_per_sec = (file_pos - prev_file_pos) / elapsed;
            if (bytes_per_sec > 0)
                cached_kbps = (int)(bytes_per_sec * 8.0 / 1000.0);
        }
        prev_file_pos = file_pos;
        prev_time     = now;
    }

    return (cached_kbps > 0) ? cached_kbps : taglib_br;
}

// ─── visualizer ──────────────────────────────────────────────────────────────
int AudioManager::copySamples(float* dst, int n) const {
    if (n <= 0 || n > VIZ_BUF_SIZE) return 0;
    int wp = viz_write_pos_.load();
    for (int i = 0; i < n; ++i)
        dst[i] = viz_buf_[(wp - n + i + VIZ_BUF_SIZE) % VIZ_BUF_SIZE];
    return n;
}

void AudioManager::pushVizSample(float s) {
    int wp = viz_write_pos_.load();
    viz_buf_[wp % VIZ_BUF_SIZE] = s;
    viz_write_pos_.store((wp + 1) % VIZ_BUF_SIZE);
}

// ─── poll ────────────────────────────────────────────────────────────────────
void AudioManager::pollEvents() {
    if (bpm_needed_flag_.exchange(false))
        startBpmDetection(current_track_.path, (int)decoder_.outputSampleRate);
    if (preload_next_flag_.exchange(false))
        if (on_preload_next_) on_preload_next_();
    if (track_ended_flag_.exchange(false))
        if (on_track_end_) on_track_end_();
}

// ─── audio callback ──────────────────────────────────────────────────────────
void AudioManager::maDataCallback(ma_device* device, void* output,
                                   const void*, ma_uint32 frame_count) {
    static_cast<AudioManager*>(device->pUserData)->onDataCallback(output, frame_count);
}

void AudioManager::onDataCallback(void* output, ma_uint32 frame_count) {
    const ma_uint32 ch = device_.playback.channels;
    if (!decoder_initialised_ && !cd_mode_.load()) {
        if (ch > 0) std::memset(output, 0, frame_count * ch * sizeof(float));
        return;
    }

#ifdef _WIN32
    // ── CD mode: read directly from CDSource ring buffer ─────────────────
    if (cd_mode_.load()) {
        float* out = static_cast<float*>(output);
        cd_source_.readFrames(out, frame_count);
        // Apply volume, mute, balance and EQ to CD output
        if (muted_.load()) {
            std::memset(out, 0, frame_count * 2 * sizeof(float));
        } else {
            float vol = volume_.load();
            if (vol != 1.0f)
                for (ma_uint32 i = 0; i < frame_count * 2; ++i) out[i] *= vol;
            float bal = balance_.load();
            if (bal != 0.0f) {
                float lg = (bal <= 0.0f) ? 1.0f : 1.0f - bal;
                float rg = (bal >= 0.0f) ? 1.0f : 1.0f + bal;
                for (ma_uint32 f = 0; f < frame_count; ++f) {
                    out[f*2]   *= lg;
                    out[f*2+1] *= rg;
                }
            }
            // EQ — CD is always 44100Hz stereo
            if (eq_enabled_.load())
                applyEq(out, frame_count, 2, 44100.0f);
        }
        // Media removed — silence output, let UI loop handle cleanup
        if (cd_source_.mediaRemoved()) {
            std::memset(out, 0, frame_count * 2 * sizeof(float));
            // Don't fire track_ended_flag_ here — UI loop will closeCD and purge playlist
            return;
        }
        // Signal track end only when a track was playing and has now stopped
        if (cd_source_.currentTrack() > 0 &&
            !cd_source_.isPlaying() && !cd_source_.paused())
            track_ended_flag_.store(true);
        return;
    }
#endif

    float* out    = static_cast<float*>(output);
    const ma_uint32 sr = device_.sampleRate;

    if (!decoder_initialised_ || ch == 0 || sr == 0) {
        std::memset(output, 0, frame_count * std::max(ch, 2u) * sizeof(float));
        return;
    }

    // ── Crossfade trigger ────────────────────────────────────────────────────
    if (crossfade_secs > 0.0f && next_decoder_initialised_.load() && !crossfading_.load()) {
        double pos = positionSec();
        double dur = durationSec();
        if (dur > 0.0 && (dur - pos) <= (double)crossfade_secs) {
            crossfading_.store(true);
            xfade_pos_.store(0.0f);
            xfade_step_ = 1.0f / ((float)crossfade_secs * (float)sr);
        }
    }

    ma_uint64 frames_read_a = 0;
    ma_result result = MA_SUCCESS;
    try {
        result = ma_decoder_read_pcm_frames(&decoder_, out, frame_count, &frames_read_a);
    } catch (...) {
        std::memset(out, 0, frame_count * ch * sizeof(float));
        track_ended_flag_.store(true);
        return;
    }

    bool track_done = (result == MA_AT_END || frames_read_a < frame_count);

    if (crossfading_.load() && next_decoder_initialised_.load()) {
        // Mix next track in, ramping up while current ramps down
        std::vector<float> tmp((size_t)frame_count * ch, 0.0f);

        ma_uint64 frames_read_b = 0;
        try {
            ma_decoder_read_pcm_frames(&next_decoder_, tmp.data(), frame_count, &frames_read_b);
        } catch (...) { frames_read_b = 0; }

        float xp = xfade_pos_.load();
        for (ma_uint32 f = 0; f < frame_count; ++f) {
            float gain_a = std::max(0.0f, 1.0f - xp);
            float gain_b = std::min(1.0f, xp);
            for (ma_uint32 c = 0; c < ch; ++c) {
                ma_uint64 i = (ma_uint64)f * ch + c;
                float a = (f < frames_read_a) ? out[i] : 0.0f;
                float b = (f < frames_read_b) ? tmp[(size_t)i] : 0.0f;
                out[i] = a * gain_a + b * gain_b;
            }
            xp += xfade_step_;
        }
        xfade_pos_.store(xp);

        // Crossfade complete or current track ended
        if (xp >= 1.0f || track_done) {
            // Swap: next becomes current
            initCrossfade();
            crossfading_.store(false);
            // Signal UI to preload the NEXT next track only — don't replay
            preload_next_flag_.store(true);
        }
    } else if (track_done) {
        // Gapless: if next is preloaded, switch immediately with no gap
        if (next_decoder_initialised_.load()) {
            // Fill remainder from next decoder
            ma_uint32 remaining = frame_count - (ma_uint32)frames_read_a;
            if (remaining > 0) {
                ma_uint64 fr = 0;
                ma_decoder_read_pcm_frames(&next_decoder_,
                    out + frames_read_a * ch, remaining, &fr);
                if (fr < remaining)
                    std::memset(out + (frames_read_a + fr) * ch, 0,
                        (remaining - fr) * ch * sizeof(float));
            }
            initCrossfade();
            track_ended_flag_.store(true);
        } else {
            // No next track preloaded — silence remainder
            if (frames_read_a < frame_count)
                std::memset(out + frames_read_a * ch, 0,
                    (frame_count - (ma_uint32)frames_read_a) * ch * sizeof(float));
            track_ended_flag_.store(true);
        }
    }

    // Feed visualizer from output (already mixed)
    for (ma_uint32 f = 0; f < frame_count; ++f) {
        float mono = 0.0f;
        for (ma_uint32 c = 0; c < ch; ++c)
            mono += out[(ma_uint64)f * ch + c];
        pushVizSample(mono / (float)ch);
    }

    // ── Speed adjustment (tape-speed: pitch shifts with speed) ───────────
    float spd = speed_.load();
    if (spd != 1.0f && frame_count > 1) {
        std::vector<float> speed_buf((size_t)frame_count * ch);
        std::memcpy(speed_buf.data(), out, frame_count * ch * sizeof(float));
        for (ma_uint32 f = 0; f < frame_count; ++f) {
            float src_f = f * spd;
            auto  src_i = (ma_uint32)src_f;
            if (src_i >= frame_count) src_i = frame_count - 1;  // clamp
            float frac  = src_f - (float)src_i;
            ma_uint32 src_i1 = std::min(src_i + 1, frame_count - 1);
            for (ma_uint32 c = 0; c < ch; ++c) {
                float a = speed_buf[(ma_uint64)src_i  * ch + c];
                float b = speed_buf[(ma_uint64)src_i1 * ch + c];
                out[(ma_uint64)f * ch + c] = a + frac * (b - a);
            }
        }
    }

    // ── ReplayGain ────────────────────────────────────────────────────────
    if (replaygain_enabled_.load()) {
        float db   = current_track_.replaygain_db;
        float gain = (db != 0.0f) ? std::pow(10.0f, db / 20.0f) : 1.0f;
        gain *= std::pow(10.0f, 6.0f / 20.0f);
        gain = std::clamp(gain, 0.0f, 4.0f);
        if (gain != 1.0f) {
            for (ma_uint32 i = 0; i < frame_count * ch; ++i)
                out[i] *= gain;
        }
    }

    // ── Balance ──────────────────────────────────────────────────────────
    float bal = balance_.load();
    if (bal != 0.0f && ch == 2) {
        float left_gain  = (bal <= 0.0f) ? 1.0f : 1.0f - bal;
        float right_gain = (bal >= 0.0f) ? 1.0f : 1.0f + bal;
        for (ma_uint32 f = 0; f < frame_count; ++f) {
            out[(ma_uint64)f * 2 + 0] *= left_gain;
            out[(ma_uint64)f * 2 + 1] *= right_gain;
        }
    }

    // ── Equalizer ────────────────────────────────────────────────────────
    if (eq_enabled_.load()) {
        if (eq_dirty_) rebuildEqCoeffs();
        applyEq(out, frame_count, ch, (float)device_.sampleRate);
    }

    // ── Mute ─────────────────────────────────────────────────────────────
    if (muted_.load())
        std::memset(out, 0, frame_count * ch * sizeof(float));
}

void AudioManager::maStopCallback(ma_device* device) {
    auto* self = static_cast<AudioManager*>(device->pUserData);
    if (self->seeking_.load()) return;
    // Only fire if not already handled in data callback
    // (crossfade/gapless fire track_ended_flag_ from data callback directly)
}

// ─── BPM detection ───────────────────────────────────────────────────────────
void AudioManager::startBpmDetection(const std::string& path, int sample_rate) {
    // Cancel any existing BPM thread
    bpm_cancel_.store(true);
    if (bpm_thread_.joinable()) bpm_thread_.join();
    bpm_cancel_.store(false);
    current_track_.bpm = 0;

    if (path.empty()) return;

    bpm_thread_ = std::thread([this, path, sample_rate]() {
        int bpm = detectBpm(path, sample_rate, bpm_cancel_);
        if (!bpm_cancel_.load())
            current_track_.bpm = bpm;
    });
}

// Autocorrelation BPM detection.
// Strategy:
//   1. Decode the first ~20 seconds of audio into a mono float buffer
//   2. Downsample to ~4 kHz to reduce computation (energy envelope)
//   3. Compute autocorrelation across lags for 50–200 BPM
//   4. Find the peak lag, convert to BPM
int AudioManager::detectBpm(const std::string& path, int sample_rate,
                              const std::atomic<bool>& cancel) {
    if (cancel.load()) return 0;

    // Open a fresh decoder just for analysis — don't touch the playback decoder
    ma_decoder dec {};
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0); // mono f32
#ifdef _WIN32
    {
        auto wp = utf8_to_wide(path);
        if (ma_decoder_init_file_w(wp.c_str(), &cfg, &dec) != MA_SUCCESS) return 0;
    }
#else
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return 0;
#endif

    const int sr         = (int)dec.outputSampleRate;
    const int max_secs   = 20;
    const int max_frames = sr * max_secs;

    // Read raw samples
    std::vector<float> raw(max_frames);
    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&dec, raw.data(), (ma_uint64)max_frames, &frames_read);
    ma_decoder_uninit(&dec);

    if (cancel.load() || frames_read < (ma_uint64)(sr * 2)) return 0;
    raw.resize(frames_read);

    // Downsample to ~200 Hz by computing RMS energy in windows
    // Window size ~10ms, giving ~100 samples/sec
    const int win = sr / 100;  // ~10ms window
    int n_env = (int)(frames_read / win);
    if (n_env < 200) return 0;

    std::vector<float> env(n_env);
    for (int i = 0; i < n_env; ++i) {
        float sum = 0.0f;
        for (int j = 0; j < win; ++j) {
            float s = raw[(size_t)(i * win + j)];
            sum += s * s;
        }
        env[i] = std::sqrt(sum / win);
    }

    if (cancel.load()) return 0;

    // Remove DC (mean)
    float mean = 0.0f;
    for (float v : env) mean += v;
    mean /= n_env;
    for (float& v : env) v -= mean;

    // Autocorrelation
    // env_sr ≈ 100 samples/sec
    // BPM range 50–200 → period range 100/200=0.5s to 100/50=2.0s
    // lag range: 50–200 samples in env units
    const float env_sr   = (float)sr / win;
    const int   lag_min  = (int)(env_sr * 60.0f / 200.0f);  // 200 BPM
    const int   lag_max  = (int)(env_sr * 60.0f / 50.0f);   // 50 BPM
    if (lag_min <= 0 || lag_max >= n_env) return 0;

    int    best_lag = lag_min;
    double best_val = -1e30;

    for (int lag = lag_min; lag <= lag_max; ++lag) {
        if (cancel.load()) return 0;
        double acc = 0.0;
        int    cnt = n_env - lag;
        for (int i = 0; i < cnt; ++i)
            acc += (double)env[i] * env[i + lag];
        acc /= cnt;
        if (acc > best_val) {
            best_val = acc;
            best_lag = lag;
        }
    }

    // Convert lag back to BPM
    float period_secs = best_lag / env_sr;
    int bpm = (int)std::round(60.0f / period_secs);

    // Sanity clamp
    bpm = std::clamp(bpm, 50, 200);
    return bpm;
}

// ─── Equalizer ───────────────────────────────────────────────────────────────
static const float EQ_FREQS[10] = {
    31.0f, 62.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};
static const float EQ_Q = 1.41f;  // ~1 octave bandwidth

AudioManager::BiquadCoeff AudioManager::makePeakingEq(
        float freq, float gain_db, float q, float sr) {
    // Peaking EQ biquad (RBJ cookbook)
    BiquadCoeff c;
    if (std::abs(gain_db) < 0.01f) return c;  // unity, passthrough
    float A  = std::pow(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * 3.14159265f * freq / sr;
    float cw = std::cos(w0);
    float sw = std::sin(w0);
    float alpha = sw / (2.0f * q);
    float b0 =  1.0f + alpha * A;
    float b1 = -2.0f * cw;
    float b2 =  1.0f - alpha * A;
    float a0 =  1.0f + alpha / A;
    float a1 = -2.0f * cw;
    float a2 =  1.0f - alpha / A;
    c.a0 = b0 / a0;
    c.a1 = b1 / a0;
    c.a2 = b2 / a0;
    c.b1 = a1 / a0;
    c.b2 = a2 / a0;
    return c;
}

void AudioManager::rebuildEqCoeffs() {
    float sr = decoder_initialised_ ? (float)decoder_.outputSampleRate : 44100.0f;
    std::lock_guard<std::mutex> lk(eq_mutex_);
    for (int b = 0; b < EQ_BANDS; ++b)
        eq_coeffs_[b] = makePeakingEq(EQ_FREQS[b], eq_gains_db_[b], EQ_Q, sr);
    // Reset state to avoid pops when coefficients change
    for (int b = 0; b < EQ_BANDS; ++b)
        for (int c = 0; c < 2; ++c)
            eq_state_[b][c] = {};
    eq_dirty_ = false;
}

void AudioManager::applyEq(float* out, ma_uint32 frames,
                            ma_uint32 channels, float /*sr*/) {
    std::lock_guard<std::mutex> lk(eq_mutex_);
    for (int b = 0; b < EQ_BANDS; ++b) {
        const auto& cf = eq_coeffs_[b];
        if (cf.a0 == 1.0f && cf.a1 == 0.0f) continue;  // unity, skip
        for (ma_uint32 f = 0; f < frames; ++f) {
            for (ma_uint32 c = 0; c < channels && c < 2; ++c) {
                auto& s = eq_state_[b][c];
                float x = out[(ma_uint64)f * channels + c];
                float y = cf.a0*x + cf.a1*s.x1 + cf.a2*s.x2
                                   - cf.b1*s.y1 - cf.b2*s.y2;
                s.x2 = s.x1; s.x1 = x;
                s.y2 = s.y1; s.y1 = y;
                out[(ma_uint64)f * channels + c] = y;
            }
        }
    }
}

void AudioManager::setEqGain(int band, float db) {
    if (band < 0 || band >= EQ_BANDS) return;
    db = std::clamp(db, -12.0f, 12.0f);
    {
        std::lock_guard<std::mutex> lk(eq_mutex_);
        eq_gains_db_[band] = db;
        eq_dirty_ = true;
    }
    rebuildEqCoeffs();
}

float AudioManager::getEqGain(int band) const {
    if (band < 0 || band >= EQ_BANDS) return 0.0f;
    std::lock_guard<std::mutex> lk(eq_mutex_);
    return eq_gains_db_[band];
}

void AudioManager::resetEq() {
    {
        std::lock_guard<std::mutex> lk(eq_mutex_);
        for (int b = 0; b < EQ_BANDS; ++b) eq_gains_db_[b] = 0.0f;
        eq_dirty_ = true;
    }
    rebuildEqCoeffs();
}


// ─── CD Audio mode ────────────────────────────────────────────────────────────
#ifdef _WIN32
bool AudioManager::openCD(const std::string& drive_letter) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // teardown handles already-stopped device safely
    teardown();
    teardownNext();
    track_ended_flag_.store(false);
    if (!cd_source_.open(drive_letter)) return false;
    cd_mode_.store(true);
    // Init device for 44100Hz stereo float (Red Book format)
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 44100;
    cfg.dataCallback      = &AudioManager::maDataCallback;
    cfg.stopCallback      = &AudioManager::maStopCallback;
    cfg.pUserData         = this;
    if (has_selected_device_) cfg.playback.pDeviceID = &selected_device_id_;
    if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS) {
        cd_source_.close();
        cd_mode_.store(false);
        return false;
    }
    device_initialised_ = true;
    ma_device_set_master_volume(&device_, volume_.load());
    ma_device_start(&device_);
    return true;
}

void AudioManager::closeCD() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    cd_source_.stop();
    cd_source_.close();
    cd_mode_.store(false);
    if (device_initialised_) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        device_initialised_ = false;
    }
}

bool AudioManager::playCDTrack(int track_number) {
    if (!cd_mode_.load() || !cd_source_.isOpen()) return false;
    return cd_source_.playTrack(track_number);
}
const std::vector<CDTrack>& AudioManager::cdTracks() const {
    return cd_source_.tracks();
}

int AudioManager::cdPositionSec()  const { return cd_source_.positionSec(); }
int AudioManager::cdDurationSec()  const { return cd_source_.durationSec(); }
int AudioManager::cdCurrentTrack() const { return cd_source_.currentTrack(); }
#endif
