// plugin_http_shim_test — Phase 4 slice (b): the host HTTP service shim.
// Proves the RemoctHostServices table (PluginHostServices) correctly bridges the
// plugin C ABI to core::IHttp: request/response translation, the ZERO-bridge
// cancel passthrough (the plugin's int32_t* reaches the transport unchanged), the
// cancel being honored, and host-owned response ownership (http_response_free).
//
// A FakeHttp backs it so the translation is asserted deterministically on both
// jobs; the REAL-transport int32 cancel granularity (≤ one receive-timeout) is
// locked separately by http_cancel_test, and the end-to-end real crossing by the
// slice-(c) live gate.
#include "PluginHostServices.h"
#include "core/IHttp.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) std::printf("  ok   %s\n", msg); \
    else    { std::printf("  FAIL %s  (line %d)\n", msg, __LINE__); ++g_fail; } \
} while (0)

// Minimal fake transport: records the last request, returns a canned response,
// and (optionally) honors the cancel token like a real per-chunk poll would.
struct FakeHttp final : core::IHttp {
    core::HttpRequest  last;
    core::HttpResponse next;
    bool               poll_cancel = false;
    int                fetches     = 0;

    core::HttpResponse fetch(const core::HttpRequest& req) override {
        last = req; ++fetches;
        if (poll_cancel && core::httpCancelRequested(req.cancel)) {
            core::HttpResponse r; core::finalizeCancelled(r); return r;
        }
        return next;
    }
    // openSession uses the IHttp default forwarder -> our fetch().
};

int main() {
    std::printf("== plugin_http_shim_test (Phase 4 slice b) ==\n");

    FakeHttp fake;
    core::HostServices svc(fake);              // shim over the fake transport
    const RemoctHostServices* t = svc.table();
    CHECK(t && t->struct_size == sizeof(RemoctHostServices), "table built, struct_size set");
    CHECK(t->host_ctx == &svc, "host_ctx points at the HostServices owner");

    RemoctHttpSession* s = t->http_open_session(t->host_ctx, "remoct-test", 5000);
    CHECK(s != nullptr, "http_open_session returned a session");

    // ── request translation + response ownership ───────────────────────────
    {
        fake.next = core::HttpResponse{};
        fake.next.ok = true; fake.next.status = 200;
        fake.next.body = "SEGMENT-BYTES"; fake.next.final_url = "https://cdn/final.aac";

        const char* keys[] = { "Accept", "X-Test" };
        const char* vals[] = { "audio/aac", "1" };
        RemoctHttpReq req; std::memset(&req, 0, sizeof(req));
        req.url          = "https://origin/seg1.aac";
        req.method       = "POST";
        req.body         = "PAYLOAD"; req.body_len = 7;
        req.content_type = "application/json";
        req.header_keys  = keys; req.header_vals = vals; req.header_count = 2;
        req.redirect     = REMOCT_REDIRECT_FOLLOW_SAME_SCHEME;
        req.pragma_no_cache = 1; req.max_body = 4096; req.reject_truncated = 1;

        RemoctHttpResp resp; std::memset(&resp, 0, sizeof(resp));
        t->http_fetch(s, &req, &resp);

        CHECK(fake.last.url == "https://origin/seg1.aac", "url translated");
        CHECK(fake.last.method == "POST", "method translated");
        CHECK(fake.last.body == "PAYLOAD", "body translated (bytes)");
        CHECK(fake.last.content_type == "application/json", "content_type translated");
        CHECK(fake.last.headers.size() == 2 &&
              fake.last.headers[0].first == "Accept" &&
              fake.last.headers[1].second == "1", "headers translated (parallel arrays)");
        CHECK(fake.last.redirect == core::RedirectPolicy::FollowSameScheme, "redirect enum mapped");
        CHECK(fake.last.pragma_no_cache && fake.last.reject_truncated &&
              fake.last.max_body == 4096, "flags/caps translated");

        CHECK(resp.ok == 1 && resp.status == 200, "response ok/status");
        CHECK(resp.body && resp.body_len == 13 &&
              std::memcmp(resp.body, "SEGMENT-BYTES", 13) == 0, "response body copied out");
        CHECK(resp.final_url && std::strcmp(resp.final_url, "https://cdn/final.aac") == 0,
              "final_url copied out");

        t->http_response_free(&resp);
        CHECK(resp.body == nullptr && resp.body_len == 0 && resp.final_url == nullptr,
              "http_response_free clears the host-owned buffers");
    }

    // ── cancel passthrough (ZERO bridging: the plugin's int32_t* reaches the
    //    transport unchanged) ────────────────────────────────────────────────
    {
        int32_t cancel = 0;
        RemoctHttpReq req; std::memset(&req, 0, sizeof(req));
        req.url = "https://origin/x"; req.cancel = &cancel;
        RemoctHttpResp resp; std::memset(&resp, 0, sizeof(resp));
        fake.next = core::HttpResponse{}; fake.next.ok = true;
        t->http_fetch(s, &req, &resp);
        CHECK(fake.last.cancel == &cancel,
              "cancel int32_t* passed through verbatim (no bridge)");
        t->http_response_free(&resp);
    }

    // ── cancel honored: a set flag aborts the (fake per-chunk) transport ──────
    {
        fake.poll_cancel = true;
        int32_t cancel = 1;                         // already stopped
        RemoctHttpReq req; std::memset(&req, 0, sizeof(req));
        req.url = "https://origin/wedged"; req.cancel = &cancel;
        RemoctHttpResp resp; std::memset(&resp, 0, sizeof(resp));
        t->http_fetch(s, &req, &resp);
        CHECK(resp.cancelled == 1 && resp.ok == 0 && resp.body == nullptr,
              "set cancel -> resp.cancelled, ok=0, body cleared");
        t->http_response_free(&resp);
        fake.poll_cancel = false;
    }

    t->http_session_close(s);
    std::printf("  ok   http_session_close (no crash)\n");

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
