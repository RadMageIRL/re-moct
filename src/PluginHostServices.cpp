// PluginHostServices.cpp — the host HTTP service shim (Phase 4 slice b). See
// PluginHostServices.h. No platform header (compiles on both matrix jobs).
#include "PluginHostServices.h"
#include "Log.h"

#include <cstdlib>   // malloc / free
#include <cstring>   // memcpy
#include <memory>
#include <utility>

namespace core {
namespace {

// Opaque host session handle (RemoctHttpSession* on the ABI): wraps the real
// keep-alive core::IHttpSession. Allocated in svc_open_session, freed in
// svc_session_close — host-owned end to end, never freed by the plugin.
struct HostHttpSession { std::unique_ptr<IHttpSession> sess; };

// Every entry is noexcept (the ABI forbids exceptions across the line) — bodies
// are wrapped so a std::bad_alloc etc. becomes a safe failure, not terminate().

RemoctHttpSession* svc_open_session(void* host_ctx, const char* ua,
                                    int32_t timeout_ms) noexcept {
    try {
        auto* hs = static_cast<HostServices*>(host_ctx);
        if (!hs) return nullptr;
        HttpSessionConfig cfg;
        if (ua) cfg.user_agent = ua;
        cfg.timeout_ms = timeout_ms;
        auto sess = hs->http().openSession(cfg);
        if (!sess) return nullptr;                       // caller retries (baseline)
        return reinterpret_cast<RemoctHttpSession*>(
            new HostHttpSession{std::move(sess)});
    } catch (...) { return nullptr; }
}

void svc_session_close(RemoctHttpSession* s) noexcept {
    delete reinterpret_cast<HostHttpSession*>(s);        // delete nullptr is safe
}

void svc_fetch(RemoctHttpSession* s, const RemoctHttpReq* req,
               RemoctHttpResp* out) noexcept {
    if (!out) return;
    *out = RemoctHttpResp{};                             // ok=0 default
    if (!s || !req) return;
    try {
        auto* hs = reinterpret_cast<HostHttpSession*>(s);

        HttpRequest hq;
        if (req->url)          hq.url = req->url;
        hq.method = (req->method && req->method[0]) ? req->method : "GET";
        if (req->body && req->body_len) hq.body.assign(req->body, req->body_len);
        if (req->content_type) hq.content_type = req->content_type;
        for (size_t i = 0; i < req->header_count; ++i) {
            const char* k = req->header_keys ? req->header_keys[i] : nullptr;
            const char* v = req->header_vals ? req->header_vals[i] : nullptr;
            if (k) hq.headers.emplace_back(k, v ? v : "");
        }
        switch (req->redirect) {
            case REMOCT_REDIRECT_FOLLOW_NONE:
                hq.redirect = RedirectPolicy::FollowNone; break;
            case REMOCT_REDIRECT_FOLLOW_SAME_SCHEME:
                hq.redirect = RedirectPolicy::FollowSameScheme; break;
            default:
                hq.redirect = RedirectPolicy::FollowAll; break;
        }
        hq.pragma_no_cache  = req->pragma_no_cache != 0;
        hq.max_body         = req->max_body;
        hq.reject_truncated = req->reject_truncated != 0;
        hq.cancel           = req->cancel;   // const int32_t* -> const int32_t*,
                                             // ZERO bridging (unified type)

        HttpResponse res = hs->sess->fetch(hq);

        out->ok         = res.ok        ? 1 : 0;
        out->cancelled  = res.cancelled ? 1 : 0;
        out->read_error = res.read_error? 1 : 0;
        out->truncated  = res.truncated ? 1 : 0;
        out->status     = res.status;
        // Body is a byte buffer (may contain NULs) — malloc+copy, host-owned,
        // freed by svc_response_free. Empty => leave nullptr/0.
        if (!res.body.empty()) {
            void* p = std::malloc(res.body.size());
            if (p) {
                std::memcpy(p, res.body.data(), res.body.size());
                out->body     = static_cast<const char*>(p);
                out->body_len = res.body.size();
            }
        }
        if (!res.final_url.empty()) {
            std::size_t n = res.final_url.size() + 1;
            char* u = static_cast<char*>(std::malloc(n));
            if (u) { std::memcpy(u, res.final_url.c_str(), n); out->final_url = u; }
        }
    } catch (...) {
        // Leave *out at its zeroed failure state; free any partial allocation.
        std::free(const_cast<char*>(out->body));
        std::free(const_cast<char*>(out->final_url));
        *out = RemoctHttpResp{};
    }
}

// Host-owned malloc'd fields freed by the host here — the plugin copies out first,
// then calls this; nobody frees across the boundary.
void svc_response_free(RemoctHttpResp* resp) noexcept {
    if (!resp) return;
    std::free(const_cast<char*>(resp->body));
    std::free(const_cast<char*>(resp->final_url));
    resp->body = nullptr; resp->body_len = 0; resp->final_url = nullptr;
}

void svc_log(void* /*host_ctx*/, const char* msg) noexcept {
    if (msg) try { Log::write("plugin", msg); } catch (...) {}
}

} // namespace

HostServices::HostServices(IHttp& http) : http_(&http) {
    table_.struct_size        = static_cast<uint32_t>(sizeof(RemoctHostServices));
    table_.host_ctx           = this;
    table_.http_open_session  = &svc_open_session;
    table_.http_session_close = &svc_session_close;
    table_.http_fetch         = &svc_fetch;
    table_.http_response_free = &svc_response_free;
    table_.log                = &svc_log;
}

} // namespace core
