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
#ifdef _WIN32
#include "CDSource.h"
#include "StreamSource.h"
#endif

enum class PlaybackState { Stopped, Playing, Paused };

struct TrackInfo {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    int  duration_sec = 0;
    int  bitrate_kbps = 0;
    int  sample_rate  = 0;
    int  channels     = 0;
    std::string genre;
    std::string comment;
    int  year         = 0;
    int  track_num    = 0;
    int  bpm          = 0;   // 0 = not yet detected
    float replaygain_db = 0.0f;
    std::uintmax_t file_size_bytes = 0;
};

class AudioManager {
public:
    using TrackEndCallback = std::function<void()>;

    static constexpr int VIZ_BUF_SIZE = 2048;

    // Crossfade duration in seconds (0 = gapless/instant, default 2s)
    float crossfade_secs = 2.0f;

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
    int              liveBitrateKbps() const;
    const TrackInfo& currentTrack() const { return current_track_; }
    int              currentBpm()   const { return live_bpm_.load(); }

    // Visualizer
    int copySamples(float* dst, int n) const;

    void pollEvents();
    void setTrackEndCallback(TrackEndCallback cb)    { on_track_end_    = std::move(cb); }
    void setPreloadNextCallback(TrackEndCallback cb) { on_preload_next_ = std::move(cb); }
    void signalTrackEnd() { track_ended_flag_.store(true); }
    void clearTrackEnd()  { track_ended_flag_.store(false); }
    bool pollPreloadNext() { return preload_next_flag_.exchange(false); }

#ifdef _WIN32
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
    void     setPreferDigital(bool b) { stream_source_.setPreferDigital(b); }
    // True while a stream connect is being negotiated off the UI thread.
    bool     streamConnecting()  const { return stream_connecting_.load(); }
    // Non-blocking radio start: stops current playback, negotiates the stream on
    // a worker thread, and brings up the device on a later pollEvents() tick.
    void     beginStream(const std::string& url);
    // UI consumes these once to toast the outcome of a backgrounded connect.
    bool     takeStreamConnected() { return stream_just_connected_.exchange(false); }
    bool     takeStreamFailed()    { return stream_just_failed_.exchange(false); }
    bool     streamBuffering()   const { return stream_source_.buffering(); }
    std::string streamNowPlaying() const { return stream_source_.nowPlaying(); }
    std::string streamArtUrl()     const { return stream_source_.currentArtUrl(); } // iHeart digital cover ("" -> use logo)
    std::string streamUrl()      const { return stream_source_.url(); }   // URL actually streaming
    int      streamPositionSec() const { return (int)stream_source_.positionSec(); }
#endif

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
    // ── Primary decoder + device ──────────────────────────────────────────
    ma_decoder  decoder_ {};
    ma_device   device_  {};
    bool decoder_initialised_ = false;
    bool device_initialised_  = false;

    // ── Crossfade / gapless second decoder ───────────────────────────────
    // next_decoder_ is loaded while current track is still playing.
    // When crossfade begins (or track ends for gapless), we swap them.
    ma_decoder  next_decoder_  {};
    std::atomic<bool> next_decoder_initialised_ { false };  // release on write, acquire on read
    std::string next_path_;

    // Crossfade state (written on UI thread, read on audio thread)
    std::atomic<bool>  crossfading_  { false };
    std::atomic<float> xfade_pos_    { 0.0f };  // 0=start, 1=complete
    float              xfade_step_   { 0.0f };  // per-frame increment

    void initCrossfade();    // swap decoders, start fade
    void teardownNext();     // discard preloaded decoder

    std::atomic<PlaybackState> state_ { PlaybackState::Stopped };
    std::atomic<bool>          track_ended_flag_ { false };
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
#ifdef _WIN32
    std::atomic<bool>          cd_mode_  { false };
    CDSource                   cd_source_;
    std::atomic<bool>          stream_mode_  { false };
    StreamSource               stream_source_;

    // ── Async stream connect (StreamSource::open runs off the UI thread) ──
    // beginStream() stops current playback and spawns a worker that runs the slow
    // open() (connect + producer spawn); pollStreamConnect() (called each tick from
    // pollEvents) brings up the device on success. A single worker ever touches
    // stream_source_; a depth-1 "latest-wins" slot honors a station picked mid-connect;
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
#endif
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

    // liveBitrateKbps() rolling VBR estimate — per-instance, reset each play().
    // Previously function-local statics: shared across instances and never reset
    // on track change, so the first reading after a switch was stale.
    mutable double  lbr_prev_file_pos_ = 0.0;
    mutable std::chrono::steady_clock::time_point lbr_prev_time_ {};
    mutable int     lbr_cached_kbps_   = 0;

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