// GainScan.h — batch ReplayGain over a folder (batch-r128 slice).
//
// The fourth assembly of proven parts (the plan's whole thesis): the
// per-track ebur128 block is the rip/recorder shape a third time, the
// dialect writers follow the recorder's tagCut precedent (Xiph /
// standard-TXXX MP3 / R128 Opus / APEv2 WavPack / MP4 PropertyMap), and
// decode-for-measurement is BPM detection's twin (a private LocalFileSource
// fully drained on the worker). Engine class, UIManager-free, so
// gain_scan_test pins the whole contract headlessly (the recorder pattern).
//
// Semantics: track gain only (album grouping is a recorded follow-up);
// skip-already-tagged by default (both dialects read via TagLib
// PropertyMap — which also means pre-mp3-rg-write BLOB-shape MP3s read as
// untagged and get HEALED to standard frames, blob removed); .wav counted
// skipped-with-note (untagged by rip policy too); cancel abandons the
// in-flight file (nothing written for it) — every stop path leaves whole,
// valid files only. Audio thread never involved.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class GainScan {
public:
    ~GainScan();                 // cancels + joins if running

    // Enumerate dir (non-recursive) and start the worker. false if already
    // running or the directory can't be read.
    bool start(const std::string& dir, bool force);
    void cancel();               // worker abandons the in-flight file cleanly

    bool     running()   const { return running_.load(std::memory_order_relaxed); }
    int      total()     const { return total_.load(std::memory_order_relaxed); }
    int      index()     const { return index_.load(std::memory_order_relaxed); }
    int      tagged()    const { return tagged_.load(std::memory_order_relaxed); }
    int      skipped()   const { return skipped_.load(std::memory_order_relaxed); }
    int      errors()    const { return errors_.load(std::memory_order_relaxed); }
    int      wavNoted()  const { return wav_noted_.load(std::memory_order_relaxed); }
    bool     cancelled() const { return cancel_.load(std::memory_order_relaxed); }
    // One-shot: true exactly once after the worker finishes (the UI tick
    // polls this to fire the summary toast).
    bool     takeFinished() { return finished_.exchange(false); }
    std::string currentFile() const;

private:
    void workerLoop(std::string dir, bool force);
    bool processFile(const std::string& path, bool force);   // one complete unit

    std::vector<std::string> files_;
    std::thread           worker_;
    std::atomic<bool>     running_  { false };
    std::atomic<bool>     cancel_   { false };
    std::atomic<bool>     finished_ { false };
    std::atomic<int>      total_ { 0 }, index_ { 0 };
    std::atomic<int>      tagged_ { 0 }, skipped_ { 0 }, errors_ { 0 }, wav_noted_ { 0 };
    mutable std::mutex    file_mtx_;
    std::string           current_file_;
};
