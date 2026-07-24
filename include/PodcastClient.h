#pragma once

#include "PodcastFeed.h"
#include "core/IHttp.h"   // core::ProgressFn for the streaming episode download
#include <cstdint>
#include <string>

// Thin synchronous client: GET a podcast feed URL over the core::IHttp seam and
// parse it with slice-1's parsePodcastFeed(). Mirrors RadioBrowser's shape (a
// thin static client over core::http()). The fetch itself blocks; the UI drives
// it on a worker thread (the [Podcasts] fetch), exactly as the stream connect
// and cover-art fetches run off the UI thread.
class PodcastClient {
public:
    struct Result {
        PodcastFeed feed;       // feed.ok == false on a fetch OR a parse failure
        bool fetched = false;   // true if the HTTP GET succeeded with a non-empty body
        // Lets the caller distinguish the two honest-failure toasts:
        //   !fetched            -> "Couldn't fetch feed"
        //   fetched && !feed.ok -> "Not a valid podcast feed"
    };

    // Fetch + parse. Never throws. Feeds can be large (real ones seen: ~5 MB JRE,
    // ~22 MB Adam Carolla), so the body cap is generous; the timeout is loose
    // because the call runs on a worker thread and never freezes the UI.
    static Result fetch(const std::string& url);

    struct ChaptersResult {
        std::string body;       // the RAW document, unparsed — the caller owns the trust boundary
        bool fetched = false;   // true if the HTTP GET succeeded with a non-empty body
    };

    // Fetch an episode's <podcast:chapters> document (v1.4.0). Deliberately NOT
    // fetch()'s bounds: a chapters document is kilobytes, so it gets a kilobyte-
    // scale cap and a short timeout rather than the 64 MB / 30 s a whole feed
    // needs. Returns the bytes verbatim — parsing happens in PodcastChapters.h,
    // which is the single place a document off the internet is trusted, on the
    // fetch path and on every later read of the cached copy alike. Never throws.
    static ChaptersResult fetchChapters(const std::string& url);

    // Stream an episode to a local file, reporting progress (slice 3). Runs on a
    // worker thread in the UI. `cancel` is a plain int32 flag the caller writes via
    // std::atomic_ref (0 = run, nonzero = abort) so a mid-download quit aborts fast;
    // pass nullptr for non-cancellable. Returns true iff the file was fully written.
    // Never throws. The body streams straight to disk (no in-memory buffer / cap).
    static bool download(const std::string& url, const std::string& dest_path,
                         const core::ProgressFn& progress, const std::int32_t* cancel);
};
