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
#include <memory>
#include <deque>

#include "miniaudio.h"   // declarations only — MINIAUDIO_IMPLEMENTATION lives in AudioManager.cpp
#include <fdk-aac/aacdecoder_lib.h>
#include "IHeartRadio.h"  // isolated iHeart now-playing service (HLS streams only)
#include "IHeartNowPlayingSM.h"  // pure now-playing reconciliation state machine
#include "core/IHttp.h"   // HLS manifest/segment one-shots go through the seam (slice 4)

class StreamSource {
public:
    std::string nowPlaying() const;   // current ICY StreamTitle (empty if none)
    std::string currentArtUrl() const; // album-art URL for the committed song (digital iHeart only; "" otherwise)

    // ── HLS increment 1 (dev probe) ──────────────────────────────────────────
    // Standalone, no audio path: resolve master -> poll media playlist -> fetch
    // first segment, logging sizes + ADTS sync to %TEMP%\remoct-*.log ([stream]).
    // Call with the canonical zc####/hls.m3u8 URL. Remove once increment 2 wires
    // the pump into rawRead(). Returns true if the full chain resolved.
    bool hlsProbeTest(const std::string& master_url);
    // Fixed output format — matches the CD playback device so AudioManager's
    // streaming branch can mirror its CD branch (44100 Hz, stereo, float out).
    static constexpr int SAMPLE_RATE   = 44100;
    static constexpr int CHANNELS      = 2;
    static constexpr int RING_SECONDS  = 12;                                 // jitter buffer depth (effective cushion ~half this; producer backpressures at RING_SIZE/2)
    static constexpr int PREBUFFER_SEC = 3;                                  // fill before audio flows (per-tune-in latency; keep modest)
    static constexpr int RING_SIZE     = SAMPLE_RATE * CHANNELS * RING_SECONDS;
    static constexpr int PREBUFFER_SAMPLES = SAMPLE_RATE * CHANNELS * PREBUFFER_SEC;
    static constexpr int FADE_IN_FRAMES    = SAMPLE_RATE / 2;                 // re-pin fade-in (~0.5s); ramps the new song in instead of a hard cut

    StreamSource() : ring_(RING_SIZE, 0) {}
    // Staging-lane constructor: a second instance the coordinator owns to prebuffer
    // a parallel session. is_lane_ stops it recursing (no staging-of-staging) and
    // suppresses the one global side-effect it would otherwise touch (the deep log).
    explicit StreamSource(bool lane) : ring_(RING_SIZE, 0) { is_lane_ = lane; }
    ~StreamSource() { close(); }

    StreamSource(const StreamSource&)            = delete;
    StreamSource& operator=(const StreamSource&) = delete;

    // Begin streaming the given URL. Returns false if the initial connection
    // can't be opened. Audio does not flow until PREBUFFER_SEC has accumulated.
    bool open(const std::string& url);

    // Prefer the iHeart web-player (ad-reduced "digital") rendition on the next
    // connect; falls back to the raw broadcast if the handshake fails. Set before
    // open()/reconnect. Thread-safe.
    void setPreferDigital(bool b) { prefer_digital_.store(b); }
    void close();

    bool isOpen()     const { return producer_thread_.joinable(); }
    bool isPlaying()  const { return playing_.load(); }
    bool buffering()  const { return !prebuffered_.load(); }  // true while (re)filling
    bool isPrebuffered() const { return prebuffered_.load(); }   // staging-lane readiness probe
    bool edgeIsMusic()   const { return last_cls_.load() == 1; } // newest manifest seg is music (not ad)
    const char* edgeClsName() const {                            // human-readable edge class (for staging peek logs)
        switch (last_cls_.load(std::memory_order_relaxed)) {
            case 1: return "music"; case 2: return "ad"; case 3: return "other"; default: return "none";
        }
    }
    const std::string& url() const { return url_; }  // stream URL currently open ("" if none)
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

    // ── HLS state (increment 1: populated only by the probe; increment 2 wires
    //    it into the live producer via a rawRead() branch). Additive — none of
    //    the continuous-stream members above are affected. ───────────────────
    enum class Mode { Continuous, HLS };
    Mode                    mode_ = Mode::Continuous;   // set in open() (increment 2)

    struct HlsState {
        std::string master_url;               // canonical zc####/hls.m3u8 (persisted)
        std::string variant_url;              // tokenized media-playlist URL (session token lives here)
        uint64_t    last_seq      = 0;        // highest EXT-X-MEDIA-SEQUENCE consumed
        int         target_dur_ms = 10000;    // from EXT-X-TARGETDURATION
        std::vector<std::string> pending;     // segment URLs not yet fetched
        std::vector<uint8_t>     seg;         // current segment bytes (increment 2)
        size_t                   seg_pos = 0;
        DWORD                    last_poll = 0;// GetTickCount of last media poll
    };
    HlsState                hls_;

    bool hlsEnsureSession();                  // open hls_session_ (UA + timeouts) if needed
    bool hlsHttpGet(const std::string& url, std::string* out_text,
                    std::vector<uint8_t>* out_bytes, std::string* out_final_url);
    bool hlsResolveMaster();                  // GET master -> variant_url
    std::string hlsBuildDigitalUrl(const std::string& base);  // append web-player params -> digital rendition URL
    bool hlsPollMedia();                      // GET variant -> append new segments to pending
    bool hlsFetchSegment(const std::string& url, std::vector<uint8_t>& out);
    bool hlsConnect();                        // HLS branch of connect(): resolve + prime near live edge
    DWORD hlsRawRead(void* dst, DWORD want);  // HLS branch of rawRead(): the segment pump
    void hlsParseId3(const uint8_t* tag, size_t len);  // extract TIT2/TPE1 -> now_playing_

    // ── iHeart now-playing (HLS only) ────────────────────────────────────────
    // iHeart doesn't embed song info in-band (their ID3 is only the HLS timestamp
    // PRIV frame), so for iHeart streams we poll their JSON via the isolated
    // IHeartRadio module on the producer thread's existing ~10s cadence.
    IHeartRadio iheart_;
    bool        is_iheart_        = false;  // set in hlsConnect by URL sniff
    std::atomic<bool> prefer_digital_{ false };  // user pref: try web-player (digital) rendition
    std::atomic<bool> digital_active_{ false };  // current connection is the digital rendition (vs raw)
    std::atomic<int>  connect_seq_{ 0 };         // bumped per (re)connect; delimits modes in the deep log
    // Live-edge re-pin (digital only): on an ad-onset discontinuity, request a
    // re-handshake so we rejoin the live program in step with the web player
    // instead of drifting behind across our (independent, longer) SSAI ad pod.
    std::atomic<bool> hls_repin_pending_{ false };  // producer should re-handshake at next safe point
    bool              hls_repin_armed_ = true;      // one-shot: fire once per ad break, then re-arm
    DWORD             hls_repin_cooldown_until_ = 0;// re-arm only after this tick (suppress mid-pod)

    // ── Staging lane (dual-stream smooth re-pin) ─────────────────────────────
    // The coordinator (a normal is_lane_=false instance) owns ONE staging
    // StreamSource (is_lane_=true) and prebuffers it in parallel during an ad, so
    // we can blend into a session whose OWN live edge has already cleared to music
    // — instead of hard-cutting the primary to wherever its edge happens to sit
    // (the once-an-hour mid-song clobber). Stage A: shadow observer only — arm,
    // watch for the lane's music edge, log, tear down. No audio-path change yet;
    // the existing re-pin still drives playback. B swaps into the lane, C blends.
    bool                is_lane_ = false;
    std::atomic<int>    last_cls_{ 0 };              // newest manifest seg: 0 none,1 music,2 ad,3 other
    std::unique_ptr<StreamSource> staging_;          // parallel session (coordinator only)
    enum class Stg { Idle, Opening, Arming };
    Stg                 stg_state_ = Stg::Idle;
    std::atomic<bool>   stg_opening_{ false };        // staging open() in flight on a detached thread
    DWORD               stg_cooldown_until_ = 0;       // suppress re-arm until this tick
    DWORD               stg_armed_tick_ = 0;            // tick the staging peek armed (for elapsed-since-arm logging)
    void serviceStaging();                             // coordinator-only: arm/watch/tear down the lane
    DWORD       last_iheart_poll_ = 0;      // trackHistory poll throttle (GetTickCount)
    std::string iheart_th_cache_;           // cached trackHistory result between throttled polls
    long        iheart_th_ended_  = -1;     // cached trackHistory staleness (now - endTime)
    IHeartRadio::CurrentTrack iheart_ctm_;  // cached currentTrackMeta (polled while deep log is on, or in digital mode for album art)
    // Debounced reconciliation state machine (pure unit; see IHeartNowPlayingSM.h).
    IHeartNowPlayingSM ih_sm_;
    void        updateIHeartNowPlaying(const std::string& body);
    static std::string hlsFirstUri(const std::string& body);
    static std::string hlsResolveUrl(const std::string& base, const std::string& ref);

    std::string             url_;
    std::string             last_error_;

    // ICY de-interleaving state (producer thread)
    int                     icy_metaint_ = 0;   // bytes between metadata blocks (0 = none)
    int                     icy_counter_ = 0;   // audio bytes until next metadata block
    std::vector<uint8_t>    raw_buf_;           // buffered raw network bytes
    size_t                  raw_pos_     = 0;
    mutable std::mutex      now_playing_mtx_;
    std::string             now_playing_;
    // ── Audio-aligned label publish ──────────────────────────────────────────
    // now_playing_ commits off the manifest live edge, which sits ~one buffer
    // depth (ring + undecoded pending) AHEAD of the speaker — so the label flips
    // to the next song / "Commercial break" while the current song's tail is still
    // audible. We hold each committed label until the audio it describes is heard:
    // on commit, enqueue it with a release tick = now + measured buffer depth; the
    // getter publishes entries as they come due. Display-only — reconciliation,
    // debounce, scrobble, and decode are untouched. Reset on ringClear() (re-pin),
    // where the buffer is flushed and the label must snap to the live jump.
    struct NpPub { DWORD releaseTick; std::string disp; };
    mutable std::deque<NpPub> np_pub_q_;
    mutable std::string       np_published_;
    std::string             iheart_art_;       // album-art URL for the committed song (digital mode; "" otherwise). Guarded by now_playing_mtx_.

    // WinINet handles (owned by producer thread once open() returns). ICY/continuous
    // path ONLY — the live audio read loop (rawRead -> InternetReadFile(hConn_) ->
    // ring) stays raw WinINet, permanently outside the IHttp seam.
    HINTERNET               hInet_ = nullptr;
    HINTERNET               hConn_ = nullptr;
    // HLS keep-alive session (slice 4): backs the hlsHttpGet manifest/segment
    // one-shots via the core::IHttp seam. Same lifetime the raw hInet_ had for HLS —
    // created in hlsEnsureSession, dropped in disconnect(), so every re-handshake /
    // ad-onset re-pin gets a fresh session, exactly as baseline.
    std::unique_ptr<core::IHttpSession> hls_session_;

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
    std::atomic<uint32_t>   fade_in_remaining_{ 0 };// frames of re-pin fade-in left (set by ringClear, consumed by readFrames)

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
    void ringClear();   // producer-side flush (re-pin): drop buffered audio, jump to live
};

#endif // _WIN32
