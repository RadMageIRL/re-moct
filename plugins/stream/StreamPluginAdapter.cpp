// StreamPluginAdapter.cpp — the RemoctPlugin C-ABI adapter over StreamSource
// (Phase 4). Each C entry forwards 1:1 to a StreamSource; the instance (`self`) is
// a StreamInstance owning the source + the plugin-side HTTP bridge. Since slice (c)
// this file + StreamSource live in plugins/stream/ and this TU exports
// remoct_plugin_query (the .so's one symbol); remoct_stream_plugin_query() is kept
// so the compiled-in plugin_stream_test stays the byte-identity reference (D4).
//
// The callbacks are internal-linkage (anonymous namespace) and noexcept (the ABI
// forbids exceptions across the line — StreamSource's queried surface does not
// throw). Assigning C++ statics to the RemoctPlugin C function-pointer members is
// the same pattern the codebase already uses for miniaudio's C callbacks
// (AudioManager::maDataCallback). HTTP is routed through the injected host services
// (slice c): create() builds a plugin::HostServiceHttp over the RemoctHostServices
// table and constructs StreamSource with it — no core::http() in the plugin.
#include "StreamPluginAdapter.h"
#include "StreamSource.h"
#include "HostServiceHttp.h"   // core::IHttp over the injected host services (slice c)
#include "IHeartDeepLog.h"     // set_config("deeplog", ...) — deep-log toggle across the ABI

#include <cstring>
#include <string>

namespace {

// The instance behind `self`. Owns the source + the plugin-side HTTP bridge built
// from the host services handed in at create. Member order matters: `host` then
// `http` (built from host) then `src` (built from http) — declaration order = ctor
// init order, so each is live before the next uses it.
struct StreamInstance {
    const RemoctHostServices* host;
    plugin::HostServiceHttp   http;   // core::IHttp over host->http_* (the slice-b shim's real consumer)
    StreamSource              src;    // constructed with the injected transport
    explicit StreamInstance(const RemoctHostServices* h)
        : host(h), http(h), src(http) {}
};

inline StreamInstance* inst(void* self) { return static_cast<StreamInstance*>(self); }

// snprintf-style fill of a host-owned buffer; returns the full length.
std::size_t fillBuf(char* buf, std::size_t cap, const std::string& s) {
    if (buf && cap) {
        std::size_t n = (s.size() < cap - 1) ? s.size() : cap - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
    return s.size();
}

void* sp_create(const RemoctHostServices* host, void* /*host_ctx*/) noexcept {
    // try/catch: StreamSource's ctor allocates the ring — no exception may cross
    // the C ABI (rule #2), so a bad_alloc becomes a null return (create failed).
    try { return new StreamInstance(host); }
    catch (...) { return nullptr; }
}
void sp_destroy(void* self) noexcept { delete inst(self); }

int32_t sp_handles_url(void* /*self*/, const char* url) noexcept {
    if (!url) return 0;
    std::string u = url;
    // The streaming source plays http/https byte streams + HLS manifests
    // (iHeart revma is https). Local files / CD are host built-ins, not here.
    return (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) ? 1 : 0;
}

int32_t sp_open(void* self, const char* url) noexcept {
    return inst(self)->src.open(url ? url : "") ? 0 : 1;   // 0 = ok
}

uint32_t sp_read_frames(void* self, float* dst, uint32_t n) noexcept {
    return inst(self)->src.readFrames(dst, n);
}

void sp_get_caps(void* self, RemoctSourceCaps* out) noexcept {
    core::SourceCaps c = inst(self)->src.caps();
    out->seekable = c.seekable ? 1 : 0;
    out->finite   = c.finite   ? 1 : 0;
    out->live     = c.live     ? 1 : 0;
}

double  sp_position_sec(void* self) noexcept { return inst(self)->src.positionSec(); }
double  sp_duration_sec(void* self) noexcept { return inst(self)->src.durationSec(); }
int32_t sp_seek_to(void* self, double s) noexcept { return inst(self)->src.seekTo(s) ? 1 : 0; }
void    sp_set_paused(void* self, int32_t p) noexcept { inst(self)->src.pause(p != 0); }
int32_t sp_is_buffering(void* self) noexcept { return inst(self)->src.buffering() ? 1 : 0; }
void    sp_close(void* self) noexcept { inst(self)->src.close(); }

std::size_t sp_now_playing(void* self, char* b, std::size_t c) noexcept {
    return fillBuf(b, c, inst(self)->src.nowPlaying());
}
std::size_t sp_art_url(void* self, char* b, std::size_t c) noexcept {
    return fillBuf(b, c, inst(self)->src.currentArtUrl());
}
std::size_t sp_last_error(void* self, char* b, std::size_t c) noexcept {
    return fillBuf(b, c, inst(self)->src.lastError());
}

void sp_set_config(void* self, const char* key, const char* value) noexcept {
    if (!key) return;
    const bool on = value && value[0] == '1';
    if (std::strcmp(key, "prefer_digital") == 0)
        inst(self)->src.setPreferDigital(on);
    else if (std::strcmp(key, "deeplog") == 0)
        // Deep-log toggle crosses the ABI here (was UIManager -> IHeartDeepLog::
        // toggle() directly; IHeartDeepLog now lives in this .so). Host tracks the
        // on/off state and pushes it; this is the in-plugin call. (slice c, row 8)
        IHeartDeepLog::setEnabled(on);
    else if (std::strcmp(key, "iheart_probe_minted") == 0)
        // Identity A/B arm (probe): minted anonymous profileId vs today's anon handshake.
        // Read at hlsConnect time + forced anon unless the deep log is on, so this is inert
        // outside a probe session (see StreamSource::hlsConnect).
        inst(self)->src.setProbeMinted(on);
}

const RemoctPlugin STREAM_PLUGIN = {
    /* abi_version  */ REMOCT_ABI_VERSION,
    /* struct_size  */ static_cast<uint32_t>(sizeof(RemoctPlugin)),
    /* name         */ "stream",
    /* display_name */ "iHeart / Internet Radio",
    /* create       */ sp_create,
    /* destroy      */ sp_destroy,
    /* handles_url  */ sp_handles_url,
    /* open         */ sp_open,
    /* read_frames  */ sp_read_frames,
    /* get_caps     */ sp_get_caps,
    /* position_sec */ sp_position_sec,
    /* duration_sec */ sp_duration_sec,
    /* seek_to      */ sp_seek_to,
    /* set_paused   */ sp_set_paused,
    /* is_buffering */ sp_is_buffering,
    /* close        */ sp_close,
    /* now_playing  */ sp_now_playing,
    /* art_url      */ sp_art_url,
    /* last_error   */ sp_last_error,
    /* set_config   */ sp_set_config,
};

} // namespace

// In-process acquisition (compiled-in): kept so plugin_stream_test drives the
// SAME descriptor without dlopen — the byte-identity reference slice (d) compares
// the loaded .so against (D4).
const RemoctPlugin* remoct_stream_plugin_query() { return &STREAM_PLUGIN; }

// The ONE exported symbol (C linkage via remoct_plugin.h's extern "C"). This is
// what core::loadPlugin resolves from the .so/.dll (slice c). Same descriptor.
REMOCT_EXPORT const RemoctPlugin* remoct_plugin_query(void) { return &STREAM_PLUGIN; }
