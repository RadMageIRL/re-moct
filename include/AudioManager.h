#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <functional>
#include <array>
#include <cstdint>
#include <thread>
#include <algorithm>
#include <vector>
#include <chrono>

#include "miniaudio.h"
#include "TrackInfo.h"        // hoisted from this header (slice B) — same struct, zero call-site changes
#include "LocalFileSource.h"  // the file path as a source object (slice B)
#include <memory>
#include "CDSource.h"
// Phase 4: the streaming source is driven through the plugin C ABI. The host holds
// a PluginSource (the driver) over the plugin descriptor, plus the HTTP service
// table it hands the plugin at create. Since slice (c) the descriptor comes from a
// LOADED .so/.dll (core::loadPlugin) instead of the compiled-in adapter — the
// host no longer references any streaming-source symbol.
#include "PluginSource.h"
#include "PluginHostServices.h"
#include "PluginHost.h"        // core::loadPlugin / LoadedPlugin / PluginLoad (slice c)
#include "StreamRecorder.h"    // stream-record R1: the host-side capture engine

enum class PlaybackState { Stopped, Playing, Paused };

class AudioManager {
public:
    using TrackEndCallback = std::function<void()>;

    static constexpr int VIZ_BUF_SIZE = 2048;

    // Crossfade duration in seconds (0 = gapless/instant - the default; the
    // product sets this from the "crossfade" config key at startup)
    float crossfade_secs = 0.0f;

    AudioManager();
    ~AudioManager();
    AudioManager(const AudioManager&)            = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // Playback control
    bool play(const std::string& path);
    void pause();
    void resume();
    void togglePause();
    void stop();

    // Seek
    void seekTo(double seconds);
    void seekBy(double delta_seconds);
    void setRepeatOne(bool on) { repeat_one_.store(on); }
    void clearNext();  // discard preloaded next track

    // True only while a crossfade is mixing (decoder_ is still the outgoing track,
    // advancing to its end). UI reads this to let the progress head complete instead
    // of snapping to the incoming track's 0% mid-overlap. Read-only; no side effects.
    bool isCrossfading() const { return crossfading_.load(std::memory_order_acquire); }

    // True while a next track is armed (preloadNext published, not yet consumed
    // by a swap). The UI-thread arm poll (NextArm.h) keys off this; it is the
    // ONLY armed-state read the UI gets - next_path_ is audio-thread-mutated, so
    // no string accessor exists on purpose.
    bool hasNextArmed() const { return next_decoder_initialised_.load(std::memory_order_acquire); }

    // Volume
    void  setVolume(float v);
    void  adjustVolume(float delta);
    float volume() const { return volume_.load(); }
    void  toggleMute() { muted_.store(!muted_.load()); }
    bool  muted()  const { return muted_.load(); }
    void  setBalance(float b) { balance_.store(std::clamp(b, -1.0f, 1.0f)); }
    float balance() const { return balance_.load(); }

    // Speed / pitch control (0.5 = half speed, 2.0 = double, 1.0 = normal)
    void  setSpeed(float s)    { speed_.store(std::clamp(s, 0.5f, 2.0f)); }
    void  adjustSpeed(float d) { setSpeed(speed_.load() + d); }
    float speed()  const { return speed_.load(); }

    // ReplayGain — apply track/album gain automatically
    void setReplayGain(bool on) { replaygain_enabled_.store(on); }
    bool replayGain() const     { return replaygain_enabled_.load(); }

    // 10-band graphic equalizer (±12 dB per band)
    // Bands: 31, 62, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz
    static constexpr int EQ_BANDS = 10;
    void  setEqGain(int band, float db);   // -12..+12 dB
    float getEqGain(int band) const;
    void  setEqEnabled(bool on) { eq_enabled_.store(on); }
    bool  eqEnabled()     const { return eq_enabled_.load(); }
    void  resetEq();

    // Query
    PlaybackState    state()        const { return state_.load(); }
    double           positionSec()  const;
    double           durationSec()  const;
    int              bitrateKbps() const;   // TagLib nominal/average (static — see .cpp)
    const TrackInfo& currentTrack() const { return current_track_; }
    int              currentBpm()   const { return live_bpm_.load(); }

    // Visualizer
    int copySamples(float* dst, int n) const;

    void pollEvents();
    void setTrackEndCallback(TrackEndCallback cb)    { on_track_end_    = std::move(cb); }
    void setPreloadNextCallback(TrackEndCallback cb) { on_preload_next_ = std::move(cb); }
    void signalTrackEnd() { track_ended_flag_.store(true); }
    void clearTrackEnd()  { track_ended_flag_.store(false); track_end_advanced_.store(false); }
    // One-shot: true when the track_end being handled was a gapless-splice
    // ADVANCE (the audio thread already swapped the armed track in; it is
    // playing). The track-end callback then only ACCOUNTS - pops the consumed
    // queue head or advances the index - and never restarts the track from
    // zero, which was an audible double-start. False = the track ended into
    // silence and the callback must actually start whatever plays next.
    bool takeTrackEndAdvanced() { return track_end_advanced_.exchange(false); }
    bool pollPreloadNext() { return preload_next_flag_.exchange(false); }

    // ── CD Audio mode ────────────────────────────────────────────────────────
    bool     openCD(const std::string& drive_letter);  // detect & load TOC
    void     closeCD();
    bool     cdMode()  const { return cd_mode_.load(); }
    bool     playCDTrack(int track_number);
    const std::vector<CDTrack>& cdTracks() const;
    int      cdPositionSec()  const;
    int      cdDurationSec()  const;
    int      cdCurrentTrack() const;
    const CDSource& cdSource() const { return cd_source_; }
          CDSource& cdSource()       { return cd_source_; }

    // ── Streaming (internet radio) mode ───────────────────────────────────────
    bool     streamMode()        const { return stream_mode_.load(); }
    // Prefer the iHeart digital (web-player) rendition on the next stream connect.
    void     setPreferDigital(bool b) { stream_plugin_.setPreferDigital(b); }
    // Deep-log toggle across the ABI (slice c): the host tracks the on/off state
    // (UIManager, Ctrl+A) and pushes it here; IHeartDeepLog now lives in the .so.
    void     setDeepLog(bool on) { stream_plugin_.setConfig("deeplog", on ? "1" : "0"); }
    // Identity A/B arm (probe): use a minted anonymous profileId on the digital
    // handshake (deep-log only). Same config-passthrough shape as setDeepLog.
    void     setProbeMinted(bool on) { stream_plugin_.setConfig("iheart_probe_minted", on ? "1" : "0"); }
    // iHeart re-pin behaviour (F6): 0 off / 1 on / 2 smart. Config-passthrough shape,
    // read live by the producer/SM so it takes effect without a reconnect.
    void     setRepinMode(int m) { stream_plugin_.setConfig("repin_mode", std::to_string(m).c_str()); }
    // Whether the streaming plugin loaded (slice c). false => streaming disabled
    // (missing/incompatible .so/.dll). beginStream() guards on this and surfaces a
    // failure toast; the UI may also preflight streamPluginError() for the specific
    // reason. Host stays alive either way.
    bool     streamPluginReady() const { return stream_plugin_.valid(); }
    std::string streamPluginError() const;   // "" when ready, else a human reason
    // True while a stream connect is being negotiated off the UI thread.
    bool     streamConnecting()  const { return stream_connecting_.load(); }
    // Non-blocking radio start: stops current playback, negotiates the stream on
    // a worker thread, and brings up the device on a later pollEvents() tick.
    void     beginStream(const std::string& url);
    // UI consumes these once to toast the outcome of a backgrounded connect.
    bool     takeStreamConnected() { return stream_just_connected_.exchange(false); }
    bool     takeStreamFailed()    { return stream_just_failed_.exchange(false); }
    bool     streamBuffering()   const { return stream_plugin_.buffering(); }
    std::string streamNowPlaying() const { return stream_plugin_.nowPlaying(); }
    std::string streamArtUrl()     const { return stream_plugin_.currentArtUrl(); } // iHeart digital cover ("" -> use logo)
    std::string streamUrl()      const { return stream_plugin_.url(); }   // URL actually streaming
    int      streamPositionSec() const { return (int)stream_plugin_.positionSec(); }

    // Stream capture (stream-record R1). The engine is host-side and headless
    // here — R2's [Rec] panel drives it through this accessor (start/stop/
    // onTitle/state); the audio callback taps into it in the stream branch.
    StreamRecorder& streamRecorder() { return stream_recorder_; }
    // abi-cluster slice A: recording start/stop go through these wrappers so
    // the keep-draining signal always travels WITH the recorder lifecycle
    // (set_record_active(1) on start, (0) on every stop path incl. teardown).
    // beginRecording's recorder result is authoritative; the drain ack is
    // best-effort (old plugin -> false -> the pause-gap note stays honest).
    bool beginRecording(const RecOptions& opt, const std::string& station,
                        const std::string& out_dir);
    void endRecording();
    bool recordingDrainSupported() const { return stream_plugin_.supportsRecordActive(); }
    // abi-cluster slice B: whether the plugin offers the copy tee (drives the
    // [Rec] panel's Copy row — greyed when false), and the live codec for its
    // quality column (REMOCT_CODEC_* numeric; 0 = none/unknown/not flowing).
    bool    recordingCopySupported() const { return stream_plugin_.supportsEncodedCapture(); }
    int32_t streamEncodedCaps()      const { return stream_plugin_.encodedCaps(); }

    // Called by track-end callback to pre-load next track for crossfade/gapless
    // Returns false if next track can't be opened
    bool preloadNext(const std::string& path);

    // Output device selection
    struct DeviceInfo { std::string name; ma_device_id id; };
    std::vector<DeviceInfo> enumerateDevices() const;
    void setDevice(const ma_device_id* id);   // nullptr = default
    int  selectedDeviceIndex() const { return selected_device_idx_; }
    void setSelectedDeviceIndex(int i) { selected_device_idx_ = i; }

private:
    // ── Primary source + device ───────────────────────────────────────────
    // file_src_ is the current local-file source (slice B: replaces the inline
    // ma_decoder decoder_ + decoder_initialised_; "initialised" == non-null).
    // Mutated on the main thread only in device-stopped windows — the same
    // synchronization the plain bool relied on — EXCEPT the one audio-thread
    // mutation: the initCrossfade swap.
    std::unique_ptr<LocalFileSource> file_src_;
    ma_device   device_  {};
    bool device_initialised_  = false;

    // Init the ma_device_ for CD playback (44100Hz stereo f32, Red Book). Returns
    // false on ma_device_init failure. Extracted from openCD() (Slice 3) so the
    // lazy-reopen path — which goes through openCD() after stop() tore the device
    // down — always re-inits it and never skips ma_device_ setup. Caller must hold
    // state_mutex_ and owns cd_mode_ / cleanup on failure.
    bool initCDDevice();

    // ── Crossfade / gapless second source ─────────────────────────────────
    // next_src_ is loaded while current track is still playing.
    // When crossfade begins (or track ends for gapless), we swap them.
    std::unique_ptr<LocalFileSource> next_src_;
    std::atomic<bool> next_decoder_initialised_ { false };  // release on write, acquire on read
    std::string next_path_;
    // The outgoing source after an audio-thread swap. The audio thread RETIRES
    // the old current here instead of freeing it; pollEvents reaps it on the
    // main thread under the existing track_swap_flag_ synchronizes-with edge,
    // so a UI-thread positionSec() racing the swap can only ever dereference a
    // live decoder (and the audio callback no longer frees heap).
    std::unique_ptr<LocalFileSource> retired_src_;

    // Crossfade state (written on UI thread, read on audio thread)
    std::atomic<bool>  crossfading_  { false };
    std::atomic<float> xfade_pos_    { 0.0f };  // 0=start, 1=complete
    float              xfade_step_   { 0.0f };  // per-frame increment

    void initCrossfade();    // swap decoders, start fade
    void teardownNext();     // discard preloaded decoder
    // Install a swap the audio thread has already performed. MAIN THREAD ONLY,
    // and acquires no lock: its two callers (pollEvents, teardownNext) are both
    // UI-thread, and teardownNext runs with state_mutex_ already held.
    void installPendingSwap();

    std::atomic<PlaybackState> state_ { PlaybackState::Stopped };
    std::atomic<bool>          track_ended_flag_ { false };
    std::atomic<bool>          track_end_advanced_ { false };  // splice-advance marker, see takeTrackEndAdvanced
    std::atomic<bool>          preload_next_flag_{ false }; // crossfade done — preload only
    std::atomic<bool>          bpm_needed_flag_  { false };
    std::atomic<bool>          seeking_ { false };
    std::atomic<bool>          repeat_one_ { false };
    std::atomic<float>         volume_   { 1.0f };
    std::atomic<bool>          muted_    { false };
    std::atomic<float>         balance_  { 0.0f };  // -1.0 = full left, 0 = center, 1.0 = full right
    std::atomic<double>        cached_duration_ { 0.0 };
    std::atomic<float>         speed_    { 1.0f };
    // Varispeed (tape-speed) streaming linear resampler — audio-thread-only state.
    // Replaces the old in-place buffer warp that never actually changed how many
    // source frames were consumed. Produces frame_count output frames from
    // frame_count*speed source frames, carrying a residual + fractional position
    // across callbacks so there are no per-buffer discontinuities.
    std::vector<float>         speed_res_;             // residual decoded source frames
    int                        speed_res_frames_ = 0;  // valid frames in speed_res_
    std::vector<float>         xfade_buf_;             // crossfade next-track scratch (cb-owned, lazy-grown)
    double                     speed_pos_        = 0.0; // fractional source read position
    void      resetSpeedResampler() { speed_res_frames_ = 0; speed_pos_ = 0.0; }
    ma_result readVarispeed(float* out, ma_uint32 frame_count, ma_uint32 channels,
                            float speed, ma_uint64& produced);
    std::atomic<bool>          cd_mode_  { false };
    CDSource                   cd_source_;
    std::atomic<bool>          stream_mode_  { false };
    // Absolute path of the streaming plugin next to the binary:
    // <exeDir>/plugins/remoct_stream.{dll,so} (slice c). Static — used by the
    // loaded_plugin_ default member initializer below.
    static std::string streamPluginPath();

    // The HTTP service table handed to the streaming plugin at create, backed by
    // core::http(). Declared BEFORE stream_plugin_ so it is constructed first
    // (the plugin instance is created with its table()). All three live as long as
    // AudioManager — the ABI service-lifetime contract.
    core::HostServices         host_services_{};
    // Acquisition flip (slice c): the streaming source is a LOADED .so/.dll now.
    // loaded_plugin_ owns the module + borrows the descriptor (RAII: module unloads
    // after the descriptor). A load failure (missing / ABI-mismatch / too-small —
    // slice-(a) reject paths, now in production) leaves loaded_plugin_ null and
    // plugin_load_result_ set; PluginSource(nullptr,...) is valid()==false, so every
    // op no-ops and the host stays alive. Declaration order = init order:
    // plugin_load_result_ (out-param) then loaded_plugin_ (the load) then
    // stream_plugin_ (built from the descriptor).
    core::PluginLoad                    plugin_load_result_ = core::PluginLoad::Ok;
    std::unique_ptr<core::LoadedPlugin> loaded_plugin_ =
        core::loadPlugin(streamPluginPath(), &plugin_load_result_);
    core::PluginSource         stream_plugin_{
        loaded_plugin_ ? loaded_plugin_->plugin() : nullptr,
        host_services_.table() };
    // Stream capture (stream-record R1): the recorder the callback taps into.
    // Inert (capturing_ false, no worker) until something calls start() — in R1
    // only the test/harness do; the [Rec] panel arrives in R2. Every stream-
    // teardown site stops it BEFORE stream_plugin_.close() so an in-flight
    // capture finalizes its current cut instead of truncating it.
    StreamRecorder             stream_recorder_;

    // ── Async stream connect (StreamSource::open runs off the UI thread) ──
    // beginStream() stops current playback and spawns a worker that runs the slow
    // open() (connect + producer spawn); pollStreamConnect() (called each tick from
    // pollEvents) brings up the device on success. A single worker ever touches
    // stream_plugin_; a depth-1 "latest-wins" slot honors a station picked mid-connect;
    // a generation counter lets a file/CD start interrupt and supersede a connect.
    std::atomic<bool>          stream_connecting_   { false }; // worker in flight
    std::atomic<bool>          stream_connect_done_ { false }; // worker finished; result pending
    std::atomic<uint64_t>      stream_connect_gen_  { 0 };     // bumped by every playback start
    std::atomic<bool>          stream_just_connected_{ false };// UI toast latch
    std::atomic<bool>          stream_just_failed_   { false };// UI toast latch
    std::mutex                 stream_connect_mtx_;            // guards the strings + flags below
    std::string                stream_connect_url_;            // url the worker is opening
    bool                       stream_connect_ok_ = false;     // worker result
    uint64_t                   stream_connect_worker_gen_ = 0; // gen captured when the worker spawned
    std::string                stream_pending_url_;            // latest-wins depth-1 slot
    bool                       stream_pending_has_ = false;
    std::thread                stream_connect_thread_;
    void startStreamConnectLocked(const std::string& url);     // state_mutex_ must be held
    void pollStreamConnect();                                  // main thread; from pollEvents
    std::atomic<bool>          replaygain_enabled_ { false };
    std::atomic<float>         replaygain_gain_    { 1.0f };
    // The audio callback must never read current_track_ (a struct of std::strings
    // that the main thread reassigns). The one field it needs — the track's
    // ReplayGain value — is mirrored here as a lock-free atomic, published by
    // play()/the deferred swap.
    std::atomic<float>         current_rg_db_ { 0.0f };
    // Set by initCrossfade() on the audio thread when it swaps decoders; consumed
    // by pollEvents() on the main thread, which is the ONLY writer of current_track_.
    std::atomic<bool>          track_swap_flag_ { false };

    // EQ biquad state (one per band per channel, max 2 ch)
    std::atomic<bool>  eq_enabled_ { false };
    struct BiquadState { float x1=0,x2=0,y1=0,y2=0; };
    struct BiquadCoeff { float a0=1,a1=0,a2=0,b1=0,b2=0; };
    // Gains are the only cross-thread EQ data (UI writes, audio reads at rebuild),
    // so they're atomic and the whole EQ path is lock-free. Coefficients and filter
    // state are touched ONLY on the audio thread: rebuildEqCoeffs() now runs solely
    // from the data callback when eq_dirty_ is set, immediately before applyEq().
    std::atomic<float>          eq_gains_db_[EQ_BANDS] {};  // default all 0
    BiquadCoeff                 eq_coeffs_[EQ_BANDS] {};
    BiquadState                 eq_state_[EQ_BANDS][2] {};  // [band][channel]
    std::atomic<bool>           eq_dirty_ { false };
    void rebuildEqCoeffs();
    void applyEq(float* out, ma_uint32 frames, ma_uint32 channels, float sample_rate);
    static BiquadCoeff makePeakingEq(float freq, float gain_db, float q, float sr);

    TrackInfo  current_track_;
    TrackInfo  next_track_info_;  // written by preloadNext (UI), read by initCrossfade (audio)
                                  // Safe: next_decoder_initialised_ atomic acts as release/acquire fence
    std::mutex state_mutex_;

    // BPM detection — runs on a background thread
    std::thread          bpm_thread_;
    std::atomic<bool>    bpm_cancel_  { false };
    // Detected BPM lives in its own atomic, NOT in current_track_.bpm: the BPM
    // worker thread must never write into current_track_ while the audio thread
    // reassigns that struct (initCrossfade) and the UI reads it. UI reads via
    // currentBpm().
    std::atomic<int>     live_bpm_    { 0 };
    void                 startBpmDetection(const std::string& path, int sample_rate);
    static int           detectBpm(const std::string& path, int sample_rate,
                                   const std::atomic<bool>& cancel);
    // Pure autocorrelation BPM over an already-decoded mono buffer — shared by the
    // file path (detectBpm decodes then calls this) and the CD path (live audio).
    static int           detectBpmFromSamples(const std::vector<float>& mono, int sr,
                                              const std::atomic<bool>& cancel);
    void                 startBpmDetectionFromSamples(const std::vector<float>& mono,
                                                      int n, int sample_rate);

    // Live BPM for CD playback: there is no file to analyse offline, so a window of
    // playback audio is accumulated in the data callback and the same detector is
    // run on it once the window fills. CD is always 44100 Hz.
    static constexpr int CD_BPM_WINDOW_SECS = 20;   // match detectBpm's file window
    static constexpr int CD_BPM_FRAMES      = 44100 * CD_BPM_WINDOW_SECS;
    std::vector<float>   cd_bpm_buf_;               // mono accumulation window
    std::vector<float>   cd_bpm_snapshot_;          // UI-owned copy taken before clearing ready
    std::atomic<int>     cd_bpm_fill_   { 0 };      // frames filled (audio thread)
    std::atomic<bool>    cd_bpm_ready_  { false };  // window full → detect on UI thread
    std::atomic<bool>    cd_bpm_reset_  { false };  // seek requested → restart window
    int                  cd_bpm_track_  { -1 };     // CD track being accumulated (cb-owned)

    mutable std::array<float, VIZ_BUF_SIZE> viz_buf_ {};
    std::atomic<int> viz_write_pos_ { 0 };

    TrackEndCallback on_track_end_;
    TrackEndCallback on_preload_next_;

    // Selected output device (nullptr = default)
    ma_device_id selected_device_id_ {};
    bool         has_selected_device_ = false;
    int          selected_device_idx_ = -1;  // -1 = default

    void teardown();
    bool initDevice();

    static void maDataCallback(ma_device*, void*, const void*, ma_uint32);
    void        onDataCallback(void* output, ma_uint32 frame_count);
    static void maStopCallback(ma_device*);

    // Mix helpers
    static float readMonoSample(const void* buf, ma_format fmt,
                                 ma_uint32 channels, ma_uint64 frame);
    void pushVizSample(float s);
};