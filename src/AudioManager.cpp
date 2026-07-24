#define MINIAUDIO_IMPLEMENTATION
#include "AudioManager.h"
#include "StringUtils.h"
#include "CustomBackends.h" // AAC/Opus/WavPack backends (used by detectBpm's analysis decoder)
#include "PortUtil.h"     // port::exeDir — resolve the streaming plugin beside the binary (slice c)
#include "Log.h"          // stream-plugin load diagnostics

// Slice B: the file-path helpers (populate_track_info / open_decoder /
// prime_decoder) and the TagLib metadata read moved verbatim into
// LocalFileSource.cpp; decoder ownership lives in LocalFileSource objects.

#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

// ─── construction ────────────────────────────────────────────────────────────
AudioManager::AudioManager()  = default;

// Streaming plugin (slice c): resolve <exeDir>/plugins/remoct_stream.{dll,so} —
// the module beside the binary. Static (used by loaded_plugin_'s default member
// initializer). exeDir() falls back to "." if the OS query fails.
std::string AudioManager::streamPluginPath() {
    std::string dir = port::exeDir();
    if (dir.empty()) dir = ".";
#ifdef _WIN32
    return dir + "/plugins/remoct_stream.dll";
#else
    return dir + "/plugins/remoct_stream.so";
#endif
}

// "" when the streaming plugin loaded; else a human-readable reason (the slice-a
// reject-path names, now a production diagnostic).
std::string AudioManager::streamPluginError() const {
    if (stream_plugin_.valid()) return {};
    return std::string("streaming plugin unavailable: ") +
           core::pluginLoadName(plugin_load_result_);
}
AudioManager::~AudioManager() {
    bpm_cancel_.store(true);
    if (bpm_thread_.joinable()) bpm_thread_.join();
    if (stream_connect_thread_.joinable()) stream_connect_thread_.join();
    endRecording();            // finalize an in-flight capture while the plugin
                               // and device still exist (R1); no-op when idle
    teardown();
    teardownNext();
}

// ─── stream capture lifecycle (abi-cluster slice A) ──────────────────────────
// The keep-draining signal travels WITH the recorder lifecycle so no stop path
// can leave the plugin draining through pauses with nothing recording.
bool AudioManager::beginRecording(const RecOptions& opt, const std::string& station,
                                  const std::string& out_dir) {
    if (opt.copy_mode) {
        // abi-cluster slice B: copy mode. Wire the recorder's pull callback
        // to the plugin's read_encoded and arm the tee BEFORE the pump
        // starts. The PCM tap never arms in copy mode (StreamRecorder::start
        // owns that) — one lifecycle, one mode flag, the two tees can never
        // arm together (the design doc's hazard 5.7, closed structurally).
        if (!stream_plugin_.supportsEncodedCapture()) return false;  // old plugin:
        // the panel greys Copy pre-start; this is the belt to that suspender.
        RecOptions o = opt;
        o.pull = [this](uint8_t* dst, uint32_t cap,
                        int32_t* codec, int32_t* discont) {
            return stream_plugin_.readEncoded(dst, cap, codec, discont);
        };
        stream_plugin_.setEncodedCapture(true);
        if (!stream_recorder_.start(o, station, out_dir)) {
            stream_plugin_.setEncodedCapture(false);
            return false;
        }
        // Keep-draining composes: the tee sits producer-side, so a playback
        // pause mid-copy stays gapless through the same slice-A signal.
        stream_plugin_.setRecordActive(true);
        return true;
    }
    if (!stream_recorder_.start(opt, station, out_dir)) return false;
    // Best-effort: 1 = the plugin drains through playback pauses (gapless);
    // 0/absent = old plugin, the pause-gap behavior remains (the panel note
    // stays honest via recordingDrainSupported()).
    stream_plugin_.setRecordActive(true);
    return true;
}

void AudioManager::endRecording() {
    stream_recorder_.stop();               // idempotent; finalizes in-flight cut
                                           // (the copy pump joins here, BEFORE
                                           // the tee disarms — pull-while-armed)
    if (stream_plugin_.valid()) {
        stream_plugin_.setEncodedCapture(false); // no-op when never armed
        stream_plugin_.setRecordActive(false);   // no-op plugin-side when already off
    }
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
    file_src_.reset();      // device is stopped above — no callback can be mid-read
    retired_src_.reset();   // reap any un-reaped retired source while quiesced
    viz_buf_.fill(0.0f);
    viz_write_pos_.store(0);
    resetSpeedResampler();  // device is stopped above, so this is race-free
}

void AudioManager::clearNext() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    teardownNext();
}

// Install a swap the audio thread has already performed, on the main thread.
//
// If track_swap_flag_ is set then the swap ALREADY HAPPENED — file_src_ is the new
// track and it is audible. The only question is whether the UI's identity follows.
// Draining is therefore always correct: refusing to install can only desynchronize
// current_track_ from what is playing.
//
// Called from pollEvents() (the normal tick) and from the top of teardownNext(),
// because next_track_info_ is a SINGLE SLOT serving two roles — the payload of a
// swap awaiting install, and the info of a newly armed preload. preloadNext()
// overwrites it, so any teardown must drain the pending payload BEFORE the clobber.
// (Simply cancelling the install instead was measured: it strands the previous
// track's title against the new track's audio. Cancelling later, after the slot is
// overwritten, installs a track that was armed but never played. Drain-first is the
// only ordering that installs the track that actually swapped in.)
//
// Takes no lock and must not: pollEvents() holds none, teardownNext()'s callers all
// hold state_mutex_, and both entry points are the UI thread, so they cannot race.
void AudioManager::installPendingSwap() {
    if (track_swap_flag_.exchange(false)) {
        current_track_ = next_track_info_;
        // Reap the source the audio thread retired at the swap (slice B): the
        // seq_cst exchange above synchronizes-with the flag store that followed
        // the swap, so retired_src_ is fully written and quiescent here — the
        // free happens on the main thread, never in the audio callback.
        retired_src_.reset();
    }
}

void AudioManager::teardownNext() {
    installPendingSwap();   // drain before next_track_info_ can be clobbered
    if (next_decoder_initialised_.load(std::memory_order_acquire)) {
        // Clear the published flag FIRST so the audio thread starts no new
        // crossfade or gapless read of next_decoder_ (every such read is gated on
        // this flag). Ordering matters: the free must never precede the un-publish.
        next_decoder_initialised_.store(false, std::memory_order_release);

        // If a crossfade is actively mixing next_decoder_ across callbacks, the
        // un-publish alone can't protect an already-in-flight read. ma_device_stop()
        // blocks until the current callback returns and guarantees it won't be
        // re-entered, so after it the decoder is safe to free. Scope the stop to the
        // crossfade window only — the common mid-track clearNext (no crossfade) frees
        // directly with no audio glitch. (device_initialised_ + is_started also guards
        // the paused-mid-crossfade case, where the device is already stopped.)
        if (crossfading_.load(std::memory_order_acquire) &&
            device_initialised_ && ma_device_is_started(&device_)) {
            ma_device_stop(&device_);
            crossfading_.store(false);
            next_src_.reset();          // free inside the quiesced window
            ma_device_start(&device_);
        }
    }
    // Unconditional: initCrossfade's repeat-one branch un-publishes the armed
    // decoder from the audio thread without freeing it (it cannot free there), so
    // next_src_ can still be owned while the flag reads false. Freeing here is the
    // main-thread counterpart to that. No-op when the branch above already freed
    // it, and safe when the flag is false - both audio-thread read sites (the
    // crossfade mix and the gapless splice) are gated on that flag.
    next_src_.reset();
    next_path_.clear();
}

// ─── play ────────────────────────────────────────────────────────────────────
bool AudioManager::play(const std::string& path) {
    // Scheme-branch: HTTP(S) URLs route to the (non-blocking) streaming path.
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) {
        beginStream(path);
        return true;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Starting a file supersedes any in-flight stream connect (its result will be
    // discarded rather than clobbering this playback).
    stream_connect_gen_.fetch_add(1);
    // Exit stream mode if switching to file playback (clear the flag first so the
    // audio callback stops entering the stream branch, then join the producer).
    if (stream_mode_.load()) {
        stream_mode_.store(false);
        endRecording();            // finalize an in-flight capture BEFORE close (R1)
        stream_plugin_.close();
    }
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
    teardown();
    teardownNext();
    track_ended_flag_.store(false); track_end_advanced_.store(false);

    // Open the file as a source object (slice B). LocalFileSource::open =
    // open_decoder + zero-format guard + prime_decoder + populate_track_info,
    // the baseline sequence verbatim. (The old TagLib hint pre-read is gone:
    // open_decoder provably discarded its hints — `(void)hint_channels` — since
    // the forced 44100/2 config replaced hint-based sniffing.)
    auto src = std::make_unique<LocalFileSource>();
    if (!src->open(path)) return false;
    file_src_ = std::move(src);
    current_track_ = file_src_->info();
    current_rg_db_.store(current_track_.replaygain_db);  // mirror for the audio thread

    if (!initDevice()) { teardown(); return false; }
    ma_device_set_master_volume(&device_, volume_.load());
    ma_device_start(&device_);
    state_.store(PlaybackState::Playing);
    track_ended_flag_.store(false); track_end_advanced_.store(false);

    // Seed duration from TagLib (already available in current_track_)
    // This avoids calling ma_decoder_get_length_in_pcm_frames from UI thread
    // which races with the audio callback on files without Xing/Info headers
    cached_duration_.store((double)current_track_.duration_sec);

    // Start background BPM detection
    startBpmDetection(path, (int)file_src_->sampleRate());
    return true;
}

bool AudioManager::initDevice() {
    // ── One-time audio-backend warm-up ──────────────────────────────────────
    // The very first ma_device_init in the process brings up the shared playback
    // backend (WASAPI/COM on Windows). When that first bring-up coincides with the
    // custom FDK-AAC ma_decoder as the active decoder, the device ends up producing
    // silence for the rest of the session — a chirp then nothing, on every file and
    // stream after it. Playing a native-backend file (FLAC/MP3) or the radio path
    // first avoids it, which is the tell: it's the *first* backend init that's
    // fragile, not the AAC file. So prime the backend once here with a throwaway
    // fixed-format device, before any real init — now the first bring-up never
    // coincides with AAC, regardless of which file the user opens first. init/uninit
    // (no start) is enough to do the COM/WASAPI bring-up; the callback never fires.
    static bool audio_backend_warmed = false;
    if (!audio_backend_warmed) {
        audio_backend_warmed = true;
        ma_device_config wc = ma_device_config_init(ma_device_type_playback);
        wc.playback.format   = ma_format_f32;
        wc.playback.channels = 2;
        wc.sampleRate         = 44100;
        wc.dataCallback      = &AudioManager::maDataCallback;
        wc.pUserData         = this;
        if (has_selected_device_)
            wc.playback.pDeviceID = &selected_device_id_;
        ma_device warm{};
        if (ma_device_init(nullptr, &wc, &warm) == MA_SUCCESS) {
            // Fully activate the WASAPI render client once (start/stop), not just
            // init/uninit — init alone primes the backend enough for ordinary AAC
            // files but not for the first-start path some files take, so do the
            // complete bring-up here. The callback fires but file_src_ is null,
            // so it only writes a few ms of (inaudible) silence.
            ma_device_start(&warm);
            ma_device_stop(&warm);
            ma_device_uninit(&warm);
        }
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = file_src_->channels();     // always 2 (forced in open_decoder)
    cfg.sampleRate        = file_src_->sampleRate();   // always 44100 (forced)
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
    // Live sources (radio stream / CD) keep their source alive and own ma_device_
    // directly — hot-swap just the device endpoint without tearing down the source.
    if ((stream_mode_.load() || cd_mode_.load()) && device_initialised_) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        device_initialised_ = false;

        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate        = 44100;
        cfg.dataCallback      = &AudioManager::maDataCallback;
        cfg.stopCallback      = &AudioManager::maStopCallback;
        cfg.pUserData         = this;
        if (has_selected_device_) cfg.playback.pDeviceID = &selected_device_id_;
        if (ma_device_init(nullptr, &cfg, &device_) == MA_SUCCESS) {
            device_initialised_ = true;
            ma_device_set_master_volume(&device_, volume_.load());
            ma_device_start(&device_);
        }
        return;
    }
    // If currently playing, restart on new device
    if (state_.load() != PlaybackState::Stopped && file_src_) {
        std::string path = current_track_.path;
        double pos       = positionSec();
        teardown();
        auto src = std::make_unique<LocalFileSource>();
        if (src->open(path)) {
            file_src_ = std::move(src);
            if (initDevice()) {
                ma_device_set_master_volume(&device_, volume_.load());
                // Raw (unprimed) frame seek — the baseline restart shape.
                file_src_->seekToFrame((uint64_t)(pos * file_src_->sampleRate()));
                state_.store(PlaybackState::Playing);
                ma_device_start(&device_);
            }
        }
    }
}
bool AudioManager::preloadNext(const std::string& path) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    teardownNext();
    // Open the next source fully BEFORE publishing it (open = decoder +
    // zero-format guard + prime + TagLib info, the baseline sequence; the old
    // TagLib hint pre-read is gone — its outputs were provably discarded).
    auto src = std::make_unique<LocalFileSource>();
    if (!src->open(path)) return false;
    next_path_ = path;
    next_track_info_ = src->info();
    next_src_ = std::move(src);
    // Store with release ordering so audio thread sees fully-written next_track_info_
    // (and, slice B, the fully-constructed next_src_ object through the pointer —
    // the same happens-before edge, now covering pointer + pointee).
    next_decoder_initialised_.store(true, std::memory_order_release);
    return true;
}

// Called from audio thread when crossfade should begin (or track ends gaplessly)
void AudioManager::initCrossfade() {
    // Acquire ordering on next_decoder_initialised_ ensures next_track_info_ is visible
    if (!next_decoder_initialised_.load(std::memory_order_acquire)) return;
    // If repeat-one is active, don't swap — just loop the current track
    if (repeat_one_.load()) {
        // Seek current decoder back to start instead (raw, unprimed — baseline)
        if (file_src_) file_src_->seekToFrame(0);
        // Un-publish the armed decoder so a later pass cannot read it. Flag ONLY:
        // no reset() (that would free heap on the audio thread) and no retiring
        // into retired_src_ (which is reaped solely under track_swap_flag_, a flag
        // this branch never sets - so it would never be reaped, and a second pass
        // through here would free the previous one from the callback). teardownNext
        // frees next_src_ unconditionally on the main thread instead.
        next_decoder_initialised_.store(false, std::memory_order_release);
        return;
    }

    // Swap sources: next becomes current. The outgoing source is RETIRED, not
    // freed — pollEvents reaps retired_src_ on the main thread under the
    // track_swap_flag_ synchronizes-with edge below, so a UI-thread
    // positionSec() racing this swap can only ever dereference a live decoder,
    // and the audio callback no longer frees heap here. The publication
    // protocol is unchanged: next_src_ was made visible by preloadNext's
    // release store (guarded by the acquire above); the flag clear keeps its
    // exact position after the payload transfer.
    retired_src_ = std::move(file_src_);
    file_src_    = std::move(next_src_);
    next_decoder_initialised_.store(false);

    // Do NOT write current_track_ here — that struct (with its std::strings) is
    // owned by the main thread. Publish what the audio thread needs via atomics and
    // hand the string-bearing install off to pollEvents() via track_swap_flag_.
    current_rg_db_.store(next_track_info_.replaygain_db);
    live_bpm_.store(0);                  // new track: clear BPM (atomic)
    resetSpeedResampler();               // decoder swapped — drop stale varispeed residual
    cached_duration_.store((double)next_track_info_.duration_sec);
    next_path_.clear();
    track_swap_flag_.store(true);        // main thread installs current_track_ = next_track_info_
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
    if (cd_mode_.load()) {
        cd_source_.pause(!cd_source_.paused());
        state_.store(cd_source_.paused() ? PlaybackState::Paused
                                         : PlaybackState::Playing);
        return;
    }
    if (stream_mode_.load()) {
        stream_plugin_.pause(!stream_plugin_.paused());
        state_.store(stream_plugin_.paused() ? PlaybackState::Paused
                                             : PlaybackState::Playing);
        return;
    }
    if      (state_.load() == PlaybackState::Playing) pause();
    else if (state_.load() == PlaybackState::Paused)  resume();
}

void AudioManager::stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (cd_mode_.load()) {
        cd_source_.stop();
        cd_source_.close();
        cd_mode_.store(false);
        state_.store(PlaybackState::Stopped);
        if (device_initialised_) {
            ma_device_stop(&device_);
            ma_device_uninit(&device_);
            device_initialised_ = false;
        }
        track_ended_flag_.store(false); track_end_advanced_.store(false);  // don't advance playlist on manual stop
        return;
    }
    if (stream_mode_.load()) {
        endRecording();              // finalize an in-flight capture BEFORE close (R1);
                                     // its first act gates the tap off, so the
                                     // baseline close/mode order below is untouched
        stream_plugin_.close();
        stream_mode_.store(false);
        state_.store(PlaybackState::Stopped);
        if (device_initialised_) {
            ma_device_stop(&device_);
            ma_device_uninit(&device_);
            device_initialised_ = false;
        }
        track_ended_flag_.store(false); track_end_advanced_.store(false);
        return;
    }
    teardown();
    teardownNext();
    track_ended_flag_.store(false); track_end_advanced_.store(false);  // don't advance playlist on manual stop
    current_track_ = {};
}

// ─── seek ────────────────────────────────────────────────────────────────────
void AudioManager::seekTo(double seconds) {
    if (!file_src_) return;
    if (state_.load() == PlaybackState::Stopped) return;
    double dur = durationSec();
    if (seconds < 0.0) seconds = 0.0;
    if (dur > 0.0 && seconds > dur) seconds = dur;

    // Device/effect policy stays here; the decoder work — seek to ~180ms before
    // the target and decode-discard up to it (the MP3 bit-reservoir prime) —
    // moved verbatim into LocalFileSource::seekTo. Runs while the device is
    // stopped, so the few-ms extra decode is inaudible, exactly as before.
    bool was_playing = (state_.load() == PlaybackState::Playing);
    seeking_.store(true);
    if (was_playing && device_initialised_) ma_device_stop(&device_);
    file_src_->seekTo(seconds);
    resetSpeedResampler();  // drop residual from the pre-seek position
    if (was_playing && device_initialised_) ma_device_start(&device_);
    seeking_.store(false);
}

void AudioManager::seekBy(double delta) {
    if (cd_mode_.load()) {
        // (int casts: CDSource position/duration are whole seconds widened to
        // double per core::ISource — slice A signature harmonization only.)
        int new_pos = std::clamp((int)cd_source_.positionSec() + (int)delta, 0,
                                 (int)cd_source_.durationSec());
        cd_source_.seekTo(new_pos);
        cd_bpm_reset_.store(true);   // discontinuity — restart the BPM window
        return;
    }
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
    // Cursor math moved verbatim into LocalFileSource::positionSec. Called from
    // the UI thread and the audio callback, as before; the retirement scheme in
    // initCrossfade guarantees a UI-thread call racing the swap dereferences a
    // live object.
    return file_src_ ? file_src_->positionSec() : 0.0;
}

double AudioManager::durationSec() const {
    return cached_duration_.load();
}

// Displayed bitrate: TagLib's nominal/average — for VBR exactly as for CBR and
// lossless. The "live VBR estimate" this replaces was a linear position model
// ((pos/dur)·file_size, differentiated) whose steady-state derivative is
// constant BY CONSTRUCTION: the file average dressed as a live reading, made
// less accurate by tag/album-art bytes in file_size and spiking across seeks
// (the position jump landed in one 0.5 s delta window — probe evidence in
// roadmap's resolved entry, 2026-07-03). TagLib's number is both simpler and
// more accurate.
int AudioManager::bitrateKbps() const {
    if (current_track_.bitrate_kbps > 0)
        return current_track_.bitrate_kbps;
    // Tagless fallback: a one-shot static average from size/duration — no
    // rolling state, derived from immutable per-track fields.
    if (file_src_ && current_track_.file_size_bytes > 0 && current_track_.duration_sec > 0)
        return (int)((double)current_track_.file_size_bytes * 8.0
                     / ((double)current_track_.duration_sec * 1000.0));
    return current_track_.bitrate_kbps;   // 0 — the UI hides the field
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
    // Install a pending decoder swap FIRST: this is the only place current_track_
    // is updated for an auto-advance, and it must happen before startBpmDetection
    // (which reads current_track_.path) and before on_preload_next_ (which
    // overwrites next_track_info_). next_track_info_ is stable here: the audio
    // thread finished reading it before setting the flag, and next_decoder_
    // initialised_ is false post-swap so no preload can overwrite it until below.
    installPendingSwap();
    if (bpm_needed_flag_.exchange(false))
        startBpmDetection(current_track_.path,
                          file_src_ ? (int)file_src_->sampleRate() : 44100);
    if (cd_bpm_ready_.load(std::memory_order_acquire)) {
        // Snapshot the window before releasing the flag. While ready is set the
        // audio thread is frozen (see fill site), so this copy is clean; clearing
        // ready afterwards lets accumulation resume into cd_bpm_buf_ without racing
        // the detector, which now reads the UI-owned snapshot instead.
        cd_bpm_snapshot_.assign(cd_bpm_buf_.begin(), cd_bpm_buf_.begin() + CD_BPM_FRAMES);
        cd_bpm_ready_.store(false, std::memory_order_release);
        startBpmDetectionFromSamples(cd_bpm_snapshot_, CD_BPM_FRAMES, 44100);
    }
    if (preload_next_flag_.exchange(false))
        if (on_preload_next_) on_preload_next_();
    if (track_ended_flag_.exchange(false))
        if (on_track_end_) on_track_end_();
    pollStreamConnect();   // pick up a finished background stream connect
}

// ─── audio callback ──────────────────────────────────────────────────────────
void AudioManager::maDataCallback(ma_device* device, void* output,
                                   const void*, ma_uint32 frame_count) {
    static_cast<AudioManager*>(device->pUserData)->onDataCallback(output, frame_count);
}

// Streaming linear-interpolation resampler for tape-speed playback. Produces
// n_out output frames from n_out*speed source frames decoded from decoder_,
// carrying a residual buffer + fractional read position across callbacks so the
// phase is continuous (no per-buffer clicks). Returns MA_AT_END only when the
// source is exhausted mid-buffer (produced < n_out); the caller's existing
// track-done logic then handles gapless/silence exactly as for a 1:1 read.
ma_result AudioManager::readVarispeed(float* out, ma_uint32 n_out, ma_uint32 ch,
                                      float spd, ma_uint64& produced) {
    produced = 0;
    if (!file_src_ || ch == 0) return MA_AT_END;

    // Source frames needed to cover all outputs (+1 frame of interpolation
    // lookahead, +1 slack so an integer-aligned position never under-reads).
    int need = (int)std::floor(speed_pos_ + (double)n_out * (double)spd) + 2;
    if (need < 1) need = 1;
    if ((int)speed_res_.size() < need * (int)ch)
        speed_res_.resize((size_t)need * (size_t)ch);

    // Top up the residual with freshly decoded source frames until we have `need`
    // or the decoder runs dry.
    bool eof = false;
    while (speed_res_frames_ < need) {
        // (slice B: the read goes through the source; got==0 is the EOF signal.
        // A final got>0 read that also hit EOF now takes one extra 0-frame call
        // to detect — same frames delivered, same final state.)
        uint32_t got = file_src_->readFrames(
            &speed_res_[(size_t)speed_res_frames_ * ch],
            (uint32_t)(need - speed_res_frames_));
        speed_res_frames_ += (int)got;
        if (got == 0) { eof = true; break; }
    }

    // Resample residual -> out by linear interpolation at fractional positions.
    for (; produced < n_out; ++produced) {
        double pos = speed_pos_ + (double)produced * (double)spd;
        int i0 = (int)pos;
        if (i0 + 1 >= speed_res_frames_) break;     // source exhausted: EOF tail
        float fr = (float)(pos - (double)i0);
        const float* f0 = &speed_res_[(size_t)i0 * ch];
        const float* f1 = &speed_res_[(size_t)(i0 + 1) * ch];
        for (ma_uint32 c = 0; c < ch; ++c)
            out[(size_t)produced * ch + c] = f0[c] + (f1[c] - f0[c]) * fr;
    }

    // Advance the continuous read position, then drop fully-consumed leading
    // frames so the residual stays tiny and phase carries into the next callback.
    speed_pos_ += (double)produced * (double)spd;
    int drop = (int)speed_pos_;
    if (drop > speed_res_frames_) drop = speed_res_frames_;
    if (drop > 0) {
        int remain = speed_res_frames_ - drop;
        if (remain > 0)
            std::memmove(speed_res_.data(), &speed_res_[(size_t)drop * ch],
                         (size_t)remain * ch * sizeof(float));
        speed_res_frames_ = remain;
        speed_pos_       -= (double)drop;
    }

    return (eof && produced < n_out) ? MA_AT_END : MA_SUCCESS;
}

void AudioManager::onDataCallback(void* output, ma_uint32 frame_count) {
    const ma_uint32 ch = device_.playback.channels;
    if (!file_src_ && !cd_mode_.load() && !stream_mode_.load()) {
        if (ch > 0) std::memset(output, 0, frame_count * ch * sizeof(float));
        return;
    }

    // ── Stream mode: read from StreamSource ring buffer ──────────────────
    if (stream_mode_.load()) {
        float* out = static_cast<float*>(output);
        stream_plugin_.readFrames(out, frame_count);  // 44100/stereo; silence while buffering

        // Stream-capture tap (stream-record R1): BEFORE volume/balance/EQ/mute
        // so the recording is the undecorated broadcast — the same doctrine as
        // the CD live-BPM tap below. Inline no-op unless armed; when armed, one
        // bounded copy into the recorder's SPSC ring (no lock/alloc/IO — see
        // StreamRecorder::push).
        stream_recorder_.capture(out, frame_count);

        // abi-cluster keep-draining: while paused-and-recording, a capable
        // plugin keeps delivering REAL frames (one truth) — the tap above got
        // them; playback goes silent HERE (the second view). ORDER IS THE
        // CONTRACT: this memset must stay AFTER capture() or the file gets the
        // silence. With an old plugin readFrames already delivered silence and
        // this is a harmless double-zero. Two relaxed atomics — no new
        // blocking work on the audio thread; volume/EQ run on zeros and the
        // viz flatlines exactly as a pause always looked.
        if (stream_recorder_.recording() &&
            state_.load(std::memory_order_relaxed) == PlaybackState::Paused) {
            std::memset(out, 0, (size_t)frame_count * 2 * sizeof(float));
        }

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
        if (eq_enabled_.load()) {
            if (eq_dirty_) rebuildEqCoeffs();
            applyEq(out, frame_count, 2, 44100.0f);
        }
        for (ma_uint32 f = 0; f < frame_count; ++f)
            pushVizSample((out[f*2] + out[f*2+1]) * 0.5f);
        if (muted_.load())
            std::memset(out, 0, frame_count * 2 * sizeof(float));
        return;
    }

    // ── CD mode: read directly from CDSource ring buffer ─────────────────
    if (cd_mode_.load()) {
        float* out = static_cast<float*>(output);
        cd_source_.readFrames(out, frame_count);

        // ── Live BPM accumulation from RAW audio (before any effect) ─────────
        // Matches the file detector, which analyses the undecorated signal — EQ
        // would otherwise reshape the energy envelope and bias the tempo estimate.
        // A seek (FF/RW) restarts the window via cd_bpm_reset_ so the buffer can
        // never splice two non-contiguous sections of the track together.
        {
            const int cur = cd_source_.currentTrack();
            bool reset = (cur != cd_bpm_track_) || cd_bpm_reset_.exchange(false);
            if (reset) {
                cd_bpm_track_ = cur;
                cd_bpm_fill_.store(0,  std::memory_order_relaxed);
                cd_bpm_ready_.store(false, std::memory_order_relaxed);
                live_bpm_.store(0);
            }
            // Freeze accumulation while a full window is awaiting UI consumption
            // (cd_bpm_ready_): the UI thread copies cd_bpm_buf_ out before clearing
            // the flag, so not writing here keeps that copy from tearing.
            if (cur > 0 && !cd_source_.paused() &&
                !cd_bpm_ready_.load(std::memory_order_acquire)) {
                int bf = cd_bpm_fill_.load(std::memory_order_relaxed);
                for (ma_uint32 f = 0; f < frame_count && bf < CD_BPM_FRAMES; ++f)
                    cd_bpm_buf_[(size_t)bf++] = (out[f*2] + out[f*2+1]) * 0.5f;
                cd_bpm_fill_.store(bf, std::memory_order_release);
                if (bf >= CD_BPM_FRAMES)
                    cd_bpm_ready_.store(true, std::memory_order_release);
            }
        }

        // Apply volume, balance and EQ to the CD output. Process unconditionally
        // (even when muted) so the visualizer is fed real audio and the EQ biquad
        // state stays continuous across a mute toggle. Mute is applied last.
        {
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
            // EQ — CD is always 44100Hz stereo. Rebuild coeffs when dirty, exactly
            // like the file path: without this the coeffs stay at their unity default
            // and applyEq() skips every band, so EQ never affects CD output.
            if (eq_enabled_.load()) {
                if (eq_dirty_) rebuildEqCoeffs();
                applyEq(out, frame_count, 2, 44100.0f);
            }
        }
        // Feed the visualizer from the processed output, BEFORE mute — mirrors the
        // file path so the display stays alive while muted. CD is always 2ch.
        for (ma_uint32 f = 0; f < frame_count; ++f)
            pushVizSample((out[f*2] + out[f*2+1]) * 0.5f);
        // Mute last: zeroes only the audible output; the viz already has its copy.
        if (muted_.load())
            std::memset(out, 0, frame_count * 2 * sizeof(float));
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

    float* out    = static_cast<float*>(output);
    const ma_uint32 sr = device_.sampleRate;

    if (!file_src_ || ch == 0 || sr == 0) {
        std::memset(output, 0, frame_count * std::max(ch, 2u) * sizeof(float));
        return;
    }

    // ── Crossfade trigger ────────────────────────────────────────────────────
    // Suppressed while varispeed is active (speed != 1.0): the two effects both
    // consume from the decoder, and tape-speed near a track boundary is an edge
    // not worth the extra accounting. A crossfade already in flight finishes via
    // the normal (non-varispeed) read path below.
    // Suppressed under repeat-one for the same reason the swap is declined below:
    // the loop must not sound the armed track at all. Deciding this only at
    // initCrossfade() was too late - by then the fade had already run to full gain,
    // so the next track was audible for the whole crossfade before being declined.
    if (crossfade_secs > 0.0f && next_decoder_initialised_.load() && !crossfading_.load()
        && !repeat_one_.load() && speed_.load() == 1.0f) {
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
    float spd = speed_.load();
    bool use_varispeed = (spd != 1.0f && frame_count > 1 && !crossfading_.load());
    try {
        if (use_varispeed) {
            // Varispeed read: produce frame_count outputs from frame_count*spd
            // source frames, advancing the decoder at the sped-up/down rate.
            result = readVarispeed(out, frame_count, ch, spd, frames_read_a);
        } else {
            // Normal 1:1 read. If we just left varispeed, drop any half-consumed
            // residual so we neither replay nor skip a few source frames.
            if (speed_res_frames_ != 0 || speed_pos_ != 0.0) resetSpeedResampler();
            frames_read_a = file_src_->readFrames(out, frame_count);
        }
    } catch (...) {
        std::memset(out, 0, frame_count * ch * sizeof(float));
        track_ended_flag_.store(true);
        return;
    }

    bool track_done = (result == MA_AT_END || frames_read_a < frame_count);

    if (crossfading_.load() && next_decoder_initialised_.load()) {
        // Mix next track in, ramping up while current ramps down. Use a member
        // buffer grown on demand (steady-state frame_count is constant, so this is
        // a no-op after the first crossfade) instead of a per-callback heap alloc.
        const size_t need = (size_t)frame_count * ch;
        if (xfade_buf_.size() < need) xfade_buf_.resize(need);
        float* tmp = xfade_buf_.data();

        ma_uint64 frames_read_b = 0;
        try {
            frames_read_b = next_src_->readFrames(tmp, frame_count);
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
        // Gapless: if next is preloaded, switch immediately with no gap.
        // Repeat-one is excluded so the tail-fill below cannot sound the armed
        // track either; it falls to the silence-fill else, which sets
        // track_ended_flag_ and lets the normal repeat-one replay (and the queue
        // drain that shares that callback) run.
        if (next_decoder_initialised_.load() && !repeat_one_.load()) {
            // Fill remainder from next decoder
            ma_uint32 remaining = frame_count - (ma_uint32)frames_read_a;
            if (remaining > 0) {
                ma_uint64 fr = 0;
                fr = next_src_->readFrames(out + frames_read_a * ch, remaining);
                if (fr < remaining)
                    std::memset(out + (frames_read_a + fr) * ch, 0,
                        (remaining - fr) * ch * sizeof(float));
            }
            initCrossfade();
            track_end_advanced_.store(true);   // swap done - callback accounts, must NOT restart
            track_ended_flag_.store(true);
        } else {
            // No next track preloaded — silence remainder
            if (frames_read_a < frame_count)
                std::memset(out + frames_read_a * ch, 0,
                    (frame_count - (ma_uint32)frames_read_a) * ch * sizeof(float));
            track_end_advanced_.store(false);  // ended into silence - callback starts the next
            track_ended_flag_.store(true);
        }
    }

    // ── ReplayGain ────────────────────────────────────────────────────────
    if (replaygain_enabled_.load()) {
        float db   = current_rg_db_.load();   // atomic mirror — never touch current_track_ here
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

    // Feed visualizer from the processed output (ReplayGain/balance/EQ applied),
    // but BEFORE mute so the display stays alive when muted.
    for (ma_uint32 f = 0; f < frame_count; ++f) {
        float mono = 0.0f;
        for (ma_uint32 c = 0; c < ch; ++c)
            mono += out[(ma_uint64)f * ch + c];
        pushVizSample(mono / (float)ch);
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
    live_bpm_.store(0);

    if (path.empty()) return;

    bpm_thread_ = std::thread([this, path, sample_rate]() {
        int bpm = detectBpm(path, sample_rate, bpm_cancel_);
        if (!bpm_cancel_.load())
            live_bpm_.store(bpm);
    });
}

// CD path: run the autocorrelation detector over a window of live playback audio.
// The window is copied first so the worker is independent of the live accumulation
// buffer, which the audio thread may reset on the next CD track.
void AudioManager::startBpmDetectionFromSamples(const std::vector<float>& mono,
                                                int n, int sample_rate) {
    bpm_cancel_.store(true);
    if (bpm_thread_.joinable()) bpm_thread_.join();
    bpm_cancel_.store(false);
    live_bpm_.store(0);

    int cnt = std::min(n, (int)mono.size());
    if (cnt <= 0) return;
    std::vector<float> snap(mono.begin(), mono.begin() + cnt);
    bpm_thread_ = std::thread([this, snap = std::move(snap), sample_rate]() {
        int bpm = detectBpmFromSamples(snap, sample_rate, bpm_cancel_);
        if (!bpm_cancel_.load())
            live_bpm_.store(bpm);
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
    size_t nbackends = 0;
    cfg.ppCustomBackendVTables = remoct_custom_backends(&nbackends);
    cfg.customBackendCount     = (ma_uint32)nbackends;
    cfg.pCustomBackendUserData = nullptr;
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
    raw.resize((size_t)frames_read);
    return detectBpmFromSamples(raw, sr, cancel);
}

// Pure autocorrelation tempo estimate over a decoded mono buffer. Shared by the
// file path (decoded just above) and the live CD path (accumulated during
// playback), so both routes use identical maths.
int AudioManager::detectBpmFromSamples(const std::vector<float>& raw, int sr,
                                       const std::atomic<bool>& cancel) {
    if (cancel.load()) return 0;
    const ma_uint64 frames_read = raw.size();
    if (frames_read < (ma_uint64)(sr * 2)) return 0;

    // Downsample to ~100 Hz by computing RMS energy in ~10ms windows
    const int win = sr / 100;
    if (win <= 0) return 0;
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

    // Autocorrelation, weighted by a perceptual tempo prior.
    // Raw autocorrelation is sub-harmonic ambiguous: a pulse near ~110 BPM also
    // produces strong peaks at its 1/2 and 2/3 sub-multiples (~55, ~73), and tiny
    // input differences decide which one "wins" — that is exactly why the same
    // track landed on 72 from the file but 109 from the CD. Listeners feel tempo
    // around ~120 BPM, so we bias peak selection toward that range with a gentle
    // log-Gaussian weight: a clearly stronger peak elsewhere still wins, but the
    // fundamental pulse is preferred over a sub-harmonic, and the two paths agree.
    const float env_sr   = (float)sr / win;
    const int   lag_min  = (int)(env_sr * 60.0f / 200.0f);  // 200 BPM
    const int   lag_max  = (int)(env_sr * 60.0f / 50.0f);   // 50 BPM
    if (lag_min <= 0 || lag_max >= n_env) return 0;

    auto tempoWeight = [](float bpm) {
        constexpr float center = 120.0f;  // perceptual resonance peak (BPM)
        constexpr float sigma  = 0.9f;    // spread in natural-log space (tunable)
        float x = std::log(bpm / center) / sigma;
        return std::exp(-0.5f * x * x);
    };

    int    best_lag   = lag_min;
    double best_score = -1e30;

    for (int lag = lag_min; lag <= lag_max; ++lag) {
        if (cancel.load()) return 0;
        double acc = 0.0;
        int    cnt = n_env - lag;
        for (int i = 0; i < cnt; ++i)
            acc += (double)env[i] * env[i + lag];
        acc /= cnt;
        double score = acc * (double)tempoWeight(60.0f * env_sr / (float)lag);
        if (score > best_score) {
            best_score = score;
            best_lag   = lag;
        }
    }

    // Convert lag back to BPM
    float period_secs = best_lag / env_sr;
    int bpm = (int)std::round(60.0f / period_secs);

    // Sanity clamp
    return std::clamp(bpm, 50, 200);
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
    // Audio-thread only (called from onDataCallback when eq_dirty_). Reads the
    // atomic gains; no lock needed since coeffs/state are audio-thread-private.
    float sr = file_src_ ? (float)file_src_->sampleRate() : 44100.0f;
    for (int b = 0; b < EQ_BANDS; ++b)
        eq_coeffs_[b] = makePeakingEq(EQ_FREQS[b], eq_gains_db_[b].load(std::memory_order_relaxed), EQ_Q, sr);
    // Reset state to avoid pops when coefficients change
    for (int b = 0; b < EQ_BANDS; ++b)
        for (int c = 0; c < 2; ++c)
            eq_state_[b][c] = {};
    eq_dirty_ = false;
}

void AudioManager::applyEq(float* out, ma_uint32 frames,
                            ma_uint32 channels, float /*sr*/) {
    // Lock-free: eq_coeffs_/eq_state_ are written only by rebuildEqCoeffs on this
    // same (audio) thread, immediately before this call.
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
    eq_gains_db_[band].store(db, std::memory_order_relaxed);
    eq_dirty_ = true;   // audio thread rebuilds coeffs on its next pass
}

float AudioManager::getEqGain(int band) const {
    if (band < 0 || band >= EQ_BANDS) return 0.0f;
    return eq_gains_db_[band].load(std::memory_order_relaxed);
}

void AudioManager::resetEq() {
    for (int b = 0; b < EQ_BANDS; ++b)
        eq_gains_db_[b].store(0.0f, std::memory_order_relaxed);
    eq_dirty_ = true;
}


// ─── CD Audio mode ────────────────────────────────────────────────────────────
bool AudioManager::openCD(const std::string& drive_letter) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Exit stream mode if active before entering CD mode.
    if (stream_mode_.load()) {
        stream_mode_.store(false);
        endRecording();            // finalize an in-flight capture BEFORE close (R1)
        stream_plugin_.close();
    }
    // teardown handles already-stopped device safely
    teardown();
    teardownNext();
    track_ended_flag_.store(false); track_end_advanced_.store(false);
    // Entering CD mode means no file is the current track. Clear it so a stale
    // file identity can't leak into the UI while a CD plays - nowPlayingRow()
    // would resolve the old file's playlist row and the F3 follow-sync would
    // yank the cursor there on every CD track change (seen on CD->file->CD
    // round-trips once Slice 3 made mixed CD+file playlists persistent). Same
    // rationale, same fix as the radio path (startStreamConnectLocked).
    current_track_ = {};
    if (!cd_source_.open(drive_letter)) return false;
    cd_mode_.store(true);
    // Live-BPM accumulation window (mono, 44100 Hz) — sized once per CD session.
    cd_bpm_buf_.assign(CD_BPM_FRAMES, 0.0f);
    cd_bpm_fill_.store(0);
    cd_bpm_ready_.store(false);
    cd_bpm_reset_.store(false);
    cd_bpm_track_ = -1;
    // Init device for 44100Hz stereo float (Red Book format). Extracted to
    // initCDDevice() (Slice 3) so lazy reopen re-inits it identically.
    if (!initCDDevice()) {
        cd_source_.close();
        cd_mode_.store(false);
        return false;
    }
    return true;
}

// Init the CD playback device. Caller holds state_mutex_ and owns cd_mode_ +
// failure cleanup (see openCD). Returns false on ma_device_init failure.
bool AudioManager::initCDDevice() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 44100;
    cfg.dataCallback      = &AudioManager::maDataCallback;
    cfg.stopCallback      = &AudioManager::maStopCallback;
    cfg.pUserData         = this;
    if (has_selected_device_) cfg.playback.pDeviceID = &selected_device_id_;
    if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS) return false;
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
    state_.store(PlaybackState::Stopped);
    if (device_initialised_) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        device_initialised_ = false;
    }
}

// Non-blocking radio start. If a connect is already running, remember the latest
// request (depth-1, latest-wins) instead of racing a second worker.
void AudioManager::beginStream(const std::string& url) {
    // Graceful failure (slice c): a missing/incompatible plugin means no streaming.
    // PluginSource::open() would no-op to false anyway, but guard early so we never
    // spawn a connect worker, and surface a failure the UI can toast. (UIManager
    // also preflights streamPluginReady() for the specific reason.)
    if (!stream_plugin_.valid()) {
        Log::write("stream", streamPluginError() + " — ignoring beginStream");
        stream_just_failed_.store(true);
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (stream_connecting_.load()) {
        std::lock_guard<std::mutex> lk(stream_connect_mtx_);
        stream_pending_url_ = url;
        stream_pending_has_ = true;
        return;
    }
    startStreamConnectLocked(url);
}

// Stop current playback and spawn the connect worker. Caller holds state_mutex_.
void AudioManager::startStreamConnectLocked(const std::string& url) {
    // Stop whatever is playing so its callback releases the device before the
    // worker touches stream_plugin_. (stream_mode_ is cleared first, so a prior
    // stream's callback stops entering the stream branch.)
    if (cd_mode_.load()) { cd_source_.stop(); cd_source_.close(); cd_mode_.store(false); }
    endRecording();            // a station switch ends the old stream's capture (R1);
                               // no-op when idle. The connect-fail/superseded close
                               // sites need no hook: their stream never played, so
                               // nothing can be recording it (arming needs streamMode).
    stream_mode_.store(false);
    teardown();        // stops + uninits device_ and file decoder_
    teardownNext();
    track_ended_flag_.store(false); track_end_advanced_.store(false);
    state_.store(PlaybackState::Stopped);
    // Starting a stream means no file is the current track. Clear it so a stale
    // file identity (e.g. a just-drained override-queue song) can't drive the
    // file toast or the playlist play-indicator across a queue-drain -> radio
    // advance. Safe: startStreamConnectLocked only runs on the main thread
    // (pollEvents auto-advance, or UIManager radio-select / Ctrl+K).
    current_track_ = {};

    uint64_t gen = stream_connect_gen_.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lk(stream_connect_mtx_);
        stream_connect_url_        = url;
        stream_connect_worker_gen_ = gen;
    }
    stream_connect_done_.store(false);
    stream_connecting_.store(true);

    // The prior worker (if any) has already finished by the time we get here
    // (stream_connecting_ was false), so this join is instant.
    if (stream_connect_thread_.joinable()) stream_connect_thread_.join();
    stream_connect_thread_ = std::thread([this, url]() {
        // The slow part — connect + producer spawn — entirely off the UI thread.
        // The worker touches only stream_plugin_; the device is brought up later
        // on the main thread once this succeeds.
        bool ok = stream_plugin_.open(url);
        {
            std::lock_guard<std::mutex> lk(stream_connect_mtx_);
            stream_connect_ok_ = ok;
        }
        stream_connect_done_.store(true);
    });
}

// Called every tick from pollEvents (main thread). Picks up a finished connect
// and brings the device up, discarding the result if a newer playback superseded it.
void AudioManager::pollStreamConnect() {
    if (!stream_connect_done_.exchange(false)) return;

    std::lock_guard<std::mutex> lock(state_mutex_);

    bool ok; uint64_t wgen;
    {
        std::lock_guard<std::mutex> lk(stream_connect_mtx_);
        ok = stream_connect_ok_;
        wgen = stream_connect_worker_gen_;
    }
    if (stream_connect_thread_.joinable()) stream_connect_thread_.join();  // reap finished worker

    const bool superseded = (wgen != stream_connect_gen_.load());

    if (superseded) {
        // A file/CD/newer stream started while we were connecting — discard this
        // stream without touching the device or state the new playback now owns.
        stream_plugin_.close();
    } else if (!ok) {
        stream_plugin_.close();
        stream_mode_.store(false);
        state_.store(PlaybackState::Stopped);
        stream_just_failed_.store(true);
    } else {
        // Publish: bring up the device on the main thread now that the stream is open.
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate        = 44100;
        cfg.dataCallback      = &AudioManager::maDataCallback;
        cfg.stopCallback      = &AudioManager::maStopCallback;
        cfg.pUserData         = this;
        if (has_selected_device_) cfg.playback.pDeviceID = &selected_device_id_;
        if (ma_device_init(nullptr, &cfg, &device_) == MA_SUCCESS) {
            device_initialised_ = true;
            stream_mode_.store(true);
            ma_device_set_master_volume(&device_, volume_.load());
            ma_device_start(&device_);
            state_.store(PlaybackState::Playing);
            stream_just_connected_.store(true);
        } else {
            stream_plugin_.close();
            stream_mode_.store(false);
            state_.store(PlaybackState::Stopped);
            stream_just_failed_.store(true);
        }
    }

    stream_connecting_.store(false);

    // Latest-wins: honor a station selected during the connect.
    bool has_pending = false; std::string pending;
    {
        std::lock_guard<std::mutex> lk(stream_connect_mtx_);
        has_pending = stream_pending_has_;
        pending = stream_pending_url_;
        stream_pending_has_ = false;
    }
    if (has_pending) startStreamConnectLocked(pending);
}

bool AudioManager::playCDTrack(int track_number) {
    if (!cd_mode_.load() || !cd_source_.isOpen()) return false;
    stream_connect_gen_.fetch_add(1);   // supersede any in-flight stream connect
    bool ok = cd_source_.playTrack(track_number);
    if (ok) state_.store(PlaybackState::Playing);   // viz / BPM / UI gate on state_
    return ok;
}
const std::vector<CDTrack>& AudioManager::cdTracks() const {
    return cd_source_.tracks();
}

int AudioManager::cdPositionSec()  const { return (int)cd_source_.positionSec(); }
int AudioManager::cdDurationSec()  const { return (int)cd_source_.durationSec(); }
int AudioManager::cdCurrentTrack() const { return cd_source_.currentTrack(); }
