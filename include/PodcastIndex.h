#pragma once

#include <string>
#include <vector>
#include <cstdint>

// One feed result from a Podcast Index byterm search. `url` is the RSS feed URL and
// is what a selection hands to the existing paste-URL subscribe path.
struct PodcastIndexResult {
    std::string title;
    std::string url;
    std::string author;
    std::string artwork;   // image URL (parsed; not yet shown)
};

enum class PodcastIndexStatus {
    Ok,            // request succeeded (feeds may be empty = zero results)
    NoCreds,       // key or secret missing
    AuthFailed,    // HTTP 401 - bad key/secret OR a drifted system clock (>3 min skew)
    NetworkError,  // transport failure / timeout / empty body
};

struct PodcastIndexSearchResult {
    PodcastIndexStatus              status = PodcastIndexStatus::NetworkError;
    std::vector<PodcastIndexResult> feeds;
};

// Thin client for the Podcast Index search API. Mirrors RadioBrowser's IHttp + nlohmann
// pattern and, like it (and PodcastClient::fetch), is SYNCHRONOUS - the caller runs it
// on a worker thread so a slow/dead endpoint never blocks the UI (the podcast-fetch
// async pattern). Ships no credentials; the user supplies their own free key/secret.
class PodcastIndex {
public:
    // byterm podcast search. unixTime is passed IN (not read here) so the auth token is
    // deterministic and unit-testable. Blocks on one HTTP request, bounded by a timeout.
    static PodcastIndexSearchResult search(const std::string& key, const std::string& secret,
                                           const std::string& term, std::int64_t unixTime,
                                           int limit = 30);

    // Authorization header value: lowercase-hex SHA-1 of (key + secret + unixTime),
    // per the Podcast Index "Amazon-style" auth. Public for deterministic unit testing;
    // reuses MBLookup::sha1 (no second hash vendored).
    static std::string authToken(const std::string& key, const std::string& secret,
                                 std::int64_t unixTime);

    // Parse a byterm response body into feeds. Public for unit testing. Malformed or
    // empty input -> empty vector; never throws.
    static std::vector<PodcastIndexResult> parse(const std::string& json);
};
