#pragma once

// ─── Library scanner — walk the collection, read tags, refresh the index ─────
//
// Library slice 2. Fills and refreshes the LibraryIndex that slice 1 defined:
// walks the music root, reads tags with TagLib, and revalidates incrementally so
// a rescan re-reads only what actually changed. There is still no UI - this
// slice ends with a correct index on disk and nothing a user can see.
//
// THE INVARIANT THAT MATTERS MOST: **a cancelled scan never commits.** The
// refresh treats "not seen during the walk" as "deleted", which is correct only
// for a walk that RAN TO COMPLETION. A cancelled scan has simply not reached the
// rest of the collection yet, so committing its partial result would delete
// every record for every file the walk had not got to. ScanOutcome::completed
// is false in that case and saveIndexFileAtomic must not be called - the
// threaded LibraryScanner below enforces this, and the free functions document
// it so a future caller cannot get it wrong by accident.
//
// PATHS. Every path crossing this API is a UTF-8 std::string. Paths that come
// from the OS walk are valid UTF-8 by construction and are safe to hand to
// std::filesystem; paths built from FEED OR TAG TEXT are not, and this unit
// never builds one (tag text lands in record fields, never in a path). See
// docs/library-index-plan.md section 6 for the measured behaviour.
//
// THREADING. One worker thread, the same shape the podcast fetch/search/art/
// chapters workers use: atomics for liveness and progress, a mutex around the
// finished result, a cancel flag polled per file, and a join in the destructor.

#include "LibraryIndex.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace libidx {

// Live progress, polled by a caller on another thread. Cheap enough to read
// every frame; nothing here allocates.
struct ScanProgress {
    std::atomic<uint32_t> files_seen{0};   // candidates the walk has visited
    std::atomic<uint32_t> files_read{0};   // of those, actually tag-read
    std::atomic<bool>     cancel{false};   // set by any thread; polled per file
};

struct ScanCounters {
    uint32_t seen      = 0;   // audio files the walk visited
    uint32_t added     = 0;   // not in the previous index
    uint32_t updated   = 0;   // present but mtime/size changed
    uint32_t unchanged = 0;   // revalidated without a tag read
    uint32_t removed   = 0;   // in the previous index, not on disk any more
    uint32_t tagless   = 0;   // indexed, but TagLib gave nothing usable
    uint32_t errors    = 0;   // could not be stat'ed or otherwise skipped
};

struct ScanOutcome {
    LibraryIndex  index;
    ScanCounters  counts;
    // FALSE means the scan was cancelled and `index` is PARTIAL. Do not commit
    // it; see the invariant at the top of this file.
    bool          completed = false;
};

// ── The synchronous core (testable without threads) ─────────────────────────
// Walks `root`, reusing records from `previous` whose path, mtime and size all
// match. Never throws: an unreadable directory, an unstat-able file or a path
// std::filesystem refuses degrades to a counter, not an exception.
ScanOutcome scanCollection(const std::string& root,
                           const LibraryIndex& previous,
                           ScanProgress& progress);

// ── Index file I/O ──────────────────────────────────────────────────────────
// Default location: <config dir>/library.idx, beside remoct.conf and theme.conf.
// Never in the music root.
std::string libraryIndexPath();

// Missing or unreadable file, or a header parseIndex rejects, both yield false
// with `out` left empty - the caller treats that as "no index" and does a full
// scan. `skipped_out` (optional) receives parseIndex's honest count of dropped
// records for a file that WAS usable.
bool loadIndexFile(const std::string& path, LibraryIndex& out,
                   std::size_t* skipped_out = nullptr);

// Writes <path>.tmp, flushes, closes, then renames over <path>. A crash midway
// leaves the previous index intact and a half-written one is never observable.
bool saveIndexFileAtomic(const std::string& path, const LibraryIndex& idx);

// ── The worker ──────────────────────────────────────────────────────────────
// start() spawns; the destructor cancels and joins, so a quit mid-scan cannot
// hang. On a scan that COMPLETES, the index is saved to the path given to
// start(); on a cancelled one nothing is written.
class LibraryScanner {
public:
    LibraryScanner() = default;
    ~LibraryScanner();
    LibraryScanner(const LibraryScanner&)            = delete;
    LibraryScanner& operator=(const LibraryScanner&) = delete;

    // No-op if a scan is already running.
    void start(const std::string& root, const std::string& index_path);
    void cancel();                       // request; returns immediately
    bool active() const { return active_.load(std::memory_order_acquire); }
    bool done()   const { return done_.load(std::memory_order_acquire); }

    const ScanProgress& progress() const { return progress_; }

    // Valid once done(); moves the result out. On a cancelled scan the outcome's
    // completed flag is false and nothing was written to disk.
    ScanOutcome take();

private:
    void join();

    std::thread       worker_;
    ScanProgress      progress_;
    mutable std::mutex mtx_;
    ScanOutcome       outcome_;
    std::atomic<bool> active_{false};
    std::atomic<bool> done_{false};
    bool              saved_ = false;
};

} // namespace libidx
