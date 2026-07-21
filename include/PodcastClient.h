#pragma once

#include "PodcastFeed.h"
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
};
