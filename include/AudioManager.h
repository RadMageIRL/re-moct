#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <functional>
#include <array>
#include <cstdint>
#include <thread>
#include <algorithm>

#include "miniaudio.h"
#ifdef _WIN32
#include "CDSource.h"
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
#ifdef _WIN32
    std::atomic<bool>          cd_mode_  { false };
    CDSource                   cd_source_;
#endif
    std::atomic<bool>          replaygain_enabled_ { false };
    std::atomic<float>         replaygain_gain_    { 1.0f };

    // EQ biquad state (one per band per channel, max 2 ch)
    std::atomic<bool>  eq_enabled_ { false };
    struct BiquadState { float x1=0,x2=0,y1=0,y2=0; };
    struct BiquadCoeff { float a0=1,a1=0,a2=0,b1=0,b2=0; };
    // coefficients updated on UI thread, applied on audio thread
    // We use a simple flag to signal rebuild
    mutable std::mutex          eq_mutex_;
    float                       eq_gains_db_[EQ_BANDS] {};  // default all 0
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
    void                 startBpmDetection(const std::string& path, int sample_rate);
    static int           detectBpm(const std::string& path, int sample_rate,
                                   const std::atomic<bool>& cancel);

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
