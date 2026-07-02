// CoverArt — album-cover resolution, extracted from CDRipper so any subsystem can
// resolve art (URL or bytes) without pulling in the CD ripper. Sources: Cover Art
// Archive (by MusicBrainz id), iTunes and Deezer (by free-text artist/title).
//
// Two families share one HTTP helper:
//   bytesBy* -> downloaded image bytes, for embedding into ripped-file tags
//   urlBy*   -> external image URL, for Discord Rich Presence (which takes a URL)
//
// All consumer-facing functions do BLOCKING HTTP (WinINet) — call off the UI thread.
// (Global HTTP-helper consolidation across the tree's ~8 WinINet sites is a separate
//  refactor; this module keeps its own httpGet, matching the current per-module pattern.)

#include "CoverArt.h"
#include "IHttp.h"              // core::IHttp seam (transport); no windows.h/wininet here
#include "json.hpp"             // nlohmann single-header (vendored)
#include <string>
#include <vector>
#include <cstdint>

namespace CoverArt {

// True only if the bytes begin with a known raster-image signature. Used to gate
// the cover-art path so an HTML/JSON error body returned with a 200 (or with an
// unreadable status code) is never embedded into the files as "cover art".
static bool looksLikeImage(const std::vector<uint8_t>& d) {
    if (d.size() < 12) return false;
    const uint8_t* b = d.data();
    if (b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF) return true;                  // JPEG
    if (b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G') return true;      // PNG
    if (b[0]=='G' && b[1]=='I' && b[2]=='F' && b[3]=='8') return true;       // GIF
    if (b[0]=='B' && b[1]=='M') return true;                                 // BMP
    if (b[0]=='R' && b[1]=='I' && b[2]=='F' && b[3]=='F' &&
        b[8]=='W' && b[9]=='E' && b[10]=='B' && b[11]=='P') return true;     // WEBP
    return false;
}
std::vector<uint8_t> bytesByMbid(const std::string& mb_id) {
    if (mb_id.empty()) return {};
    for (const auto& url : {
            "https://coverartarchive.org/release/"+mb_id+"/front-500",
            "https://coverartarchive.org/release/"+mb_id+"/front"}) {
        core::HttpRequest req;
        req.url              = url;
        req.max_body         = 10u * 1024 * 1024;   // bound, then reject/clear on cap
        req.reject_truncated = true;                // never embed a truncated cover
        req.redirect         = core::RedirectPolicy::FollowSameScheme;  // CAA: no http<->https downgrade
        // user_agent left empty -> seam default (full UA), matching the old bytesByMbid.
        core::HttpResponse r = core::http().fetch(req);
        if (r.status == 200) {   // require a real 200, not an unreadable/redirect status
            std::vector<uint8_t> data(r.body.begin(), r.body.end());
            // A capped read was cleared by the seam (reject_truncated), so a truncated
            // body arrives empty here -> looksLikeImage() fails and we never embed it.
            if (looksLikeImage(data)) return data;
        }
    }
    return {};
}

// Minimal GET into a byte buffer via the core::IHttp seam. true on HTTP 200 (or unknown
// status) with data. 10 MB cap -> reject/clear (a truncated body is unusable). Per-call UA.
static bool httpGet(const std::string& url, std::vector<uint8_t>& out,
                          const std::string& user_agent) {
    out.clear();
    core::HttpRequest req;
    req.url              = url;
    req.user_agent       = user_agent;
    req.max_body         = 10u * 1024 * 1024;
    req.reject_truncated = true;
    core::HttpResponse r = core::http().fetch(req);
    if (r.status == 200 || r.status == 0)          // accept 200 or unknown, as before
        out.assign(r.body.begin(), r.body.end());
    return !out.empty();
}

// Open, no-auth cover-art fallback keyed by artist + album text.
// Runs ONLY when Cover Art Archive returns nothing: Discogs releases have no
// MBID (so CAA can't be queried), and a few MB releases lack a CAA front cover.
// Tries iTunes first, then Deezer. Neither needs a key. Returns bytes or {}.
//
// IMPORTANT: a free-text music search will happily return a *different artist's*
// album when the exact title isn't in the catalogue (e.g. a Crystals comp that
// isn't on iTunes returns a Doors "Very Best Of"). So we VERIFY: a candidate is
// accepted only if its artist matches the requested artist, and among matching
// candidates we pick the one whose album title best overlaps the request. If no
// candidate's artist matches, we return {} — no art beats wrong-artist art.
std::vector<uint8_t> bytesByText(const std::string& artist,
                                           const std::string& album) {
    if (artist.empty() && album.empty()) return {};

    // Query-term encoder (spaces -> '+', RFC-3986 percent-encoding otherwise).
    auto enc = [](const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : s) {
            if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                c=='-'||c=='_'||c=='.'||c=='~') o += (char)c;
            else if (c==' ') o += '+';
            else { o += '%'; o += hex[c>>4]; o += hex[c&0xF]; }
        }
        return o;
    };

    // Normalize for comparison: lowercase alphanumerics, single spaces between
    // tokens, leading "the " dropped ("The Crystals" == "Crystals").
    auto norm = [](const std::string& s) {
        std::string o; bool sp = false;
        for (char c : s) {
            bool aln = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');
            if (aln) { o += (c>='A'&&c<='Z') ? (char)(c + 32) : c; sp = false; }
            else if (!sp && !o.empty()) { o += ' '; sp = true; }
        }
        while (!o.empty() && o.back() == ' ') o.pop_back();
        if (o.rfind("the ", 0) == 0) o.erase(0, 4);
        return o;
    };
    auto artist_ok = [&](const std::string& want, const std::string& got) {
        std::string a = norm(want), b = norm(got);
        if (a.empty() || b.empty()) return false;
        return a == b || a.find(b) != std::string::npos
                      || b.find(a) != std::string::npos;
    };
    auto album_overlap = [&](const std::string& want, const std::string& got) {
        auto toks = [](const std::string& n) {
            std::vector<std::string> t; std::string cur;
            for (char c : n) { if (c==' ') { if(!cur.empty()){t.push_back(cur);cur.clear();} }
                               else cur += c; }
            if (!cur.empty()) t.push_back(cur);
            return t;
        };
        auto ta = toks(norm(want)), tb = toks(norm(got));
        int shared = 0;
        for (auto& x : ta) for (auto& y : tb) if (x == y) { ++shared; break; }
        return shared;
    };

    const std::string term = enc(artist + " " + album);
    std::vector<uint8_t> img;

    // ── iTunes Search API ──────────────────────────────────────────────────
    {
        std::string url = "https://itunes.apple.com/search?term=" + term
                        + "&entity=album&limit=10";
        std::vector<uint8_t> body;
        if (httpGet(url, body, "RE-MOCT/1.0.0-rc1")) {
            try {
                auto j = nlohmann::json::parse(
                             std::string((char*)body.data(), body.size()));
                std::string best_url; int best_score = -1;
                if (j.contains("results"))
                for (auto& r : j["results"]) {
                    if (!artist_ok(artist, r.value("artistName", ""))) continue;
                    std::string aurl = r.value("artworkUrl100", "");
                    if (aurl.empty()) continue;
                    int score = album_overlap(album, r.value("collectionName", ""));
                    if (score > best_score) { best_score = score; best_url = aurl; }
                }
                if (!best_url.empty()) {
                    auto pos = best_url.rfind("100x100");   // bump to 600x600
                    if (pos != std::string::npos) best_url.replace(pos, 7, "600x600");
                    if (httpGet(best_url, img, "RE-MOCT/1.0.0-rc1") && looksLikeImage(img))
                        return img;
                    img.clear();
                }
            } catch (...) {}
        }
    }

    // ── Deezer API (fallback) ──────────────────────────────────────────────
    {
        std::string url = "https://api.deezer.com/search/album?q=" + term
                        + "&limit=10";
        std::vector<uint8_t> body;
        if (httpGet(url, body, "RE-MOCT/1.0.0-rc1")) {
            try {
                auto j = nlohmann::json::parse(
                             std::string((char*)body.data(), body.size()));
                std::string best_url; int best_score = -1;
                if (j.contains("data"))
                for (auto& r : j["data"]) {
                    std::string a;
                    if (r.contains("artist") && r["artist"].contains("name"))
                        a = r["artist"]["name"].get<std::string>();
                    if (!artist_ok(artist, a)) continue;
                    std::string aurl = r.value("cover_xl", "");
                    if (aurl.empty()) aurl = r.value("cover_big", "");
                    if (aurl.empty()) continue;
                    int score = album_overlap(album, r.value("title", ""));
                    if (score > best_score) { best_score = score; best_url = aurl; }
                }
                if (!best_url.empty()) {
                    if (httpGet(best_url, img, "RE-MOCT/1.0.0-rc1") && looksLikeImage(img))
                        return img;
                    img.clear();
                }
            } catch (...) {}
        }
    }

    return {};
}

// URL-only variant of bytesByText: same iTunes->Deezer search and matching,
// but returns the best cover URL instead of downloading it. For Discord Rich
// Presence, which takes an external image URL. Leaves bytesByText untouched.
std::string urlByText(const std::string& artist,
                                        const std::string& album) {
    if (artist.empty() && album.empty()) return {};

    auto enc = [](const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : s) {
            if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                c=='-'||c=='_'||c=='.'||c=='~') o += (char)c;
            else if (c==' ') o += '+';
            else { o += '%'; o += hex[c>>4]; o += hex[c&0xF]; }
        }
        return o;
    };
    auto norm = [](const std::string& s) {
        std::string o; bool sp = false;
        for (char c : s) {
            bool aln = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');
            if (aln) { o += (c>='A'&&c<='Z') ? (char)(c + 32) : c; sp = false; }
            else if (!sp && !o.empty()) { o += ' '; sp = true; }
        }
        while (!o.empty() && o.back() == ' ') o.pop_back();
        if (o.rfind("the ", 0) == 0) o.erase(0, 4);
        return o;
    };
    auto artist_ok = [&](const std::string& want, const std::string& got) {
        std::string a = norm(want), b = norm(got);
        if (a.empty() || b.empty()) return false;
        return a == b || a.find(b) != std::string::npos
                      || b.find(a) != std::string::npos;
    };
    auto album_overlap = [&](const std::string& want, const std::string& got) {
        auto toks = [](const std::string& n) {
            std::vector<std::string> t; std::string cur;
            for (char c : n) { if (c==' ') { if(!cur.empty()){t.push_back(cur);cur.clear();} }
                               else cur += c; }
            if (!cur.empty()) t.push_back(cur);
            return t;
        };
        auto ta = toks(norm(want)), tb = toks(norm(got));
        int shared = 0;
        for (auto& x : ta) for (auto& y : tb) if (x == y) { ++shared; break; }
        return shared;
    };

    const std::string term = enc(artist + " " + album);

    // ── iTunes Search API ──────────────────────────────────────────────────
    {
        std::string url = "https://itunes.apple.com/search?term=" + term
                        + "&entity=album&limit=10";
        std::vector<uint8_t> body;
        if (httpGet(url, body, "RE-MOCT/1.0.0-rc1")) {
            try {
                auto j = nlohmann::json::parse(
                             std::string((char*)body.data(), body.size()));
                std::string best_url; int best_score = -1;
                if (j.contains("results"))
                for (auto& r : j["results"]) {
                    if (!artist_ok(artist, r.value("artistName", ""))) continue;
                    std::string aurl = r.value("artworkUrl100", "");
                    if (aurl.empty()) continue;
                    int score = album_overlap(album, r.value("collectionName", ""));
                    if (score > best_score) { best_score = score; best_url = aurl; }
                }
                if (!best_url.empty()) {
                    auto pos = best_url.rfind("100x100");   // bump to 600x600
                    if (pos != std::string::npos) best_url.replace(pos, 7, "600x600");
                    return best_url;
                }
            } catch (...) {}
        }
    }

    // ── Deezer API (fallback) ──────────────────────────────────────────────
    {
        std::string url = "https://api.deezer.com/search/album?q=" + term
                        + "&limit=10";
        std::vector<uint8_t> body;
        if (httpGet(url, body, "RE-MOCT/1.0.0-rc1")) {
            try {
                auto j = nlohmann::json::parse(
                             std::string((char*)body.data(), body.size()));
                std::string best_url; int best_score = -1;
                if (j.contains("data"))
                for (auto& r : j["data"]) {
                    std::string a;
                    if (r.contains("artist") && r["artist"].contains("name"))
                        a = r["artist"]["name"].get<std::string>();
                    if (!artist_ok(artist, a)) continue;
                    std::string aurl = r.value("cover_xl", "");
                    if (aurl.empty()) aurl = r.value("cover_big", "");
                    if (aurl.empty()) continue;
                    int score = album_overlap(album, r.value("title", ""));
                    if (score > best_score) { best_score = score; best_url = aurl; }
                }
                if (!best_url.empty()) return best_url;
            } catch (...) {}
        }
    }

    return {};
}

// Resolve a cover URL for a single SONG (radio fallback path). iTunes song-entity
// search, Deezer track search as fallback. Validates BOTH artist and title (loose,
// substring-either-direction after normalisation) so a fuzzy text match can never
// paint the wrong cover. "" when there is no confident hit — the caller keeps the
// RE-MOCT logo in that case. Deliberately separate from urlByText: feeding
// a bare track title into album search + album_overlap under-hits and mis-validates.
std::string urlBySong(const std::string& artist,
                                        const std::string& title) {
    if (artist.empty() || title.empty()) return {};

    auto enc = [](const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : s) {
            if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                c=='-'||c=='_'||c=='.'||c=='~') o += (char)c;
            else if (c==' ') o += '+';
            else { o += '%'; o += hex[c>>4]; o += hex[c&0xF]; }
        }
        return o;
    };
    auto norm = [](const std::string& s) {
        std::string o; bool sp = false;
        for (char c : s) {
            bool aln = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');
            if (aln) { o += (c>='A'&&c<='Z') ? (char)(c + 32) : c; sp = false; }
            else if (!sp && !o.empty()) { o += ' '; sp = true; }
        }
        while (!o.empty() && o.back() == ' ') o.pop_back();
        if (o.rfind("the ", 0) == 0) o.erase(0, 4);
        return o;
    };
    // Substring-either-direction after normalisation: tolerates "(feat. X)",
    // "- Remastered 2011", "(Live)" tails without sinking a correct hit.
    auto loose_ok = [&](const std::string& want, const std::string& got) {
        std::string a = norm(want), b = norm(got);
        if (a.empty() || b.empty()) return false;
        return a == b || a.find(b) != std::string::npos
                      || b.find(a) != std::string::npos;
    };

    const std::string term = enc(artist + " " + title);

    // ── iTunes Search API (song entity) ────────────────────────────────────
    {
        std::string url = "https://itunes.apple.com/search?term=" + term
                        + "&entity=song&limit=10";
        std::vector<uint8_t> body;
        if (httpGet(url, body, "RE-MOCT/1.0.0-rc1")) {
            try {
                auto j = nlohmann::json::parse(
                             std::string((char*)body.data(), body.size()));
                if (j.contains("results"))
                for (auto& r : j["results"]) {
                    if (!loose_ok(artist, r.value("artistName", ""))) continue;
                    if (!loose_ok(title,  r.value("trackName",  ""))) continue;
                    std::string aurl = r.value("artworkUrl100", "");
                    if (aurl.empty()) continue;
                    auto pos = aurl.rfind("100x100");   // bump to 600x600
                    if (pos != std::string::npos) aurl.replace(pos, 7, "600x600");
                    return aurl;                         // relevance-ordered: first match wins
                }
            } catch (...) {}
        }
    }

    // ── Deezer API (track fallback) ────────────────────────────────────────
    {
        std::string url = "https://api.deezer.com/search/track?q=" + term
                        + "&limit=10";
        std::vector<uint8_t> body;
        if (httpGet(url, body, "RE-MOCT/1.0.0-rc1")) {
            try {
                auto j = nlohmann::json::parse(
                             std::string((char*)body.data(), body.size()));
                if (j.contains("data"))
                for (auto& r : j["data"]) {
                    std::string a;
                    if (r.contains("artist") && r["artist"].contains("name"))
                        a = r["artist"]["name"].get<std::string>();
                    if (!loose_ok(artist, a)) continue;
                    if (!loose_ok(title, r.value("title", ""))) continue;
                    std::string aurl;
                    if (r.contains("album")) {
                        aurl = r["album"].value("cover_xl", "");
                        if (aurl.empty()) aurl = r["album"].value("cover_big", "");
                    }
                    if (aurl.empty()) continue;
                    return aurl;
                }
            } catch (...) {}
        }
    }

    return {};
}

}  // namespace CoverArt
