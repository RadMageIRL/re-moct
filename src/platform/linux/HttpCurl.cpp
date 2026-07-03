// HttpCurl.cpp — Linux/libcurl implementation of core::IHttp (Phase 3 slice 2).
//
// The platform::lnx sibling of src/platform/win/HttpWinInet.cpp, same TU
// layout: a file-static executeRequest() shared by the one-shot fetch() and
// session fetches; core::http()/setHttp() defined here with the exact
// g_override shape the Windows TU has (tests inject identically on both
// platforms). Parity is field-by-field per docs/phase3-slice2-design.md §2 —
// notable mappings and their reasoning:
//
//   Sessions   — a held CURLSH share handle (CONNECT + DNS + SSL_SESSION
//                caches, mutex-locked) + one easy handle PER FETCH bound via
//                CURLOPT_SHARE. Keep-alive reuse across fetches AND the
//                contract's concurrent-fetch thread safety (a single shared
//                easy handle can't run two transfers — rejected in design).
//   Cancel     — pre-check before perform (pre-cancelled = zero network),
//                then CURLOPT_XFERINFOFUNCTION polling the atomic; nonzero
//                return -> CURLE_ABORTED_BY_CALLBACK -> finalizeCancelled().
//                Accepted-better residual: the callback also fires during
//                CONNECT, so a stalled connect is cancellable here where
//                WinINet can't (documented cross-platform difference).
//   timeout_ms — WinINet's RECEIVE_TIMEOUT is a per-read STALL guard, not a
//                whole-transfer deadline: mapped to CONNECTTIMEOUT_MS +
//                LOW_SPEED_LIMIT=1/LOW_SPEED_TIME≈timeout. Deliberately NOT
//                CURLOPT_TIMEOUT_MS (that would kill a healthy slow cover-art
//                download mid-body — behavior WinINet never had).
//   Redirects  — FollowAll -> FOLLOWLOCATION; FollowNone -> off (3xx is the
//                final response, as WinINet's NO_AUTO_REDIRECT); Follow-
//                SameScheme -> FOLLOWLOCATION + REDIR_PROTOCOLS_STR pinned to
//                the ORIGINAL scheme (cross-scheme redirect fails, the same
//                observable outcome as WinINet's IGNORE_REDIRECT_TO_* pair).
//   Cache flags— WinINet's RELOAD/NO_CACHE_WRITE bypass ITS client cache;
//                curl has none -> no-op. pragma_no_cache stays a real request
//                header ("Pragma: no-cache"), as the flag it mirrors.
//   Proxy      — curl's env-var convention (http_proxy/https_proxy/no_proxy),
//                the Linux system convention (WinINet PRECONFIG = IE/system).
//   POST body  — CURLOPT_POSTFIELDS raw bytes; UTF-8 is never widened (no
//                wide API exists on this path at all — the group-(b) rule).
//
// The ICY/continuous live audio read loop stays OUT of this seam permanently
// (its Linux twin is slice 3) — exactly as WinINet's header says.
#ifdef __linux__

#include "core/IHttp.h"

#include <curl/curl.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace {

// The app's canonical UA — byte-identical to HttpWinInet's kDefaultUA.
constexpr const char* kDefaultUA =
    "RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)";

std::once_flag g_curl_init_once;
void ensureGlobalInit() {
    std::call_once(g_curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool wasCancelled(const core::HttpRequest& req) {
    return req.cancel && req.cancel->load();
}

// Per-transfer state threaded through the callbacks.
struct TransferCtx {
    const core::HttpRequest* req = nullptr;
    core::HttpResponse*      res = nullptr;
    bool cap_hit = false;   // write callback stopped the transfer at max_body
};

// Body sink. Mirrors HttpWinInet::readBody's shape: append the whole incoming
// chunk FIRST, then check the cap — so a capped body may exceed max_body by up
// to one chunk, exactly like the Windows loop. Returning a short count aborts
// the transfer (CURLE_WRITE_ERROR), disambiguated from a real write error via
// ctx->cap_hit.
size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<TransferCtx*>(userdata);
    size_t n = size * nmemb;
    ctx->res->body.append(ptr, n);
    if (ctx->req->max_body && ctx->res->body.size() > ctx->req->max_body) {
        ctx->res->truncated = true;
        ctx->cap_hit = true;
        return 0;                        // abort — the Windows loop's `break`
    }
    return n;
}

// Cancel poll — libcurl calls this throughout the transfer (including the
// connect phase). Nonzero aborts with CURLE_ABORTED_BY_CALLBACK.
int xferInfo(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<TransferCtx*>(userdata);
    return wasCancelled(*ctx->req) ? 1 : 0;
}

// Extra request headers: Content-Type first when set, then the caller's pairs
// (the byte order buildHeaders() emits on Windows), plus Pragma: no-cache when
// the request asks (the INTERNET_FLAG_PRAGMA_NOCACHE twin).
curl_slist* buildHeaders(const core::HttpRequest& req) {
    curl_slist* h = nullptr;
    if (!req.content_type.empty())
        h = curl_slist_append(h, ("Content-Type: " + req.content_type).c_str());
    for (const auto& kv : req.headers)
        h = curl_slist_append(h, (kv.first + ": " + kv.second).c_str());
    if (req.pragma_no_cache)
        h = curl_slist_append(h, "Pragma: no-cache");
    return h;
}

// Execute one request on a fresh easy handle, optionally bound to a session's
// share handle (keep-alive pool). ua/timeout_ms are the EFFECTIVE values: the
// session's config for session fetches (per-request values ignored, per the
// IHttpSession contract), the request's own for one-shots.
core::HttpResponse executeRequest(CURLSH* share, const core::HttpRequest& req,
                                  const std::string& ua, int timeout_ms) {
    core::HttpResponse res;
    if (wasCancelled(req)) {             // cancelled before we even opened
        core::finalizeCancelled(res);
        return res;
    }

    CURL* h = curl_easy_init();
    if (!h) return res;                  // ok stays false

    TransferCtx ctx;
    ctx.req = &req;
    ctx.res = &res;

    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(h, CURLOPT_USERAGENT, ua.empty() ? kDefaultUA : ua.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferInfo);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);   // threads + SIGPIPE safety
    if (share) curl_easy_setopt(h, CURLOPT_SHARE, share);

    // Stall-guard timeout mapping (design §2): connect deadline + low-speed
    // abort, NOT a whole-transfer deadline. 0 = impl default (no timeouts),
    // matching WinINet's "don't set the options" shape.
    if (timeout_ms > 0) {
        long secs = (long)((timeout_ms + 999) / 1000);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, secs);
    }

    switch (req.redirect) {
        case core::RedirectPolicy::FollowAll:
            curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(h, CURLOPT_MAXREDIRS, 30L);
            break;
        case core::RedirectPolicy::FollowNone:
            curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
            break;
        case core::RedirectPolicy::FollowSameScheme:
            curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(h, CURLOPT_MAXREDIRS, 30L);
            curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR,
                             core::urlIsSecureScheme(req.url) ? "https" : "http");
            break;
    }

    const bool is_get = req.method.empty() || req.method == "GET";
    if (!is_get) {
        if (req.method == "POST") {
            // Raw bytes, never widened — nlohmann emits UTF-8 and it goes out
            // as-is (the "Björk" wire-fidelity rule from group (b)).
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        } else {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, req.method.c_str());
            if (!req.body.empty()) {
                curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
                curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
            }
        }
    }

    curl_slist* hdrs = buildHeaders(req);
    if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(h);

    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    res.status = status;

    auto grabFinalUrl = [&] {
        char* eu = nullptr;
        if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eu) == CURLE_OK && eu)
            res.final_url = eu;
    };

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        // req.cancel fired mid-transfer (connect or body).
        core::finalizeCancelled(res);
    } else if (rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && ctx.cap_hit)) {
        // Clean EOF, or the cap stopped us — the WinINet loop-break shape:
        // finalizeBody applies cap-and-keep vs reject_truncated+clear.
        grabFinalUrl();
        core::finalizeBody(res, req);
    } else if (status != 0) {
        // Transfer began (headers/status arrived) then died mid-body — the
        // InternetReadFile-failure shape: read_error flagged, partial body
        // KEPT, ok per finalizeBody (hls gates on the flag; the six one-shot
        // sites keep the partial, exactly as on Windows).
        res.read_error = true;
        grabFinalUrl();
        core::finalizeBody(res, req);
    }
    // else: connect-level failure (no status) -> ok stays false, like a
    // failed InternetOpenUrl.

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return res;
}

} // namespace

namespace platform::lnx {

// A persistent keep-alive session: one CURLSH share handle held for the
// object's lifetime — the connection/DNS/TLS-session caches live in the share,
// so consecutive fetches reuse pooled connections (proven live by
// http_cancel_test scenario B: two fetches, ONE observed TCP connection).
// Each fetch runs its own easy handle bound to the share, so concurrent
// fetch() calls from different threads are safe — the contract both audio-site
// baselines rely on. UA + timeout are session-level; per-request values are
// ignored, per the IHttpSession contract (same as WinInetSession).
struct CurlSession final : core::IHttpSession {
    CURLSH*                 share_ = nullptr;
    core::HttpSessionConfig cfg_;
    std::mutex              locks_[CURL_LOCK_DATA_LAST];

    static void lockCb(CURL*, curl_lock_data data, curl_lock_access, void* userp) {
        static_cast<CurlSession*>(userp)->locks_[data].lock();
    }
    static void unlockCb(CURL*, curl_lock_data data, void* userp) {
        static_cast<CurlSession*>(userp)->locks_[data].unlock();
    }

    explicit CurlSession(core::HttpSessionConfig cfg) : cfg_(std::move(cfg)) {
        share_ = curl_share_init();
        if (!share_) return;
        curl_share_setopt(share_, CURLSHOPT_LOCKFUNC,   lockCb);
        curl_share_setopt(share_, CURLSHOPT_UNLOCKFUNC, unlockCb);
        curl_share_setopt(share_, CURLSHOPT_USERDATA,   this);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
    ~CurlSession() override { if (share_) curl_share_cleanup(share_); }

    core::HttpResponse fetch(const core::HttpRequest& req) override {
        return executeRequest(share_, req, cfg_.user_agent, cfg_.timeout_ms);
    }
};

struct CurlHttp final : core::IHttp {
    // One-shot fetch: fresh easy handle per call, no share — no connection
    // reuse across one-shots, the same wire shape as WinINet's fresh
    // InternetOpen per call.
    core::HttpResponse fetch(const core::HttpRequest& req) override {
        ensureGlobalInit();
        return executeRequest(nullptr, req, req.user_agent, req.timeout_ms);
    }

    std::unique_ptr<core::IHttpSession>
    openSession(const core::HttpSessionConfig& cfg) override {
        ensureGlobalInit();
        auto s = std::make_unique<CurlSession>(cfg);
        if (!s->share_) return nullptr;   // caller retries, as baselines did
        return s;
    }
};

} // namespace platform::lnx

namespace core {

// Transitional injection pointer — the exact HttpWinInet.cpp shape, so tests
// that inject via setHttp() behave identically on both platforms.
static IHttp* g_override = nullptr;

void setHttp(IHttp* transport) { g_override = transport; }

IHttp& http() {
    if (g_override) return *g_override;
    static platform::lnx::CurlHttp instance;
    return instance;
}

} // namespace core

#endif // __linux__
