#include "PodcastIndex.h"
#include "MBLookup.h"       // reuse the in-tree SHA-1 (public, stateless static)
#include "core/IHttp.h"

#include <cctype>
#include "json.hpp"

namespace {

// Percent-encode a query term (mirrors RadioBrowser's local encoder).
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

} // namespace

std::string PodcastIndex::authToken(const std::string& key, const std::string& secret,
                                    std::int64_t unixTime) {
    std::string s = key + secret + std::to_string(unixTime);
    std::uint8_t digest[20];
    MBLookup::sha1(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), digest);
    static const char* hex = "0123456789abcdef";   // lowercase, per the API spec
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 20; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 0x0F];
    }
    return out;
}

std::vector<PodcastIndexResult> PodcastIndex::parse(const std::string& body) {
    std::vector<PodcastIndexResult> out;
    if (body.empty()) return out;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("feeds") || !j["feeds"].is_array()) return out;
        for (auto& f : j["feeds"]) {
            PodcastIndexResult r;
            r.title   = f.value("title",  std::string());
            r.url     = f.value("url",    std::string());
            r.author  = f.value("author", std::string());
            r.artwork = f.value("artwork", std::string());
            if (r.artwork.empty()) r.artwork = f.value("image", std::string());

            // Trim whitespace from the title (mirrors RadioBrowser's name trim).
            auto a = r.title.find_first_not_of(" \t\r\n");
            auto b = r.title.find_last_not_of(" \t\r\n");
            if (a != std::string::npos) r.title = r.title.substr(a, b - a + 1);
            else r.title.clear();

            if (!r.url.empty() && !r.title.empty())
                out.push_back(std::move(r));
        }
    } catch (...) {
        return {};   // malformed body -> no results, never throw
    }
    return out;
}

PodcastIndexSearchResult PodcastIndex::search(const std::string& key, const std::string& secret,
                                              const std::string& term, std::int64_t unixTime,
                                              int limit) {
    PodcastIndexSearchResult out;
    if (key.empty() || secret.empty()) { out.status = PodcastIndexStatus::NoCreds; return out; }

    std::string enc = urlEncode(term);
    if (enc.empty()) { out.status = PodcastIndexStatus::Ok; return out; }   // nothing to search

    core::HttpRequest req;
    req.url = "https://api.podcastindex.org/api/1.0/search/byterm?q=" + enc
            + "&max=" + std::to_string(limit);
    req.user_agent = "RE-MOCT/1.4 (podcast index)";   // API requires a User-Agent
    req.timeout_ms = 8000;                             // fail fast; the UI thread never waits (async)
    req.max_body   = 8u * 1024 * 1024;
    req.headers = {
        {"X-Auth-Key",    key},
        {"X-Auth-Date",   std::to_string(unixTime)},
        {"Authorization", authToken(key, secret, unixTime)},
    };

    core::HttpResponse res = core::http().fetch(req);
    if (res.status == 401) { out.status = PodcastIndexStatus::AuthFailed; return out; }
    if (!res.ok || res.body.empty()) { out.status = PodcastIndexStatus::NetworkError; return out; }

    out.feeds  = parse(res.body);
    out.status = PodcastIndexStatus::Ok;
    return out;
}
