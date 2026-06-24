#pragma once
#ifdef _WIN32

// ─── StreamSource ─────────────────────────────────────────────────────────────
// Plays an HTTP/HTTPS audio stream (internet radio) completely isolated from the
// existing ma_decoder file path and from CDSource. A single producer thread pulls
// encoded bytes over WinINet, decodes them via a callback-driven ma_decoder, and
// feeds an int16 SPSC ring buffer that the audio callback drains as float PCM —
// the exact shape CDSource uses, so AudioManager can later branch on it the same way.
//
// Step 1 scope: headless. No UI, no ICY metadata, MP3 only (forced backend).
// Resamples whatever the station sends to a fixed 44100/stereo output.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

#include "miniaudio.h"   // declarations only — MINIAUDIO_IMPLEMENTATION lives in AudioManager.cpp
#include <fdk-aac/aacdecoder_lib.h>

class StreamSource {
public:
    std::string nowPlaying() const;   // current ICY StreamTitle (empty if none)
    // Fixed output format — matches the CD playback device so AudioManager's
    // streaming branch can mirror its CD branch (44100 Hz, stereo, float out).
    static constexpr int SAMPLE_RATE   = 44100;
    static constexpr int CHANNELS      = 2;
    static constexpr int RING_SECONDS  = 12;                                 // jitter buffer depth (effective cushion ~half this; producer backpressures at RING_SIZE/2)
    static constexpr int PREBUFFER_SEC = 3;                                  // fill before audio flows (per-tune-in latency; keep modest)
    static constexpr int RING_SIZE     = SAMPLE_RATE * CHANNELS * RING_SECONDS;
    static constexpr int PREBUFFER_SAMPLES = SAMPLE_RATE * CHANNELS * PREBUFFER_SEC;

    StreamSource() : ring_(RING_SIZE, 0) {}
    ~StreamSource() { close(); }

    StreamSource(const StreamSource&)            = delete;
    StreamSource& operator=(const StreamSource&) = delete;

    // Begin streaming the given URL. Returns false if the initial connection
    // can't be opened. Audio does not flow until PREBUFFER_SEC has accumulated.
    bool open(const std::string& url);
    void close();

    bool isOpen()     const { return producer_thread_.joinable(); }
    bool isPlaying()  const { return playing_.load(); }
    bool buffering()  const { return !prebuffered_.load(); }  // true while (re)filling
    void pause(bool p)      { paused_.store(p); }
    bool paused()     const { return paused_.load(); }

    // Seconds of wall-clock playback since the first sample left the ring.
    int  positionSec() const { return position_sec_.load(); }
    // Last error string (set on hard failure). Empty == none.
    std::string lastError() const { return last_error_; }

    // Audio-callback contract — identical to CDSource::readFrames.
    // Fills dst with frame_count stereo float frames; outputs silence while
    // paused, buffering, or on underrun.
    uint32_t readFrames(float* dst, uint32_t frame_count);

private:
    enum class Codec { MP3, AAC };
    Codec                   codec_ = Codec::MP3;   // chosen in open() from the URL

    std::string             url_;
    std::string             last_error_;

    // ICY de-interleaving state (producer thread)
    int                     icy_metaint_ = 0;   // bytes between metadata blocks (0 = none)
    int                     icy_counter_ = 0;   // audio bytes until next metadata block
    std::vector<uint8_t>    raw_buf_;           // buffered raw network bytes
    size_t                  raw_pos_     = 0;
    mutable std::mutex      now_playing_mtx_;
    std::string             now_playing_;

    // WinINet handles (owned by producer thread once open() returns)
    HINTERNET               hInet_ = nullptr;
    HINTERNET               hConn_ = nullptr;

    // MP3 decoder (driven on the producer thread only)
    ma_decoder              decoder_{};
    bool                    decoder_ready_ = false;

    // AAC (FDK) decoder + optional resampler — driven on the AAC producer thread only
    HANDLE_AACDECODER       aac_dec_   = nullptr;
    ma_data_converter       conv_{};
    bool                    conv_ready_   = false;
    int                     conv_in_rate_ = 0;
    int                     conv_in_ch_   = 0;

    // State
    std::atomic<bool>       playing_     { false };
    std::atomic<bool>       paused_      { false };
    std::atomic<bool>       stop_        { false };
    std::atomic<bool>       prebuffered_ { false };
    std::atomic<int>        position_sec_{ 0 };
    std::atomic<uint64_t>   frames_drained_{ 0 };  // for position; advanced by readFrames

    // int16 SPSC ring (single producer thread, single audio-callback consumer)
    std::vector<int16_t>    ring_;
    std::atomic<int>        ring_write_  { 0 };
    std::atomic<int>        ring_read_   { 0 };

    std::thread             producer_thread_;

    // ── producer / connection ──
    void producerWorker();      // MP3 path (miniaudio)
    void producerWorkerAAC();   // AAC path (FDK-AAC + optional resample)

    // ── ICY (Shoutcast/Icecast) inline metadata ──
    DWORD readAudio(void* out, DWORD want);    // ICY-aware read both decoders pull from
    DWORD rawRead(void* dst, DWORD want);      // buffered raw network bytes
    bool  rawReadExact(void* dst, DWORD n);
    void  parseIcyMetadata(const std::string& block);
    bool connect();          // (re)open WinINet handles for url_
    void disconnect();       // close WinINet handles
    bool initDecoder();      // ma_decoder_init over the read/seek callbacks
    void uninitDecoder();

    // ── miniaudio callbacks (0.11.x signatures) ──
    static ma_result onRead(ma_decoder* dec, void* out, size_t toRead, size_t* bytesRead);
    static ma_result onSeek(ma_decoder* dec, ma_int64 offset, ma_seek_origin origin);

    // ── ring helpers (mirror CDSource semantics exactly) ──
    int  ringAvailable() const;
    void ringWrite(const int16_t* data, int samples);
    int  ringRead(int16_t* dst, int samples);
};

#endif // _WIN32
