// ConvertJob.h - the convert-core batch engine (convert-core slice).
//
// The fifth assembly of proven parts (the GainScan thesis, one more time):
// decode a source through a PRIVATE LocalFileSource fully drained on a worker
// thread (the BPM/GainScan twin - 44100/2/f32, playback-identical, the audio
// thread never involved), convert f32 -> s16, feed the shared makeEncoder
// (IEncoder) seam, finalize, then carry the source's text tags across
// (PropertyMap - the player's own read surface). Engine class, UIManager-free,
// so convert_core_test pins the whole contract headlessly.
//
// v1 scope: 44100/2 output (the forced decoder rate - a > 44100 source
// downsamples, surfaced as a note in the UI, not here). Text tags only; cover
// art carryover is a deferred follow-up slice (no art reader exists yet).
// Collision: dst exists -> skip; src == dst (same canonical path) -> skip. The
// worker never clobbers the source or a prior output.
#pragma once

#include "RipFormats.h"   // RipFormat + RipOptions

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ConvertPair { std::string src, dst; };

// dst policy (single + marked selectors + folder enumeration all share it):
// same directory, same basename, the format's extension. Returns "" if fmt has
// no row (never, for the five real formats).
std::string convertDstPath(const std::string& src, RipFormat fmt);

// Is this a source RE-MOCT can decode-and-convert? (the decodable set).
bool convertSupportedInput(const std::string& path);

class ConvertJob {
public:
    ~ConvertJob();                       // cancels + joins if running

    // Explicit pair list (single file + marked set). false if already running.
    bool startFiles(std::vector<ConvertPair> pairs, RipFormat fmt, RipOptions opt);
    // Flat, NON-recursive folder: enumerate dir one level, convert every
    // supported audio file. false if already running or dir unreadable.
    bool startFolder(const std::string& dir, RipFormat fmt, RipOptions opt);
    void cancel();                       // abandons the in-flight file cleanly

    bool running()   const { return running_.load(std::memory_order_relaxed); }
    int  total()     const { return total_.load(std::memory_order_relaxed); }
    int  index()     const { return index_.load(std::memory_order_relaxed); }
    int  converted() const { return converted_.load(std::memory_order_relaxed); }
    int  skipped()   const { return skipped_.load(std::memory_order_relaxed); }
    int  errors()    const { return errors_.load(std::memory_order_relaxed); }
    bool cancelled() const { return cancel_.load(std::memory_order_relaxed); }
    // One-shot: true exactly once after the worker finishes (UI polls it to
    // fire the completion toast).
    bool takeFinished() { return finished_.exchange(false); }
    std::string currentFile() const;

    // The convert unit, exposed for the headless test. Decode -> f32 -> s16 ->
    // encode -> tag carryover, with the src==dst and dst-exists guards.
    enum class Result { Converted, SkippedExists, SkippedSamePath, Error };
    static Result convertOne(const std::string& src, const std::string& dst,
                             RipFormat fmt, const RipOptions& opt,
                             const std::atomic<bool>* cancel = nullptr);

private:
    bool startPairs(std::vector<ConvertPair> pairs, RipFormat fmt, RipOptions opt);
    void workerLoop(std::vector<ConvertPair> pairs, RipFormat fmt, RipOptions opt);

    std::thread        worker_;
    std::atomic<bool>  running_   { false };
    std::atomic<bool>  cancel_    { false };
    std::atomic<bool>  finished_  { false };
    std::atomic<int>   total_ { 0 }, index_ { 0 };
    std::atomic<int>   converted_ { 0 }, skipped_ { 0 }, errors_ { 0 };
    mutable std::mutex file_mtx_;
    std::string        current_file_;
};
