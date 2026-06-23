#pragma once
#include <string>
#include <vector>
#include <deque>
#include <optional>
#include <cstddef>
#include <queue>
#include <mutex>
#include <algorithm>
#include <thread>
#include <atomic>

struct PlaylistEntry {
    std::string path;
    std::string display_title;
    int         duration_sec = 0;
};

enum class RepeatMode { Off, One, All };

class PlaylistManager {
public:
    PlaylistManager()  = default;
    ~PlaylistManager() {
        loader_running_.store(false);
        if (loader_thread_.joinable()) loader_thread_.join();
    }

    // Mutation
    std::size_t addTrack(const std::string& path);
    std::size_t addStream(const std::string& url, const std::string& title);
    static std::string streamLabel(const std::string& url);  // "RADIO: <name>" from a URL
    std::size_t addCDTrack(const std::string& fake_path,
                           const std::string& title, int duration_sec);
    template<typename Pred>
    void removeIf(Pred pred) {
        size_t old_current = current_;
        size_t removed_before = 0;
        for (size_t i = 0; i < entries_.size(); ) {
            if (pred(entries_[i])) {
                if (i < old_current) ++removed_before;
                entries_.erase(entries_.begin() + (std::ptrdiff_t)i);
            } else {
                ++i;
            }
        }
        if (old_current >= removed_before)
            current_ = old_current - removed_before;
        else
            current_ = 0;
        if (current_ >= entries_.size() && !entries_.empty())
            current_ = entries_.size() - 1;
        rebuildShuffleOrder();
    }
    void        addDirectory(const std::string& dir_path);
    void        removeAt(std::size_t index);
    void        clear();
    void        moveUp(std::size_t index);    // swap with previous entry
    void        moveDown(std::size_t index);  // swap with next entry

    // Navigation
    std::optional<std::string> currentPath() const;
    std::optional<std::string> next();
    std::optional<std::string> previous();
    std::optional<std::string> peekNext() const;  // look ahead without advancing
    bool selectAt(std::size_t index);

    // Repeat
    RepeatMode  repeatMode()  const { return repeat_; }
    void        setRepeat(RepeatMode m) { repeat_ = m; }
    void        cycleRepeat() { repeat_ = static_cast<RepeatMode>((int(repeat_)+1) % 3); }
    const char* repeatLabel() const {
        switch (repeat_) {
            case RepeatMode::Off: return "  ";
            case RepeatMode::One: return "R1";
            case RepeatMode::All: return "RA";
        }
        return "  ";
    }

    // Shuffle
    bool shuffle()           const { return shuffle_; }
    void setShuffle(bool on)       { shuffle_ = on; rebuildShuffleOrder(); }
    void toggleShuffle()           { setShuffle(!shuffle_); }
    const char* shuffleLabel() const { return shuffle_ ? "SH" : "  "; }

    // Query
    std::size_t size()    const { return entries_.size(); }
    bool        empty()   const { return entries_.empty(); }
    std::size_t current() const { return current_; }
    const PlaylistEntry&              at(std::size_t i) const { return entries_.at(i); }
    const std::vector<PlaylistEntry>& entries()         const { return entries_; }
    void setDisplayTitle(std::size_t i, const std::string& t) {
        if (i < entries_.size()) entries_[i].display_title = t;
    }

    // Sort
    enum class SortMode { None, Title, Artist, Duration, Filename };
    void sortBy(SortMode mode);
    SortMode sortMode() const { return sort_mode_; }
    const char* sortLabel() const {
        switch (sort_mode_) {
            case SortMode::None:     return "";
            case SortMode::Title:    return "title";
            case SortMode::Artist:   return "artist";
            case SortMode::Duration: return "duration";
            case SortMode::Filename: return "filename";
        }
        return "";
    }
    void cycleSort() {
        sort_mode_ = static_cast<SortMode>((int(sort_mode_)+1) % 5);
        sortBy(sort_mode_);
    }

    // M3U save / load
    // saveM3U writes an extended M3U (#EXTM3U) to path, returns true on success
    bool saveM3U(const std::string& path) const;
    // loadM3U appends tracks from an M3U file, returns number added
    int  loadM3U(const std::string& path);

    // PLS support
    bool savePLS(const std::string& path) const;
    int  loadPLS(const std::string& path);

    // XSPF support
    bool saveXSPF(const std::string& path) const;
    int  loadXSPF(const std::string& path);

    // Unified: detect format from extension (.m3u/.m3u8/.pls/.xspf)
    bool savePlaylist(const std::string& path) const;
    int  loadPlaylist(const std::string& path);
    // Count of audio entries in the last load whose file was not found on disk
    // (reset at the start of each loadPlaylist; ignores non-audio entries).
    int  lastLoadMissing() const { return last_load_missing_; }

    // ── Play Queue ───────────────────────────────────────────────────────────
    // FIFO queue that overrides playlist order. Consumed one entry at a time.
    void                       queueAdd(const PlaylistEntry& e);
    bool                       queueEmpty()  const { return play_queue_.empty(); }
    int                        queueSize()   const { return (int)play_queue_.size(); }
    std::optional<PlaylistEntry> queuePop();        // take front, remove it
    const PlaylistEntry&       queueAt(int i) const { return play_queue_.at((size_t)i); }
    void                       queueRemoveAt(int i);
    void                       queueClear()        { play_queue_.clear(); }

    template<typename Pred>
    void queueRemoveIf(Pred pred) {
        auto it = std::remove_if(play_queue_.begin(), play_queue_.end(), pred);
        play_queue_.erase(it, play_queue_.end());
    }

    static bool isSupportedAudio(const std::string& path);

    // Background loader — non-blocking addDirectory
    void addDirectoryAsync(const std::string& dir_path);
    bool drainPending();   // call from UI tick; returns true if any added
    int  pendingCount() const { return (int)pending_count_.load(); }
    bool isLoading()    const { return loader_running_.load(); }

private:
    std::vector<PlaylistEntry> entries_;
    std::size_t current_ = 0;
    RepeatMode  repeat_  = RepeatMode::Off;
    bool        shuffle_ = false;
    std::deque<PlaylistEntry>  play_queue_;  // overrides playlist order when non-empty

    // Shuffle order: shuffle_order_[play_pos] = real index into entries_
    std::vector<std::size_t> shuffle_order_;
    std::size_t              shuffle_pos_ = 0;

    SortMode sort_mode_ = SortMode::None;
    int last_load_missing_ = 0;   // see lastLoadMissing()

    void rebuildShuffleOrder();
    static void populateMetadata(PlaylistEntry& entry);

    // Async loader internals
    std::queue<std::string>  work_queue_;    // paths to scan (fed by addDirectoryAsync)
    std::queue<PlaylistEntry> done_queue_;   // completed entries ready to add
    std::mutex               work_mutex_;
    std::mutex               done_mutex_;
    std::atomic<int>         pending_count_ { 0 };
    std::atomic<bool>        loader_running_{ false };
    std::thread              loader_thread_;
    void loaderWorker();
};
