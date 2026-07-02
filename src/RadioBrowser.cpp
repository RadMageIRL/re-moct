#ifdef _WIN32

#include "RadioBrowser.h"
#include "IHttp.h"

#include <cctype>
#include "json.hpp"


namespace {

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F]; }
    }
    return out;
}

// Mirror servers — any may be down, so callers try them in order.
const char* kMirrors[] = {
    "https://de1.api.radio-browser.info",
    "https://nl1.api.radio-browser.info",
    "https://at1.api.radio-browser.info",
};

} // namespace

// HTTP GET via the core::IHttp seam (WinINet impl). Parity with the former inline
// WinINet GET: same 6 s fail-fast timeout (so a down mirror doesn't hang the UI),
// same 4 MB cap, HTTP status ignored (empty body -> caller tries the next mirror),
// default UA + redirect-follow.
std::string RadioBrowser::httpGet(const std::string& url) {
    core::HttpRequest req;
    req.url        = url;
    req.timeout_ms = 6000;             // fail fast on a down mirror
    req.max_body   = 4u * 1024 * 1024; // 4 MB safety cap (unchanged)
    return core::http().fetch(req).body;
}

std::vector<RadioStation> RadioBrowser::search(const std::string& query, int limit) {
    std::string enc = urlEncode(query);
    if (enc.empty()) return {};

    for (const char* mirror : kMirrors) {
        std::string url = std::string(mirror)
            + "/json/stations/search?name=" + enc
            + "&hidebroken=true&order=clickcount&reverse=true&limit="
            + std::to_string(limit);

        std::string body = httpGet(url);
        if (body.empty()) continue;

        try {
            auto j = nlohmann::json::parse(body);
            if (!j.is_array()) continue;

            std::vector<RadioStation> out;
            for (auto& s : j) {
                RadioStation rs;
                rs.name = s.value("name", std::string());
                rs.url  = s.value("url_resolved", std::string());
                if (rs.url.empty()) rs.url = s.value("url", std::string());
                rs.codec   = s.value("codec", std::string());
                rs.bitrate = s.value("bitrate", 0);
                rs.country = s.value("country", std::string());
                rs.uuid    = s.value("stationuuid", std::string());

                // Trim surrounding whitespace from the name.
                auto a = rs.name.find_first_not_of(" \t\r\n");
                auto b = rs.name.find_last_not_of(" \t\r\n");
                if (a != std::string::npos) rs.name = rs.name.substr(a, b - a + 1);
                else rs.name.clear();

                if (!rs.url.empty() && !rs.name.empty())
                    out.push_back(std::move(rs));
            }
            return out;   // first mirror that parses wins
        } catch (...) {
            continue;     // try the next mirror
        }
    }
    return {};
}

void RadioBrowser::countClick(const std::string& stationuuid) {
    if (stationuuid.empty()) return;
    // Best-effort; the API asks for one of these per stream the user starts.
    httpGet(std::string(kMirrors[0]) + "/json/url/" + stationuuid);
}

#endif // _WIN32
