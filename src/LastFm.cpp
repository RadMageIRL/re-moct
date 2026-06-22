#ifdef _WIN32

#include "LastFm.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include "json.hpp"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "advapi32.lib")

// Diagnostic log — shares %TEMP%\remoct_stream.log with the streaming layer.
static void lflog(const char* fmt, ...) {
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string p = (n > 0 && n < MAX_PATH) ? std::string(tmp) + "remoct_stream.log"
                                            : std::string("remoct_stream.log");
    FILE* f = std::fopen(p.c_str(), "a");
    if (!f) return;
    std::fputs("[lastfm] ", f);
    va_list ap; va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

static const char* kUA  = "RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)";
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

// ─── HTTP ──────────────────────────────────────────────────────────────────
std::string LastFm::httpGet(const std::string& url) {
    HINTERNET inet = InternetOpenA(kUA, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) return {};
    DWORD t = 8000;
    InternetSetOptionA(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    InternetSetOptionA(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    HINTERNET conn = InternetOpenUrlA(inet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!conn) { InternetCloseHandle(inet); return {}; }
    std::string body; char buf[2048]; DWORD got = 0;
    while (InternetReadFile(conn, buf, sizeof(buf), &got) && got > 0)
        body.append(buf, got);
    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return body;
}

std::string LastFm::httpPost(const std::string& path, const std::string& body) {
    HINTERNET inet = InternetOpenA(kUA, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) return {};
    DWORD t = 8000;
    InternetSetOptionA(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    InternetSetOptionA(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));

    HINTERNET conn = InternetConnectA(inet, kHost, INTERNET_DEFAULT_HTTPS_PORT,
                                      nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { InternetCloseHandle(inet); return {}; }

    HINTERNET req = HttpOpenRequestA(conn, "POST", path.c_str(), nullptr, nullptr, nullptr,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!req) { InternetCloseHandle(conn); InternetCloseHandle(inet); return {}; }

    const char* hdr = "Content-Type: application/x-www-form-urlencoded\r\n";
    std::string resp;
    if (HttpSendRequestA(req, hdr, (DWORD)-1L,
                         (LPVOID)body.data(), (DWORD)body.size())) {
        char buf[2048]; DWORD got = 0;
        while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0)
            resp.append(buf, got);
    } else {
        lflog("httpPost: HttpSendRequest failed err=%lu", GetLastError());
    }
    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return resp;
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
