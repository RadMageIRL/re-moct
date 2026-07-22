#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>

// ─── MusicBrainz DiscID + metadata lookup ────────────────────────────────────
// Non-blocking CD metadata retrieval via WinINet.
// Spawns a single worker thread per lookup. Rate-limited to 1 req/sec.
// Uses the MusicBrainz DiscID algorithm (SHA-1 + custom Base64).

struct MBTrack {
    int         number;        // 1-based position WITHIN its disc
    std::string title;
    std::string artist;
    int         disc = 1;      // 1-based medium/disc number (for multi-disc sets)
};

struct MBRelease {
    std::string mb_id;   // MusicBrainz release ID (for Cover Art Archive)
    std::string title;
    std::string artist;
    std::string date;
    std::vector<MBTrack> tracks;
};

// For multi-disc releases, rel.tracks holds every disc's tracks tagged with
// MBTrack::disc. Given the physical disc's audio track count, return the 1-based
// disc whose track count matches. Unambiguous single match wins; otherwise (no
// match, or two discs sharing a count) fall back to disc 1. Single-disc releases
// always return 1. Lookups should scope on (number == tnum && disc == result).
inline int pickDiscForTrackCount(const MBRelease& rel, int n_physical) {
    int max_disc = 1;
    for (const auto& t : rel.tracks) if (t.disc > max_disc) max_disc = t.disc;
    if (max_disc == 1) return 1;                 // single-disc: nothing to scope
    int found = 1, nmatch = 0;
    for (int d = 1; d <= max_disc; ++d) {
        int c = 0;
        for (const auto& t : rel.tracks) if (t.disc == d) ++c;
        if (c == n_physical) { found = d; ++nmatch; }
    }
    return (nmatch == 1) ? found : 1;            // exact unique match, else disc 1
}

// Callback fired on completion (success or failure), always on worker thread.
// Caller must sync before touching UI state.
using MBCallback = std::function<void(bool ok, const MBRelease& result, const std::string& err)>;

// ─── Text search result (lightweight — just enough for the picker UI) ─────────
struct MBSearchResult {
    std::string mbid;
    std::string title;
    std::string artist;
    std::string date;
    std::string country;
    std::string label;
    int         track_count  = 0;
    bool        from_discogs = false;  // true if result came from Discogs fallback
};

using MBSearchCallback = std::function<void(bool ok,
                                            std::vector<MBSearchResult> results,
                                            const std::string& err)>;

class MBLookup {
public:
    MBLookup() = default;
    ~MBLookup() { cancel(); }

    // Start async DiscID lookup. Returns false if already in progress.
    // toc_offsets: sector offsets for each track (1-based), plus lead-out as last entry.
    bool lookup(int first_track, int last_track,
                const std::vector<uint32_t>& toc_offsets,
                MBCallback cb);

    // Search MusicBrainz by artist + album text.
    // Falls back to Discogs if MB returns zero results.
    // Returns false if a lookup/search is already in progress.
    bool search(const std::string& artist, const std::string& album,
                MBSearchCallback cb);

    // Fetch full release metadata by MBID (feeds the existing MBCallback pipeline).
    // Returns false if already in progress.
    bool lookupByMbid(const std::string& mbid, MBCallback cb);

    // Fetch full release metadata from Discogs by numeric release ID.
    // The Discogs /database/search endpoint returns NO tracklist; this hits the
    // release-detail endpoint (/releases/{id}) and feeds the same MBCallback
    // pipeline so a Discogs pick populates tracks like an MB pick.
    // Returns false if already in progress.
    bool lookupDiscogsRelease(const std::string& discogs_id, MBCallback cb);

    bool isActive() const { return active_.load(); }
    void cancel();

    // Compute DiscID from TOC data (public for testing)
    static std::string computeDiscId(int first_track, int last_track,
                                     const std::vector<uint32_t>& offsets);

    // URL-encode a UTF-8 string for use in query parameters
    static std::string urlEncode(const std::string& s);

    // SHA-1 over a byte buffer -> 20-byte digest. Pure/stateless (no member or static
    // state); public so other request-signing paths (e.g. Podcast Index auth) reuse it
    // instead of vendoring a second hash. Used internally by the DiscID computation.
    static void sha1(const uint8_t* data, size_t len, uint8_t out[20]);

private:
    std::atomic<bool>   active_  { false };
    std::atomic<bool>   cancel_  { false };
    std::thread         thread_;

    static std::chrono::steady_clock::time_point last_request_;
    static std::mutex                            rate_mutex_;

    // DiscID worker (existing)
    void worker(int first_track, int last_track,
                std::vector<uint32_t> offsets, MBCallback cb);

    // Text search worker
    void searchWorker(std::string artist, std::string album, MBSearchCallback cb);

    // MBID direct fetch worker
    void mbidWorker(std::string mbid, MBCallback cb);

    // Discogs release-detail fetch worker (parses tracklist)
    void discogsReleaseWorker(std::string discogs_id, MBCallback cb);

    static std::string httpGet(const std::string& url);
    static MBRelease   parseJson(const std::string& json);

    // Parse the /ws/2/release?query=... response into a list of lightweight results
    static std::vector<MBSearchResult> parseSearchJson(const std::string& json);

    // Parse Discogs /database/search response as fallback
    static std::vector<MBSearchResult> parseDiscogsJson(const std::string& json);

    // DiscID internals (sha1 is declared public above for cross-path reuse)
    static std::string mb_base64(const uint8_t* data, size_t len);
};

