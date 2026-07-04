// HostServiceHttp.cpp — plugin-side core::IHttp over the RemoctHostServices C
// table (Phase 4 slice c). See HostServiceHttp.h. The exact inverse of the host's
// svc_open_session/svc_fetch (PluginHostServices.cpp): request/response fields are
// translated 1:1 and the int32_t* cancel token crosses VERBATIM (RemoctHttpReq and
// core::HttpRequest share `const int32_t*` — no reinterpret, unified in slice b).
#include "HostServiceHttp.h"

#include <vector>

namespace plugin {

core::HttpResponse HostServiceSession::fetch(const core::HttpRequest& req) {
    core::HttpResponse res;
    if (!hs_ || !hs_->http_fetch || !s_) return res;   // ok=false

    // Parallel header key/val arrays (borrowed for the call's duration).
    std::vector<const char*> keys, vals;
    keys.reserve(req.headers.size());
    vals.reserve(req.headers.size());
    for (const auto& h : req.headers) {
        keys.push_back(h.first.c_str());
        vals.push_back(h.second.c_str());
    }

    RemoctHttpReq r{};
    r.url          = req.url.c_str();
    r.method       = req.method.empty() ? "GET" : req.method.c_str();
    if (!req.body.empty())         { r.body = req.body.data(); r.body_len = req.body.size(); }
    if (!req.content_type.empty()) r.content_type = req.content_type.c_str();
    r.header_keys  = keys.empty() ? nullptr : keys.data();
    r.header_vals  = vals.empty() ? nullptr : vals.data();
    r.header_count = keys.size();
    switch (req.redirect) {
        case core::RedirectPolicy::FollowNone:
            r.redirect = REMOCT_REDIRECT_FOLLOW_NONE; break;
        case core::RedirectPolicy::FollowSameScheme:
            r.redirect = REMOCT_REDIRECT_FOLLOW_SAME_SCHEME; break;
        default:
            r.redirect = REMOCT_REDIRECT_FOLLOW_ALL; break;
    }
    r.pragma_no_cache  = req.pragma_no_cache ? 1 : 0;
    r.max_body         = req.max_body;
    r.reject_truncated = req.reject_truncated ? 1 : 0;
    r.cancel           = req.cancel;   // const int32_t* -> const int32_t*, verbatim

    RemoctHttpResp out{};
    hs_->http_fetch(s_, &r, &out);

    res.ok         = out.ok         != 0;
    res.cancelled  = out.cancelled  != 0;
    res.read_error = out.read_error != 0;
    res.truncated  = out.truncated  != 0;
    res.status     = out.status;
    if (out.body && out.body_len) res.body.assign(out.body, out.body_len);
    if (out.final_url)            res.final_url = out.final_url;

    // Host-owned buffers freed host-side; the plugin copied out above (nobody frees
    // across the boundary).
    if (hs_->http_response_free) hs_->http_response_free(&out);
    return res;
}

std::unique_ptr<core::IHttpSession>
HostServiceHttp::openSession(const core::HttpSessionConfig& cfg) {
    if (!hs_ || !hs_->http_open_session) return nullptr;
    // Empty UA => NULL (the ABI's "impl default UA"); session config carries UA +
    // timeout, exactly like the WinINet InternetOpen + SetOption(...TIMEOUT) baseline.
    RemoctHttpSession* s = hs_->http_open_session(
        hs_->host_ctx,
        cfg.user_agent.empty() ? nullptr : cfg.user_agent.c_str(),
        cfg.timeout_ms);
    if (!s) return nullptr;                    // caller retries (ensureSession parity)
    return std::make_unique<HostServiceSession>(hs_, s);
}

core::HttpResponse HostServiceHttp::fetch(const core::HttpRequest& req) {
    auto s = openSession(core::HttpSessionConfig{});   // transient default session
    if (!s) return {};
    return s->fetch(req);
}

} // namespace plugin
