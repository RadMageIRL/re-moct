#pragma once
#ifdef _WIN32

#include <string>
#include <vector>

// A single station result from radio-browser.info.
struct RadioStation {
    std::string name;       // station display name
    std::string url;        // url_resolved (playable; playlists/redirects already resolved)
    std::string codec;      // "MP3", "AAC", ...
    int         bitrate = 0;
    std::string country;
    std::string uuid;       // stationuuid (for the click-count courtesy call)
};

// Thin synchronous client for the free, no-auth radio-browser.info directory.
// Mirrors MBLookup's WinINet + nlohmann pattern. Calls block briefly (bounded by
// a per-request timeout) and fall through mirror servers on failure.
class RadioBrowser {
public:
    // Search stations by name, most-popular first, working stations only.
    static std::vector<RadioStation> search(const std::string& query, int limit = 30);

    // Courtesy popularity ping when a user starts a station. Best-effort; ignored on failure.
    static void countClick(const std::string& stationuuid);

private:
    static std::string httpGet(const std::string& url);
};

#endif // _WIN32
