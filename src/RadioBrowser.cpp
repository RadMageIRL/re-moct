#ifdef _WIN32

#include "RadioBrowser.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <cctype>
#include "json.hpp"


namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

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

std::string RadioBrowser::httpGet(const std::string& url) {
    std::wstring wurl = toWide(url);

    HINTERNET inet = InternetOpenW(
        L"RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) return {};

    // Bound the blocking time so a down mirror fails fast instead of hanging the UI.
    DWORD timeout = 6000;  // ms
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(inet, INTERNET_OPTION_SEND_TIMEOUT,    &timeout, sizeof(timeout));

    HINTERNET conn = InternetOpenUrlW(inet, wurl.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!conn) { InternetCloseHandle(inet); return {}; }

    std::string body;
    char  buf[4096];
    DWORD bytes = 0;
    while (InternetReadFile(conn, buf, sizeof(buf), &bytes)) {
        if (bytes == 0) break;
        body.append(buf, bytes);
        if (body.size() > 4 * 1024 * 1024) break;  // 4MB safety cap
    }

    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return body;
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
