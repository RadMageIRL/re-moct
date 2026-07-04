// PluginHostServices.h — the host side of the plugin service table (Phase 4
// slice b). Fills a RemoctHostServices (remoct_plugin.h) over the host's
// core::IHttp transport, so a plugin reaches the host's HTTP (WinINet/libcurl)
// across the C ABI instead of carrying its own — the DI endgame the transitional
// core::http() global always pointed at.
//
// The cancel token passes through with ZERO bridging: the ABI's
// RemoctHttpReq::cancel and core::HttpRequest::cancel are BOTH const int32_t*
// (unified in slice b), so the shim forwards the plugin's flag pointer verbatim
// and the transport polls it per chunk exactly as slice 4 did — the granularity
// preserved across the boundary.
//
// Platform-free: only talks to core::IHttp + the C ABI header.
#pragma once
#include "core/remoct_plugin.h"
#include "core/IHttp.h"

namespace core {

// Owns a RemoctHostServices table + its host_ctx, to hand a plugin at create().
// LIFETIME CONTRACT (ABI §2.6): this object must outlive every plugin instance
// created with its table() — the host holds it for at least the plugin's life.
class HostServices {
public:
    explicit HostServices(IHttp& http = core::http());
    HostServices(const HostServices&)            = delete;
    HostServices& operator=(const HostServices&) = delete;

    const RemoctHostServices* table() const { return &table_; }
    IHttp& http() const { return *http_; }

private:
    IHttp*             http_;
    RemoctHostServices table_{};
};

} // namespace core
