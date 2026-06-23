#ifdef _WIN32

#include "ListenBrainz.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <cstdarg>
#include <cstdio>
#include "json.hpp"

#pragma comment(lib, "wininet.lib")

static const char* kUA   = "RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)";
static const char* kHost = "api.listenbrainz.org";

// printf-style helper that funnels into the shared operational log.
static void lblog(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log::write("listenbrainz", buf);
}

// ─── Payload builder ─────────────────────────────────────────────────────────
// Verified in isolation against the live JSON lib (single carries listened_at,
// playing_now omits it, release_name only when album present, special chars
// round-trip). Keep this in sync with the standalone builder test.
std::string ListenBrainz::buildSubmitBody(const std::string& listen_type,
                                          const std::string& artist, const std::string& track,
                                          const std::string& album, const long* listened_at) {
    nlohmann::json track_meta;
    track_meta["artist_name"] = artist;
    track_meta["track_name"]  = track;
    if (!album.empty()) track_meta["release_name"] = album;

    nlohmann::json addl;
    addl["submission_client"]         = "RE-MOCT";
    addl["submission_client_version"] = "1.0.0-rc1";
    track_meta["additional_info"]     = addl;

    nlohmann::json listen;
    listen["track_metadata"] = track_meta;
    if (listened_at) listen["listened_at"] = *listened_at;   // single only

    nlohmann::json body;
    body["listen_type"] = listen_type;
    body["payload"]     = nlohmann::json::array({ listen });
    return body.dump();
}

// ─── HTTP ──────────────────────────────────────────────────────────────────
std::string ListenBrainz::httpPost(const std::string& path, const std::string& body,
                                   const std::string& token, long* http_status) {
    if (http_status) *http_status = 0;
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

    std::string hdr = "Content-Type: application/json\r\nAuthorization: Token " + token + "\r\n";
    std::string resp;
    if (HttpSendRequestA(req, hdr.c_str(), (DWORD)-1L,
                         (LPVOID)body.data(), (DWORD)body.size())) {
        if (http_status) {
            DWORD code = 0, len = sizeof(code);
            HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &code, &len, nullptr);
            *http_status = (long)code;
        }
        char buf[2048]; DWORD got = 0;
        while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0)
            resp.append(buf, got);
    } else {
        lblog("httpPost: HttpSendRequest failed err=%lu", GetLastError());
    }
    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return resp;
}

std::string ListenBrainz::httpGet(const std::string& path,
                                  const std::string& token, long* http_status) {
    if (http_status) *http_status = 0;
    HINTERNET inet = InternetOpenA(kUA, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) return {};
    DWORD t = 8000;
    InternetSetOptionA(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    InternetSetOptionA(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));

    HINTERNET conn = InternetConnectA(inet, kHost, INTERNET_DEFAULT_HTTPS_PORT,
                                      nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { InternetCloseHandle(inet); return {}; }

    HINTERNET req = HttpOpenRequestA(conn, "GET", path.c_str(), nullptr, nullptr, nullptr,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!req) { InternetCloseHandle(conn); InternetCloseHandle(inet); return {}; }

    std::string hdr = "Authorization: Token " + token + "\r\n";
    std::string resp;
    if (HttpSendRequestA(req, hdr.c_str(), (DWORD)-1L, nullptr, 0)) {
        if (http_status) {
            DWORD code = 0, len = sizeof(code);
            HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &code, &len, nullptr);
            *http_status = (long)code;
        }
        char buf[2048]; DWORD got = 0;
        while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0)
            resp.append(buf, got);
    } else {
        lblog("httpGet: HttpSendRequest failed err=%lu", GetLastError());
    }
    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return resp;
}

// A submit is accepted on HTTP 200; some responses also carry {"status":"ok"}.
// Treat either signal as success so a missed status read doesn't false-negative.
static bool accepted(long status, const std::string& resp) {
    if (status == 200) return true;
    if (resp.empty()) return false;
    try {
        auto j = nlohmann::json::parse(resp);
        return j.value("status", std::string()) == "ok";
    } catch (...) { return false; }
}

// ─── Public API ──────────────────────────────────────────────────────────────
bool ListenBrainz::validateToken(const std::string& token, std::string& user_out) {
    if (token.empty()) return false;
    long status = 0;
    std::string resp = httpGet("/1/validate-token", token, &status);
    lblog("validate-token status=%ld resp=%s", status, resp.empty() ? "(empty)" : resp.c_str());
    if (resp.empty()) return false;
    try {
        auto j = nlohmann::json::parse(resp);
        bool valid = j.value("valid", false);
        if (valid) user_out = j.value("user_name", std::string());
        return valid;
    } catch (...) { return false; }
}

bool ListenBrainz::playingNow(const std::string& token,
                              const std::string& artist, const std::string& track,
                              const std::string& album) {
    if (token.empty() || artist.empty() || track.empty()) return false;
    std::string body = buildSubmitBody("playing_now", artist, track, album, nullptr);
    long status = 0;
    std::string resp = httpPost("/1/submit-listens", body, token, &status);
    bool ok = accepted(status, resp);
    lblog("playing_now %s status=%ld resp=%s", ok ? "OK" : "FAIL", status,
          resp.empty() ? "(empty)" : resp.c_str());
    return ok;
}

bool ListenBrainz::submitSingle(const std::string& token,
                                const std::string& artist, const std::string& track,
                                long listened_at, const std::string& album) {
    if (token.empty() || artist.empty() || track.empty()) return false;
    std::string body = buildSubmitBody("single", artist, track, album, &listened_at);
    long status = 0;
    std::string resp = httpPost("/1/submit-listens", body, token, &status);
    bool ok = accepted(status, resp);
    lblog("single %s status=%ld ts=%ld resp=%s", ok ? "OK" : "FAIL", status, listened_at,
          resp.empty() ? "(empty)" : resp.c_str());
    return ok;
}

#endif // _WIN32
