// StreamRecorder.h — "rip the radio": capture the playing stream to disk,
// split on metadata-title changes, tag each cut (stream-record slice R1).
//
// Three isolated lanes that never block each other (the plan's governing
// constraint, docs/stream-record-plan.md §3):
//
//   audio thread  -> capture(): ONE relaxed-atomic check + ONE bounded copy
//                    into a preallocated SPSC ring. No lock, no allocation,
//                    no syscall — strictly lighter than the EQ pass already
//                    running in the same callback branch. A full ring DROPS
//                    the incoming block and counts it (visible, never
//                    blocking, never corrupting).
//   worker thread -> a dedicated std::thread owned here: drains the ring,
//                    feeds libebur128 (per-cut ReplayGain), converts f32->s16,
//                    drives the IEncoder fan-out (Mp3Encoder/OpusRipEncoder —
//                    pure PCM-to-file, constructed directly), rolls to a new
//                    cut when the split flag fires, tags finished cuts
//                    (TagLib; R128 dialect for Opus via R128Gain.h).
//   UI thread     -> start()/stop()/onTitle() + state accessors. onTitle is
//                    the split trigger: the caller's EXISTING per-tick
//                    streamNowPlaying() poll hands changes here; this class
//                    never touches the plugin or the audio thread.
//
// R1 is headless: nothing in the UI arms it (the [Rec] panel + ^E are R2).
// The engine contract is frozen by stream_record_test before any UI exists.
#pragma once

#include "RipFormats.h"   // RipFormat (Mp3/Opus rows only are accepted here)

// libebur128 for the per-cut ReplayGain state — included, not forward-declared:
// ebur128_state is an anonymous-struct typedef (the same fact CDRipper.h
// documents for its ripTrack parameter).
#include <ebur128.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class IEncoder;

// The capture request the caller hands to start() — copied in, like
// RipOptions into CDRipper::start (no mutable cross-thread state). Defaults
// mirror RipOptions' quality literals (one quality truth; config-fed in R2).
struct RecOptions {
    std::vector<RipFormat> formats { RipFormat::Opus };  // Mp3/Opus only (plan §2)
    int  mp3_vbr_q     = 0;        // LAME VBR quality 0-9 (0 = V0)
    int  opus_bitrate  = 128000;   // = kOpusDefaultBitrate; caller clamps
    bool split_on_meta = true;     // false -> one continuous timestamp-named file
};

class StreamRecorder {
public:
    static constexpr uint32_t SAMPLE_RATE = 44100;
    static constexpr uint32_t CHANNELS    = 2;
    // Default ring: 2^19 frames ≈ 11.9 s ≈ 4 MiB of f32 stereo — generous
    // headroom for a worker that encodes many-times-realtime. Ctor-injectable
    // (rounded up to a power of two) so the overflow test can force drops.
    static constexpr uint32_t kDefaultRingFrames = 1u << 19;

    // A finished (or failed) cut, for the state surface + the test contract.
    struct CutInfo {
        std::vector<std::string> paths;   // one per format, final names
        std::string artist, title, raw;   // metadata the cut was tagged with
        uint64_t    frames  = 0;          // PCM frames encoded into it
        bool        partial = false;      // session-start / session-stop edge
        bool        failed  = false;      // write/finalize failure (files removed)
    };

    explicit StreamRecorder(uint32_t ring_frames = kDefaultRingFrames);
    ~StreamRecorder();                    // stops + joins if still running

    StreamRecorder(const StreamRecorder&)            = delete;
    StreamRecorder& operator=(const StreamRecorder&) = delete;

    // ── UI thread ────────────────────────────────────────────────────────────
    // Begin capturing under out_dir/<station>/. Returns false (and records
    // lastError) if already recording, the format list is empty/not lossy-only,
    // or the directory cannot be created. Encoders open lazily on the first
    // drained frame, so arming during prebuffer is fine.
    bool start(const RecOptions& opt, const std::string& station,
               const std::string& out_dir);
    // Finalize the current cut (marked partial), join the worker. Cheap no-op
    // when idle — teardown sites may call it unconditionally.
    void stop();
    // The split trigger: hand every polled now-playing CHANGE here (the caller
    // compares; passing an unchanged title is a harmless no-op). Splits at the
    // next worker drain boundary when split_on_meta is set; before any audio
    // has flowed it just (re)labels the pending first cut.
    void onTitle(const std::string& raw_now_playing);
    // Cover art for the CURRENT song (rec-cover-art): bytes fetched by the
    // caller's existing radio-art machinery, keyed by the RAW now-playing
    // string so it matches cut_raw_ with exact string equality — no parse in
    // the loop. Validated here (JPEG/PNG magic -> MIME; anything else
    // rejected). Embedded at tag time ONLY if the key still matches the
    // closing cut: no art rather than wrong art. Never fetches, never blocks
    // a roll — the onTitle pattern (meta_mtx_, UI <-> worker only).
    void onArt(const std::string& raw_now_playing, std::vector<uint8_t> bytes);

    // ── audio thread (lock-free; see push() for the full argument) ──────────
    void capture(const float* interleaved, uint32_t frames) {
        if (!capturing_.load(std::memory_order_relaxed)) return;
        push(interleaved, frames);
    }

    // ── state (any thread) ───────────────────────────────────────────────────
    bool     recording()     const { return running_.load(std::memory_order_relaxed); }
    uint64_t droppedFrames() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t totalFrames()   const { return total_frames_.load(std::memory_order_relaxed); }
    uint64_t bytesWritten()  const { return bytes_written_.load(std::memory_order_relaxed); }
    int      elapsedSec()    const { return (int)(totalFrames() / SAMPLE_RATE); }
    // Index of the cut currently being written (== finished-cut count).
    int      cutIndex()      const { return cut_index_.load(std::memory_order_relaxed); }
    std::string currentTitle() const;         // the title the open cut will carry
    std::string lastError()    const;         // empty = healthy
    std::vector<CutInfo> cuts() const;        // finished cuts (copy)

    // "Artist - Title" per the ICY convention (the scrobbler's exact idiom:
    // first " - " splits, both sides trimmed). ok only when BOTH sides are
    // non-empty — anything else falls back to timestamp naming, and the raw
    // string still reaches the TITLE tag so no metadata is silently lost.
    struct ParsedTitle { std::string artist, title; bool ok = false; };
    static ParsedTitle parseNowPlaying(const std::string& raw);

private:
    void push(const float* interleaved, uint32_t frames);   // audio thread
    void workerLoop();
    // Worker-side helpers (all on the worker thread).
    bool openCut();                        // lazy: on the first drained frame
    bool processBlock(const float* f32, uint32_t frames);   // false = cut failed
    void rollCut(bool partial_tail);       // finalize+tag current, ready next
    void failCut(const std::string& why);  // finalize(false), remove files, stop
    std::string cutBasePath() const;       // dir + sanitized name (no ext)

    // ── ring (SPSC: audio thread writes, worker reads) ───────────────────────
    std::vector<float>    ring_;           // ring_frames_ * CHANNELS floats
    uint32_t              ring_frames_ = 0;             // power of two
    std::atomic<uint64_t> wpos_ { 0 }, rpos_ { 0 };     // frames, monotonic
    std::atomic<uint64_t> dropped_ { 0 };

    // ── control / state ──────────────────────────────────────────────────────
    std::atomic<bool>     capturing_ { false };   // gates the audio-thread tap
    std::atomic<bool>     running_   { false };   // worker lifetime
    std::atomic<bool>     stop_req_  { false };
    std::thread           worker_;

    RecOptions            opt_;            // copied at start(); worker-owned after
    std::string           station_dir_;    // out_dir/<station>, created at start()
    std::string           station_;

    // Pending metadata from onTitle (UI thread -> worker). Never touched by
    // the audio thread.
    mutable std::mutex    meta_mtx_;
    std::string           pending_raw_;
    std::atomic<bool>     split_pending_ { false };
    // Pending cover art (rec-cover-art): raw-title key + validated bytes +
    // magic-derived MIME. Consumed (by key match) at tag time.
    std::string           pending_art_raw_;
    std::vector<uint8_t>  pending_art_;
    const char*           pending_art_mime_ = nullptr;

    // ── current cut (worker-owned between open/roll) ─────────────────────────
    struct Out { RipFormat fmt; std::string path; std::unique_ptr<IEncoder> enc; };
    std::vector<Out>      outs_;
    ebur128_state*        ebur_ = nullptr;
    std::string           cut_artist_, cut_title_, cut_raw_;
    uint64_t              cut_frames_ = 0;
    bool                  cut_open_ = false;
    bool                  first_cut_ = true;
    std::vector<int16_t>  s16_;            // f32->s16 staging (worker scratch)

    // ── results / counters ───────────────────────────────────────────────────
    std::atomic<int>      cut_index_ { 0 };
    std::atomic<uint64_t> total_frames_ { 0 };
    std::atomic<uint64_t> bytes_written_ { 0 };
    mutable std::mutex    cuts_mtx_;
    std::vector<CutInfo>  cuts_;
    std::string           error_;          // guarded by cuts_mtx_
};
