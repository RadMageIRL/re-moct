// IHttp.h — platform-neutral HTTP seam (Phase 1, slice 1: GET/JSON sites).
//
// `core::IHttp` is the interface; `platform::win::WinInetHttp` (src/HttpWinInet.cpp)
// is the Windows/WinINet implementation. They live in include/ + src/ per the project
// convention today (headers in include/, sources in src/); at the later core/platform
// reorg they move into include/core/ + src/platform/win/ subdirs. The interface
// survives that move unchanged.
//
// This header must NOT include <windows.h> or any platform header — that is the
// "does core stay portable" seam test (it compiles on Linux CI as-is).
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace core {

// A single HTTP request. Slice 1 exercises GET only; the POST/body/content_type
// fields exist so the shape is proven, but are unused until a later slice migrates
// the scrobble sites. All strings are UTF-8; the platform impl widens as needed.
struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;  // extra request headers
    std::string body;                       // POST body (unused in slice 1)
    std::string content_type;               // POST content-type (unused in slice 1)
    std::string user_agent;                 // empty -> impl default UA
    int    timeout_ms       = 0;            // 0 -> impl default (no explicit timeout)
    std::size_t max_body    = 0;            // 0 -> unlimited; else cap the body in bytes
    bool   follow_redirects = true;         // default matches WinINet's auto-follow
    bool   reject_truncated = false;        // if capped before EOF, mark the response !ok
};

// The response. `body` is a byte buffer (text consumers treat it as a string; binary
// consumers copy out). `ok` = transport succeeded; `status` = HTTP code (0 if unread).
// `final_url` is the post-redirect URL (needed by a later site for token propagation).
struct HttpResponse {
    bool        ok        = false;
    long        status    = 0;
    std::string body;
    std::string final_url;
    bool        truncated = false;          // hit max_body before EOF
};

struct IHttp {
    virtual ~IHttp() = default;
    virtual HttpResponse fetch(const HttpRequest& req) = 0;
};

// TRANSITIONAL process-wide accessor. This global is a migration seam ONLY — the
// endgame is dependency injection (the host hands plugins an IHttp service, per
// docs/architecture.md). Do NOT enshrine it: where a consumer can cheaply hold an
// `IHttp&`, prefer that over reaching for this global.
IHttp& http();

// TRANSITIONAL injection hook (same status as http() above — not the endgame).
// Overrides what http() returns process-wide; pass nullptr to restore the default
// WinINet transport. Exists so tests can inject a fake transport to inspect the
// request a consumer builds; do NOT use it in production code.
void setHttp(IHttp* transport);

} // namespace core
