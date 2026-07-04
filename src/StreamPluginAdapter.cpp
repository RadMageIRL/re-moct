// StreamPluginAdapter.cpp — the RemoctPlugin C-ABI adapter over StreamSource
// (Phase 4 slice b). Each C entry forwards 1:1 to the in-tree StreamSource; the
// instance (`self`) IS a StreamSource. In slice (c) this file + StreamSource move
// into plugins/stream/ and gain the exported remoct_plugin_query; here it is
// compiled into the host and reached directly (StreamPluginAdapter.h).
//
// The callbacks are internal-linkage (anonymous namespace) and noexcept (the ABI
// forbids exceptions across the line — StreamSource's queried surface does not
// throw). Assigning C++ statics to the RemoctPlugin C function-pointer members is
// the same pattern the codebase already uses for miniaudio's C callbacks
// (AudioManager::maDataCallback). HTTP still goes through core::http() here;
// routing it through the injected host services is the slice-(c) rewire (the
// descriptor already receives the services at create).
#include "StreamPluginAdapter.h"
#include "StreamSource.h"

#include <cstring>
#include <string>

namespace {

// The instance behind `self`. Holds the source + the host services it was handed
// at create (unused in (b); the (c) HTTP rewire consumes them).
struct StreamInstance {
    StreamSource              src;
    const RemoctHostServices* host = nullptr;
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
    auto* i = new (std::nothrow) StreamInstance();
    if (i) i->host = host;
    return i;
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
    if (key && std::strcmp(key, "prefer_digital") == 0)
        inst(self)->src.setPreferDigital(value && value[0] == '1');
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

const RemoctPlugin* remoct_stream_plugin_query() { return &STREAM_PLUGIN; }
