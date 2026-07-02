#ifdef _WIN32

#include "LastFm.h"
#include "Log.h"
#include "core/IHttp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>       // CryptoAPI (MD5 api_sig signing) still needs this
#include <wincrypt.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include "json.hpp"


// Diagnostic log — routed through the shared operational logger (Log).
static void lflog(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log::write("lastfm", buf);
}

static const char* kHost = "ws.audioscrobbler.com";
static const char* kPath = "/2.0/";

// ─── MD5 via Windows CryptoAPI (guaranteed-correct; no hand-rolled hash) ───────
std::string LastFm::md5Hex(const std::string& s) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
            if (CryptHashData(hash, reinterpret_cast<const BYTE*>(s.data()),
                              (DWORD)s.size(), 0)) {
                BYTE  digest[16];
                DWORD len = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
                    static const char* hx = "0123456789abcdef";
                    for (int i = 0; i < 16; ++i) {
                        out += hx[digest[i] >> 4];
                        out += hx[digest[i] & 0x0F];
                    }
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    return out;
}

// api_sig = md5( for each param sorted by name: name+value ... then + secret )
std::string LastFm::sign(Params& params, const std::string& secret) {
    std::sort(params.begin(), params.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string s;
    for (const auto& [k, v] : params) { s += k; s += v; }
    s += secret;
    return md5Hex(s);
}

std::string LastFm::urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F]; }
    }
    return out;
}

// ─── HTTP via the core::IHttp seam (WinINet impl) ────────────────────────────
// Signing + body assembly stay here (md5Hex / sign / postWriteCall); only transport
// moved. Parity: UA is the seam default (byte-identical to the app UA), 8 s
// connect+receive timeout, HTTPS from scheme, HTTP status ignored (writes gate on
// the JSON `error` field in postWriteCall).
std::string LastFm::httpGet(const std::string& url) {
    core::HttpRequest req;
    req.url        = url;
    req.timeout_ms = 8000;
    return core::http().fetch(req).body;
}

std::string LastFm::httpPost(const std::string& path, const std::string& body) {
    core::HttpRequest req;
    req.method       = "POST";
    req.url          = std::string("https://") + kHost + path;   // ws.audioscrobbler.com + /2.0/
    req.body         = body;
    req.content_type = "application/x-www-form-urlencoded";
    req.timeout_ms   = 8000;
    return core::http().fetch(req).body;
}

// Build the form body from signed params + api_sig + format=json, POST it,
// and treat a response without an "error" field as success.
bool LastFm::postWriteCall(const Params& signed_params, const std::string& api_sig) {
    std::string body;
    for (const auto& [k, v] : signed_params)
        body += urlEncode(k) + "=" + urlEncode(v) + "&";
    body += "api_sig=" + api_sig + "&format=json";

    std::string resp = httpPost(kPath, body);
    lflog("write resp: %s", resp.empty() ? "(empty/connect-fail)" : resp.c_str());
    if (resp.empty()) return false;
    try {
        auto j = nlohmann::json::parse(resp);
        if (j.contains("error")) {
            lflog("write rejected: error=%d", j.value("error", 0));
            return false;
        }
        return true;
    } catch (...) { return false; }
}

// ─── Auth ──────────────────────────────────────────────────────────────────
bool LastFm::requestToken(const std::string& api_key, const std::string& secret,
                          std::string& token_out, std::string& authorize_url_out) {
    Params p = { {"api_key", api_key}, {"method", "auth.getToken"} };
    std::string sig = sign(p, secret);
    std::string url = std::string("https://") + kHost + kPath
        + "?method=auth.getToken&api_key=" + api_key
        + "&api_sig=" + sig + "&format=json";
    std::string body = httpGet(url);
    lflog("requestToken resp: %s", body.empty() ? "(empty)" : body.c_str());
    if (body.empty()) return false;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("token")) return false;
        token_out = j["token"].get<std::string>();
    } catch (...) { return false; }
    if (token_out.empty()) return false;
    authorize_url_out = "https://www.last.fm/api/auth/?api_key=" + api_key
                      + "&token=" + token_out;
    return true;
}

bool LastFm::getSession(const std::string& api_key, const std::string& secret,
                        const std::string& token,
                        std::string& session_key_out, std::string& username_out) {
    Params p = { {"api_key", api_key}, {"method", "auth.getSession"}, {"token", token} };
    std::string sig = sign(p, secret);
    std::string url = std::string("https://") + kHost + kPath
        + "?method=auth.getSession&api_key=" + api_key
        + "&token=" + token + "&api_sig=" + sig + "&format=json";
    std::string body = httpGet(url);
    lflog("getSession resp: %s", body.empty() ? "(empty)" : body.c_str());
    if (body.empty()) return false;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("session")) return false;
        session_key_out = j["session"].value("key",  std::string());
        username_out    = j["session"].value("name", std::string());
    } catch (...) { return false; }
    return !session_key_out.empty();
}

// ─── Scrobbling ──────────────────────────────────────────────────────────────
bool LastFm::updateNowPlaying(const std::string& api_key, const std::string& secret,
                              const std::string& session_key,
                              const std::string& artist, const std::string& track,
                              const std::string& album) {
    if (artist.empty() || track.empty()) return false;
    Params p = {
        {"api_key", api_key}, {"method", "track.updateNowPlaying"},
        {"sk", session_key}, {"artist", artist}, {"track", track},
    };
    if (!album.empty()) p.push_back({"album", album});
    std::string sig = sign(p, secret);   // sorts p
    return postWriteCall(p, sig);
}

bool LastFm::scrobble(const std::string& api_key, const std::string& secret,
                      const std::string& session_key,
                      const std::string& artist, const std::string& track,
                      long timestamp, bool chosen_by_user,
                      const std::string& album) {
    if (artist.empty() || track.empty()) return false;
    Params p = {
        {"api_key", api_key}, {"method", "track.scrobble"},
        {"sk", session_key}, {"artist", artist}, {"track", track},
        {"timestamp", std::to_string(timestamp)},
    };
    if (!album.empty())   p.push_back({"album", album});
    if (!chosen_by_user)  p.push_back({"chosenByUser", "0"});  // e.g. radio
    std::string sig = sign(p, secret);
    return postWriteCall(p, sig);
}

#endif // _WIN32
