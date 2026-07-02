#ifdef _WIN32
#include "MBLookup.h"
#include "StringUtils.h"
#include "core/IHttp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
// wininet linked via CMakeLists target_link_libraries

#include "json.hpp"

#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <algorithm>

// ─── Static members ───────────────────────────────────────────────────────────
std::chrono::steady_clock::time_point MBLookup::last_request_ =
    std::chrono::steady_clock::now() - std::chrono::seconds(10);
std::mutex MBLookup::rate_mutex_;

// ─── SHA-1 implementation (RFC 3174) ─────────────────────────────────────────
static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void MBLookup::sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    uint64_t bit_len = (uint64_t)len * 8;

    // Pad message
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)(bit_len >> (i * 8)));

    // Process blocks
    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; ++j)
            w[j] = ((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                   ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for (int j = 16; j < 80; ++j)
            w[j] = rol32(w[j-3]^w[j-8]^w[j-14]^w[j-16], 1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int j = 0; j < 80; ++j) {
            uint32_t f, k;
            if      (j < 20) { f=(b&c)|(~b&d); k=0x5A827999; }
            else if (j < 40) { f=b^c^d;        k=0x6ED9EBA1; }
            else if (j < 60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else              { f=b^c^d;        k=0xCA62C1D6; }
            uint32_t t=rol32(a,5)+f+e+k+w[j]; e=d; d=c; c=rol32(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (int i = 0; i < 5; ++i) {
        out[i*4+0]=(h[i]>>24)&0xFF; out[i*4+1]=(h[i]>>16)&0xFF;
        out[i*4+2]=(h[i]>>8)&0xFF;  out[i*4+3]=h[i]&0xFF;
    }
}

// ─── MusicBrainz custom Base64 ────────────────────────────────────────────────
// Standard Base64 with + → . and / → _ and = → -
std::string MBLookup::mb_base64(const uint8_t* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i+1 < len) b |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) b |= data[i+2];
        out += tbl[(b>>18)&0x3F];
        out += tbl[(b>>12)&0x3F];
        out += (i+1<len) ? tbl[(b>>6)&0x3F] : '=';
        out += (i+2<len) ? tbl[b&0x3F]      : '=';
    }
    // MB substitutions
    for (char& c : out) {
        if (c == '+') c = '.';
        else if (c == '/') c = '_';
        else if (c == '=') c = '-';
    }
    return out;
}

// ─── DiscID computation ───────────────────────────────────────────────────────
// MusicBrainz DiscID algorithm:
// 1. Build string: first_track + last_track + lead_out + 99 track offsets (hex, 8-char each)
// 2. SHA-1 the string
// 3. Custom Base64 the 20-byte digest → 28-char DiscID
std::string MBLookup::computeDiscId(int first_track, int last_track,
                                     const std::vector<DWORD>& offsets) {
    // offsets[0..last_track-1] = track starts, offsets[last_track] = lead-out
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    ss << std::setw(2) << first_track;
    ss << std::setw(2) << last_track;
    // Lead-out is offsets[last_track]
    ss << std::setw(8) << (last_track < (int)offsets.size() ? offsets[last_track] : 0);
    // 99 track offsets (pad unused with 0)
    for (int i = 0; i < 99; ++i) {
        DWORD off = (i < (int)offsets.size() - 1) ? offsets[i] : 0;
        ss << std::setw(8) << off;
    }
    std::string s = ss.str();
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(s.data()), s.size(), digest);
    return mb_base64(digest, 20);
}

// ─── HTTP GET via the core::IHttp seam (WinINet impl) ─────────────────────────
// Parity with the former inline WinINet GET: same 4 MB safety cap, HTTP status
// ignored (a partial/failed body fails JSON parsing gracefully downstream), and the
// default UA + redirect-follow. Transport now lives behind core::http().
std::string MBLookup::httpGet(const std::string& url) {
    core::HttpRequest req;
    req.url      = url;
    req.max_body = 4u * 1024 * 1024;   // 4 MB safety cap (unchanged)
    return core::http().fetch(req).body;
}

// ─── JSON parsing ─────────────────────────────────────────────────────────────
MBRelease MBLookup::parseJson(const std::string& json_str) {
    MBRelease result;
    try {
        auto j = nlohmann::json::parse(json_str);

        // MusicBrainz returns releases array — pick first
        auto& releases = j.at("releases");
        if (releases.empty()) return result;
        auto& rel = releases[0];

        result.mb_id  = rel.value("id", "");
        result.title  = rel.value("title", "");
        result.date   = rel.value("date", "");

        // Artist credit
        if (rel.contains("artist-credit") && !rel["artist-credit"].empty())
            result.artist = rel["artist-credit"][0]["artist"].value("name", "");

        // Media → tracks. Parse ALL media (discs), tagging each track with its
        // 1-based disc number. The correct disc is selected at consumption time
        // via pickDiscForTrackCount() — see MBLookup.h. (Previously this read
        // only media[0], so every disc inherited disc 1's track titles.)
        if (rel.contains("media")) {
            int disc_no = 0;
            for (auto& media : rel["media"]) {
                ++disc_no;
                if (!media.contains("tracks")) continue;
                for (auto& t : media["tracks"]) {
                    MBTrack mt;
                    // "position" is the integer track index (1-based), always present.
                    // "number" is a display string that may be non-numeric (vinyl: "A1").
                    mt.number = t.value("position", 0);
                    mt.title  = t.value("title", "");
                    mt.disc   = disc_no;
                    // Artist from recording
                    if (t.contains("recording")) {
                        auto& rec = t["recording"];
                        if (rec.contains("artist-credit") && !rec["artist-credit"].empty())
                            mt.artist = rec["artist-credit"][0]["artist"].value("name", "");
                    }
                    result.tracks.push_back(std::move(mt));
                }
            }
        }
    } catch (...) {}
    return result;
}

// ─── Worker thread ────────────────────────────────────────────────────────────
void MBLookup::worker(int first_track, int last_track,
                       std::vector<DWORD> offsets, MBCallback cb) {
    // Rate limiting — enforce 1 req/sec
    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_request_).count();
        if (elapsed < 1000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed));
    }

    if (cancel_.load()) { active_.store(false); return; }

    std::string disc_id = computeDiscId(first_track, last_track, offsets);
    std::string url = "https://musicbrainz.org/ws/2/discid/" + disc_id +
                      "?fmt=json&inc=recordings+artist-credits";

    std::string body = httpGet(url);

    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        last_request_ = std::chrono::steady_clock::now();
    }

    if (cancel_.load()) { active_.store(false); return; }

    if (body.empty()) {
        if (cb) cb(false, {}, "Network error or no response");
        active_.store(false);
        return;
    }

    // Check for 404 / error in response
    if (body.find("\"error\"") != std::string::npos) {
        if (cb) cb(false, {}, "Disc not found in MusicBrainz database");
        active_.store(false);
        return;
    }

    MBRelease release = parseJson(body);
    if (release.title.empty() && release.tracks.empty()) {
        if (cb) cb(false, {}, "Could not parse MusicBrainz response");
        active_.store(false);
        return;
    }

    if (cb) cb(true, release, "");
    active_.store(false);
}

// ─── Public API ──────────────────────────────────────────────────────────────
bool MBLookup::lookup(int first_track, int last_track,
                      const std::vector<DWORD>& offsets, MBCallback cb) {
    if (active_.load()) return false;
    active_.store(true);
    cancel_.store(false);
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&MBLookup::worker, this,
                          first_track, last_track, offsets, cb);
    return true;
}

void MBLookup::cancel() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
    active_.store(false);
}

// ─── URL encoding ─────────────────────────────────────────────────────────────
std::string MBLookup::urlEncode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << c;
        else
            out << '%' << std::setw(2) << std::setfill('0') << (int)c;
    }
    return out.str();
}

// ─── Parse MusicBrainz text-search JSON ──────────────────────────────────────
// Response: { "releases": [ { "id", "title", "date", "country",
//             "artist-credit": [...], "track-count", "label-info": [...] } ] }
std::vector<MBSearchResult> MBLookup::parseSearchJson(const std::string& json_str) {
    std::vector<MBSearchResult> results;
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.contains("releases")) return results;
        for (auto& rel : j["releases"]) {
            MBSearchResult r;
            r.mbid        = rel.value("id", "");
            r.title       = rel.value("title", "");
            r.date        = rel.value("date", "");
            r.country     = rel.value("country", "");
            r.track_count = rel.value("track-count", 0);
            if (rel.contains("artist-credit") && !rel["artist-credit"].empty())
                r.artist = rel["artist-credit"][0]["artist"].value("name", "");
            if (rel.contains("label-info") && !rel["label-info"].empty()) {
                auto& li = rel["label-info"][0];
                if (li.contains("label"))
                    r.label = li["label"].value("name", "");
            }
            if (!r.mbid.empty() && !r.title.empty())
                results.push_back(std::move(r));
            if (results.size() >= 12) break;  // cap at 12 for UI comfort
        }
    } catch (...) {}
    return results;
}

// ─── Parse Discogs search JSON (fallback) ────────────────────────────────────
// Search response: { "results": [ { "id", "title" (= "Artist - Album"),
//             "year", "country", "label": [...], "resource_url" } ] }
// NOTE: the search endpoint does NOT include a tracklist — only summary fields.
// Track titles come from the release-detail endpoint (see discogsReleaseWorker).
std::vector<MBSearchResult> MBLookup::parseDiscogsJson(const std::string& json_str) {
    std::vector<MBSearchResult> results;
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.contains("results")) return results;
        for (auto& rel : j["results"]) {
            MBSearchResult r;
            r.from_discogs = true;
            // Discogs encodes "Artist - Title" in the title field
            std::string full = rel.value("title", "");
            auto dash = full.find(" - ");
            if (dash != std::string::npos) {
                r.artist = full.substr(0, dash);
                r.title  = full.substr(dash + 3);
            } else {
                r.title = full;
            }
            r.date    = rel.value("year", "");
            r.country = rel.value("country", "");
            // Discogs numeric ID — prefix so it's visually distinct from MBIDs
            r.mbid = "discogs:" + std::to_string(rel.value("id", 0));
            if (rel.contains("label") && rel["label"].is_array() && !rel["label"].empty())
                r.label = rel["label"][0].get<std::string>();
            if (!r.title.empty())
                results.push_back(std::move(r));
            if (results.size() >= 8) break;
        }
    } catch (...) {}
    return results;
}

// ─── Text search worker ───────────────────────────────────────────────────────
void MBLookup::searchWorker(std::string artist, std::string album,
                             MBSearchCallback cb) {
    // Rate-limit
    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_request_).count();
        if (elapsed < 1000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed));
    }
    if (cancel_.load()) { active_.store(false); return; }

    // Build MB search URL
    // Lucene query: artist:"<artist>" AND release:"<album>"
    std::string query = "artist:\"" + artist + "\" AND release:\"" + album + "\"";
    std::string url   = "https://musicbrainz.org/ws/2/release/?query="
                        + urlEncode(query)
                        + "&fmt=json&limit=12&inc=artist-credits+labels";

    std::string body = httpGet(url);
    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        last_request_ = std::chrono::steady_clock::now();
    }

    if (cancel_.load()) { active_.store(false); return; }

    std::vector<MBSearchResult> results;
    std::string source = "MusicBrainz";

    if (!body.empty() && body.find("\"error\"") == std::string::npos)
        results = parseSearchJson(body);

    // ── Discogs fallback if MB returned nothing ───────────────────────────────
    if (results.empty() && !cancel_.load()) {
        // Brief extra rate-limit pause before hitting a different API
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::string durl = "https://api.discogs.com/database/search?artist="
                         + urlEncode(artist)
                         + "&release_title=" + urlEncode(album)
                         + "&type=release&per_page=8";

        std::string dbody = httpGet(durl);
        if (!dbody.empty())
            results = parseDiscogsJson(dbody);

        if (!results.empty())
            source = "Discogs";
    }

    if (results.empty()) {
        if (cb) cb(false, {}, "No results found on MusicBrainz or Discogs");
    } else {
        if (cb) cb(true, std::move(results), source);
    }
    active_.store(false);
}

// ─── MBID direct-fetch worker ─────────────────────────────────────────────────
void MBLookup::mbidWorker(std::string mbid, MBCallback cb) {
    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_request_).count();
        if (elapsed < 500)
            std::this_thread::sleep_for(std::chrono::milliseconds(500 - elapsed));
    }
    if (cancel_.load()) { active_.store(false); return; }

    std::string url = "https://musicbrainz.org/ws/2/release/" + mbid
                    + "?fmt=json&inc=recordings+artist-credits";
    std::string body = httpGet(url);
    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        last_request_ = std::chrono::steady_clock::now();
    }

    if (body.empty()) {
        if (cb) cb(false, {}, "Network error fetching release");
        active_.store(false);
        return;
    }
    if (body.find("\"error\"") != std::string::npos) {
        if (cb) cb(false, {}, "Release MBID not found");
        active_.store(false);
        return;
    }
    // Parse the single-release response directly (no wrapping needed)
    MBRelease release;
    try {
        auto j = nlohmann::json::parse(body);
        release.mb_id  = j.value("id", mbid);
        release.title  = j.value("title", "");
        release.date   = j.value("date", "");
        if (j.contains("artist-credit") && !j["artist-credit"].empty())
            release.artist = j["artist-credit"][0]["artist"].value("name", "");
        if (j.contains("media")) {
            int disc_no = 0;
            for (auto& media : j["media"]) {
                ++disc_no;
                if (!media.contains("tracks")) continue;
                for (auto& t : media["tracks"]) {
                    MBTrack mt;
                    // "position" is the integer track index (1-based), always present.
                    // "number" is a display string that may be non-numeric (vinyl: "A1").
                    mt.number = t.value("position", 0);
                    mt.title  = t.value("title", "");
                    mt.disc   = disc_no;
                    if (t.contains("recording")) {
                        auto& rec = t["recording"];
                        if (rec.contains("artist-credit") && !rec["artist-credit"].empty())
                            mt.artist = rec["artist-credit"][0]["artist"].value("name", "");
                    }
                    if (!mt.title.empty())
                        release.tracks.push_back(std::move(mt));
                }
            }
        }
    } catch (const std::exception& e) {
        if (cb) cb(false, {}, std::string("Parse error: ") + e.what());
        active_.store(false);
        return;
    } catch (...) {
        if (cb) cb(false, {}, "Could not parse release data");
        active_.store(false);
        return;
    }
    if (release.title.empty()) {
        if (cb) cb(false, {}, "Could not parse release data");
        active_.store(false);
        return;
    }
    if (cb) cb(true, release, "");
    active_.store(false);
}

// ─── Public: search ──────────────────────────────────────────────────────────
bool MBLookup::search(const std::string& artist, const std::string& album,
                      MBSearchCallback cb) {
    if (active_.load()) return false;
    active_.store(true);
    cancel_.store(false);
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&MBLookup::searchWorker, this, artist, album, cb);
    return true;
}

// ─── Public: lookupByMbid ────────────────────────────────────────────────────
bool MBLookup::lookupByMbid(const std::string& mbid, MBCallback cb) {
    if (active_.load()) return false;
    active_.store(true);
    cancel_.store(false);
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&MBLookup::mbidWorker, this, mbid, cb);
    return true;
}

// ─── Public: lookupDiscogsRelease ────────────────────────────────────────────
bool MBLookup::lookupDiscogsRelease(const std::string& discogs_id, MBCallback cb) {
    if (active_.load()) return false;
    active_.store(true);
    cancel_.store(false);
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&MBLookup::discogsReleaseWorker, this, discogs_id, cb);
    return true;
}

// ─── Discogs release-detail worker ───────────────────────────────────────────
// The Discogs /database/search endpoint returns NO tracklist — only summary
// fields. The tracklist lives on the release-detail endpoint. This mirrors
// mbidWorker: GET /releases/{id}, parse the `tracklist` array, and feed the
// same MBCallback pipeline so a Discogs pick populates tracks like an MB pick.
//
// Discogs quirks handled here:
//   • positions are non-numeric on many releases ("A1", "1-2") — we renumber
//     sequentially so they line up with CD track numbers.
//   • tracklists contain "heading"/"index" rows (section titles, medley
//     containers) with no audio — filtered out by type_.
//   • the release-level artist name often carries a " (N)" disambiguator that
//     Discogs appends for duplicate names — stripped for display.
//   • per-track artists exist on compilations — captured when present.
void MBLookup::discogsReleaseWorker(std::string discogs_id, MBCallback cb) {
    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_request_).count();
        if (elapsed < 1000)   // Discogs throttles harder than MB; pace at 1s
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed));
    }
    if (cancel_.load()) { active_.store(false); return; }

    std::string url  = "https://api.discogs.com/releases/" + discogs_id;
    std::string body = httpGet(url);
    {
        std::lock_guard<std::mutex> lk(rate_mutex_);
        last_request_ = std::chrono::steady_clock::now();
    }

    if (body.empty()) {
        if (cb) cb(false, {}, "Network error fetching Discogs release");
        active_.store(false);
        return;
    }

    MBRelease release;
    try {
        auto j = nlohmann::json::parse(body);

        // Discogs 404 / error payloads carry a "message" and no tracklist.
        if (!j.contains("tracklist") || !j["tracklist"].is_array()) {
            std::string msg = j.value("message", "Discogs release has no tracklist");
            if (cb) cb(false, {}, msg);
            active_.store(false);
            return;
        }

        release.mb_id = "";   // no Cover Art Archive ID for Discogs releases
        release.title = j.value("title", "");

        // "year" is an int in the release endpoint (sometimes a string).
        if (j.contains("year")) {
            if (j["year"].is_number_integer()) {
                int y = j["year"].get<int>();
                if (y > 0) release.date = std::to_string(y);
            } else if (j["year"].is_string()) {
                release.date = j["year"].get<std::string>();
            }
        }

        // Release-level artist, with Discogs " (N)" disambiguator stripped.
        if (j.contains("artists") && j["artists"].is_array() && !j["artists"].empty()) {
            std::string a = j["artists"][0].value("name", "");
            auto pos = a.rfind(" (");
            if (pos != std::string::npos && !a.empty() && a.back() == ')') {
                bool all_digits = (pos + 2 < a.size());
                for (size_t k = pos + 2; k + 1 < a.size(); ++k)
                    if (a[k] < '0' || a[k] > '9') { all_digits = false; break; }
                if (all_digits) a = a.substr(0, pos);
            }
            release.artist = a;
        }

        // Tracklist — real tracks only, renumbered sequentially.
        int seq = 0;
        for (auto& t : j["tracklist"]) {
            std::string type = t.value("type_", "track");
            if (type != "track") continue;   // skip heading / index rows
            MBTrack mt;
            mt.number = ++seq;
            mt.title  = t.value("title", "");
            if (t.contains("artists") && t["artists"].is_array() && !t["artists"].empty())
                mt.artist = t["artists"][0].value("name", "");
            if (!mt.title.empty())
                release.tracks.push_back(std::move(mt));
        }
    } catch (const std::exception& e) {
        if (cb) cb(false, {}, std::string("Parse error: ") + e.what());
        active_.store(false);
        return;
    } catch (...) {
        if (cb) cb(false, {}, "Could not parse Discogs release");
        active_.store(false);
        return;
    }

    if (release.tracks.empty()) {
        if (cb) cb(false, {}, "Discogs release had no usable tracks");
        active_.store(false);
        return;
    }

    if (cb) cb(true, release, "");
    active_.store(false);
}

#endif // _WIN32
