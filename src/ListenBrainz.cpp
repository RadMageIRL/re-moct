
#include "ListenBrainz.h"
#include "Log.h"
#include "Version.h"
#include "core/IHttp.h"

#include <cstdarg>
#include <cstdio>
#include "json.hpp"


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
    addl["submission_client_version"] = REMOCT_VERSION;
    track_meta["additional_info"]     = addl;

    nlohmann::json listen;
    listen["track_metadata"] = track_meta;
    if (listened_at) listen["listened_at"] = *listened_at;   // single only

    nlohmann::json body;
    body["listen_type"] = listen_type;
    body["payload"]     = nlohmann::json::array({ listen });
    return body.dump();
}

// ─── HTTP via the core::IHttp seam (WinINet impl) ────────────────────────────
// buildSubmitBody + accepted() stay here; only transport moved. Parity: UA is the
// seam default (byte-identical to the app UA), Content-Type application/json,
// Authorization: Token header, 8 s connect+receive timeout. The HTTP status is
// surfaced back through http_status — accepted() gates on status==200 (JSON fallback),
// so status is load-bearing here (unlike LastFm, which gates on the JSON error field).
std::string ListenBrainz::httpPost(const std::string& path, const std::string& body,
                                   const std::string& token, long* http_status) {
    core::HttpRequest req;
    req.method       = "POST";
    req.url          = std::string("https://") + kHost + path;
    req.body         = body;
    req.content_type = "application/json";
    req.headers.push_back({"Authorization", "Token " + token});
    req.timeout_ms   = 8000;
    core::HttpResponse r = core::http().fetch(req);
    if (http_status) *http_status = r.status;
    return r.body;
}

std::string ListenBrainz::httpGet(const std::string& path,
                                  const std::string& token, long* http_status) {
    core::HttpRequest req;
    req.method     = "GET";
    req.url        = std::string("https://") + kHost + path;
    req.headers.push_back({"Authorization", "Token " + token});
    req.timeout_ms = 8000;
    core::HttpResponse r = core::http().fetch(req);
    if (http_status) *http_status = r.status;
    return r.body;
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

