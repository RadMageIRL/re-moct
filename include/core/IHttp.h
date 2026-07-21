// IHttp.h — platform-neutral HTTP seam (Phase 1; slices 1-3 = one-shot fetch,
// slice 4 = cancel token + persistent sessions for the two audio-thread sites).
//
// `core::IHttp` is the interface; `platform::win::WinInetHttp`
// (src/platform/win/HttpWinInet.cpp) is the Windows/WinINet implementation. They live
// in include/core/ + src/platform/win/ — the core/platform boundary established at the
// Phase 1 reorg (consumers include "core/IHttp.h"). The interface survived that move
// unchanged.
//
// This header must NOT include <windows.h> or any platform header — that is the
// "does core stay portable" seam test (it compiles on Linux CI as-is).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace core {

// Redirect handling. FollowAll is the default and maps to WinINet's automatic follow
// with NO extra flag — byte-identical to the pre-(c) `bool follow_redirects = true`,
// so groups (a)/(b) are unchanged. FollowNone = don't follow (NO_AUTO_REDIRECT).
// FollowSameScheme = follow but never cross http<->https (CoverArt/CAA: no downgrade;
// WinINet IGNORE_REDIRECT_TO_HTTP|HTTPS). The bool genuinely couldn't express the
// third policy — this is matching baseline, not gold-plating.
enum class RedirectPolicy { FollowAll, FollowNone, FollowSameScheme };

// A single HTTP request. All strings are UTF-8; the platform impl widens as needed.
struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;  // extra request headers
    std::string body;                       // POST body (unused in slice 1)
    std::string content_type;               // POST content-type (unused in slice 1)
    std::string user_agent;                 // empty -> impl default UA
    int    timeout_ms       = 0;            // 0 -> impl default (no explicit timeout)
    std::size_t max_body    = 0;            // 0 -> unlimited; else cap the body in bytes
    RedirectPolicy redirect = RedirectPolicy::FollowAll;  // see enum above
    bool   reject_truncated = false;        // if capped before EOF: fail AND clear the body
    // Send "Pragma: no-cache" (WinINet INTERNET_FLAG_PRAGMA_NOCACHE). Both audio-path
    // sites set it in their baselines (manifest polls must never be served stale by an
    // intermediary); default false keeps the six previously-migrated sites byte-identical.
    bool   pragma_no_cache  = false;
    // Cancel token (slice 4; Phase 4 slice b: unified on int32_t). The transport polls
    // this before opening and before each chunk read (via httpCancelRequested below)
    // and aborts the fetch when it reads nonzero — the same per-chunk granularity as
    // StreamSource's baseline stop check, so worst-case abort latency stays one
    // receive-timeout on a stalled read, never the remaining body. Typed as a plain
    // int32_t* (was std::atomic<bool>*) so it is EXACTLY the plugin C-ABI cancel type
    // (remoct_plugin.h RemoctHttpReq::cancel): the host HTTP service shim forwards the
    // plugin's flag straight through, no reinterpret, no width/endianness question. The
    // pointee is a plain int32 the OWNER writes via std::atomic_ref<int32_t> (NOT a
    // std::atomic over the same bytes — mixing the two is non-conforming); it only has
    // to outlive the synchronous fetch(). nullptr = not cancellable (default — all
    // one-shot sites unchanged).
    const int32_t* cancel = nullptr;
};

// Poll a cancel token (HttpRequest::cancel, i.e. the plugin ABI's const int32_t*): true
// iff non-null and nonzero. The owner writes the flag via std::atomic_ref<int32_t>; the
// transport reads it here the same way — the standard-correct atomic access of a plain
// POD int shared across the C boundary (const_cast is safe: the pointee is a non-const
// flag, and we only load). Pure, header-only, both platforms.
inline bool httpCancelRequested(const int32_t* cancel) {
    return cancel &&
           std::atomic_ref<int32_t>(*const_cast<int32_t*>(cancel))
               .load(std::memory_order_acquire) != 0;
}

// The response. `body` is a byte buffer (text consumers treat it as a string; binary
// consumers copy out). `ok` = transport succeeded; `status` = HTTP code (0 if unread).
// `final_url` is the post-redirect URL (needed by a later site for token propagation).
struct HttpResponse {
    bool        ok        = false;
    long        status    = 0;
    std::string body;
    std::string final_url;
    bool        truncated  = false;         // hit max_body before EOF
    // Aborted via req.cancel. Distinct from a transport failure so consumers with
    // reconnect/backoff logic (the HLS segment pump) never mistake their own stop
    // for a dead stream. When set: ok=false and the partial body is CLEARED — a
    // half-downloaded segment must never reach the decoder path.
    bool        cancelled  = false;
    // The body read failed partway (transport error mid-body, NOT EOF and NOT the
    // cap). ok/body semantics are unchanged (partial kept, ok=true — the uniform
    // baseline of the six one-shot sites, which all used `while(read && got>0)`);
    // this flag exists because hlsHttpGet's baseline is the exception: it FAILS the
    // call on a mid-body read error, so it gates on this to keep parity.
    bool        read_error = false;
};

// Scheme predicate (pure): true iff the URL is https. The platform transport derives
// TLS from this, so a plain-http:// URL gets NO secure flag (the lessons.md item that
// comes due for CDRipper's AR/CTDB fetch). Testable off-platform.
inline bool urlIsSecureScheme(const std::string& url) {
    return url.size() >= 8 &&
           (url.compare(0, 8, "https://") == 0 || url.compare(0, 8, "HTTPS://") == 0);
}

// Cap/reject finalization a transport applies after reading the body. When a
// reject_truncated request hit the cap (truncated), the partial body is CLEARED so a
// leading image/JSON fragment can't be mistaken for a complete payload, and the
// response fails. Otherwise it's a success with the body kept. reject_truncated
// defaults false -> groups (a)/(b) unaffected (ok=true, body kept). Pure; unit-tested.
inline void finalizeBody(HttpResponse& res, const HttpRequest& req) {
    if (req.reject_truncated && res.truncated) {
        res.body.clear();
        res.ok = false;
    } else {
        res.ok = true;
    }
}

// Cancel finalization a transport applies when req.cancel fired (pure; mirrors
// finalizeBody). The response fails, is marked cancelled, and any partial body is
// cleared — see HttpResponse::cancelled. Unit-tested off-platform.
inline void finalizeCancelled(HttpResponse& res) {
    res.body.clear();
    res.ok        = false;
    res.cancelled = true;
}

// Session configuration (slice 4). UA + timeouts are SESSION-level: set once when the
// session opens, exactly like the baselines' InternetOpen + InternetSetOption(…TIMEOUT).
struct HttpSessionConfig {
    std::string user_agent;   // empty -> impl default UA
    int         timeout_ms = 0;  // connect/receive/send on the session; 0 -> impl default
};

// A persistent keep-alive session: connections are reused across fetches (WinINet:
// one InternetOpen handle held for the session's lifetime + KEEP_CONNECTION per call).
// Lifetime is the CALLER's to manage and is the parity contract — StreamSource's HLS
// session dies at disconnect() (fresh one per re-handshake/re-pin), IHeartRadio's
// lives with the object. Requests through a session must leave user_agent/timeout_ms
// at their defaults (the session's config wins; the impl ignores per-request values).
// Concurrent fetch() calls on one session from different threads are allowed — both
// baselines already share their session across the producer and open-path threads.
struct IHttpSession {
    virtual ~IHttpSession() = default;
    virtual HttpResponse fetch(const HttpRequest& req) = 0;
};

// Progress callback for a streaming download: (received_bytes, total_bytes). total is
// 0 when the origin sends no Content-Length. Invoked periodically on the fetching
// thread — keep it cheap and non-blocking (the podcast download UI just stores into
// atomics the draw loop reads).
using ProgressFn = std::function<void(std::uint64_t received, std::uint64_t total)>;

struct IHttp {
    virtual ~IHttp() = default;
    virtual HttpResponse fetch(const HttpRequest& req) = 0;
    // Open a persistent session (may return nullptr on failure — callers retry, as
    // the baselines' ensureSession did). The default implementation below is a thin
    // forwarder to fetch() (no pooling) so fakes/tests need no changes: it folds the
    // session config into each request and delegates. Real transports override it
    // (WinInetHttp holds an InternetOpen handle per session for connection reuse).
    // The returned session must not outlive the IHttp that created it.
    virtual std::unique_ptr<IHttpSession> openSession(const HttpSessionConfig& cfg);

    // Stream a GET straight to a file on disk, reporting progress — the body never
    // lands in memory, so this suits large files (podcast episodes) that fetch()'s
    // in-memory buffer would balloon. Honors req.cancel (per-chunk) and req.timeout_ms.
    // On success writes dest_path and returns ok=true (empty body, status set); on
    // cancel/error returns ok=false and removes any partial file. The default below
    // returns ok=false (unsupported) so fakes and other impls need no change — only the
    // WinINet and libcurl transports override it. (Slice 4 reuses this for the queue.)
    virtual HttpResponse fetchToFile(const HttpRequest& req, const std::string& dest_path,
                                     const ProgressFn& progress);
};

inline std::unique_ptr<IHttpSession> IHttp::openSession(const HttpSessionConfig& cfg) {
    struct Forwarder final : IHttpSession {
        IHttp*            owner;
        HttpSessionConfig cfg;
        Forwarder(IHttp* o, HttpSessionConfig c) : owner(o), cfg(std::move(c)) {}
        HttpResponse fetch(const HttpRequest& req) override {
            HttpRequest q = req;                        // fold session config in
            if (q.user_agent.empty()) q.user_agent = cfg.user_agent;
            if (q.timeout_ms == 0)    q.timeout_ms = cfg.timeout_ms;
            return owner->fetch(q);
        }
    };
    return std::make_unique<Forwarder>(this, cfg);
}

// Default: streaming-to-file unsupported (ok=false). The WinINet/libcurl transports
// override this; fakes and the plugin host-service shim keep compiling untouched.
inline HttpResponse IHttp::fetchToFile(const HttpRequest&, const std::string&,
                                       const ProgressFn&) {
    return HttpResponse{};
}

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
