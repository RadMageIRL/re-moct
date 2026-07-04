// HostServiceHttp.h — the PLUGIN side of the HTTP service bridge (Phase 4 slice c).
//
// The exact inverse of the host's PluginHostServices (slice b): where that fills a
// RemoctHostServices table FROM core::IHttp, this implements core::IHttp OVER the
// injected RemoctHostServices C table. StreamSource/IHeartRadio keep calling
// `->openSession(cfg)` / `session_->fetch(req)` against a core::IHttp& exactly as
// before — the transport now reaches the host's WinINet/libcurl across the ABI
// instead of the (unreachable-from-a-.so) core::http() global.
//
// This is the shim's real in-plugin consumer the slice-b handoff promised. It links
// NO transport of its own — every request crosses via host->http_* (the seam stays
// host-side); the plugin's ONLY private transport is StreamSource's sacred raw ICY
// loop (raw WinINet / curl CONNECT_ONLY), which is not this and not core::http().
//
// Platform-free: talks only to core/IHttp.h + the C ABI header.
#pragma once
#include <memory>

#include "core/remoct_plugin.h"
#include "core/IHttp.h"

namespace plugin {

// A core::IHttpSession backed by a host-owned RemoctHttpSession handle. The dtor
// closes the host session, so lifetime tracks the unique_ptr the consumers hold
// (HLS session dies at disconnect(); IHeart's lives with the object) — parity.
class HostServiceSession final : public core::IHttpSession {
public:
    HostServiceSession(const RemoctHostServices* hs, RemoctHttpSession* s)
        : hs_(hs), s_(s) {}
    ~HostServiceSession() override {
        if (hs_ && hs_->http_session_close) hs_->http_session_close(s_);
    }
    HostServiceSession(const HostServiceSession&)            = delete;
    HostServiceSession& operator=(const HostServiceSession&) = delete;

    core::HttpResponse fetch(const core::HttpRequest& req) override;

private:
    const RemoctHostServices* hs_;
    RemoctHttpSession*        s_;
};

// A core::IHttp over the injected host services. Injected into StreamSource +
// IHeartRadio in place of the core::http() global.
class HostServiceHttp final : public core::IHttp {
public:
    explicit HostServiceHttp(const RemoctHostServices* hs) : hs_(hs) {}

    // One-shot fetch (the pure-virtual; neither consumer calls it — they use
    // sessions — but the interface requires it): a transient default session.
    core::HttpResponse fetch(const core::HttpRequest& req) override;

    // Open a keep-alive session over host->http_open_session. nullptr on failure
    // (the consumers retry, exactly as ensureSession did against core::http()).
    std::unique_ptr<core::IHttpSession>
    openSession(const core::HttpSessionConfig& cfg) override;

private:
    const RemoctHostServices* hs_;
};

} // namespace plugin
