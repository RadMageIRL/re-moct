/* plugin_sine.c — the disposable Phase-4 slice-(a) test plugin.
 *
 * A pure-C source that generates a deterministic, dependency-free waveform. Two
 * jobs: (1) prove remoct_plugin.h is genuinely C-consumable — this TU compiles
 * as C, so if anything C++ had leaked into the ABI header it would fail here;
 * (2) exercise the full loader lifecycle (load -> version-gate -> create ->
 * open -> read_frames -> metadata -> config -> close -> destroy) from a real
 * .dll/.so on both platforms, BEFORE StreamSource is extracted behind the ABI.
 *
 * Zero dependencies on purpose (no libm): a per-frame ramp (counter & 0xFF)/256
 * is deterministic and non-silent, so the loader test can assert exact sample
 * values without a math library adding a transitive dlopen at plugin-load time.
 * Throwaway — deleted (or kept as a fixture) once the real stream plugin lands.
 */
#include "core/remoct_plugin.h"

#include <stdlib.h>   /* calloc, free   */
#include <string.h>   /* strncmp, strcmp, memcpy, strlen */

typedef struct SineState {
    uint32_t counter;         /* per-frame ramp position                 */
    int32_t  opened;
    int32_t  paused;
    int32_t  prefer_digital;  /* set via set_config, to prove the channel */
} SineState;

static void* sine_create(const RemoctHostServices* host, void* host_ctx) {
    (void)host; (void)host_ctx;   /* the sine source needs no host services */
    return calloc(1, sizeof(SineState));
}

static void sine_destroy(void* self) { free(self); }

static int32_t sine_handles_url(void* self, const char* url) {
    (void)self;
    return (url && strncmp(url, "sine:", 5) == 0) ? 1 : 0;
}

static int32_t sine_open(void* self, const char* url) {
    (void)url;
    ((SineState*)self)->opened = 1;
    return 0;   /* 0 = ok */
}

static uint32_t sine_read_frames(void* self, float* dst, uint32_t frame_count) {
    SineState* s = (SineState*)self;
    uint32_t i;
    for (i = 0; i < frame_count; ++i) {
        float v = s->paused ? 0.0f : (float)(s->counter & 0xFFu) / 256.0f;
        dst[2 * i]     = v;   /* L */
        dst[2 * i + 1] = v;   /* R */
        if (!s->paused) ++s->counter;
    }
    return frame_count;   /* live-source contract: always fill the buffer */
}

static void sine_get_caps(void* self, RemoctSourceCaps* out) {
    (void)self;
    out->seekable = 0;
    out->finite   = 0;
    out->live     = 1;
}

static double  sine_position_sec(void* self) { (void)self; return 0.0; }
static double  sine_duration_sec(void* self) { (void)self; return 0.0; }
static int32_t sine_seek_to(void* self, double seconds) { (void)self; (void)seconds; return 0; }
static void    sine_set_paused(void* self, int32_t paused) { ((SineState*)self)->paused = paused; }
static int32_t sine_is_buffering(void* self) { (void)self; return 0; }
static void    sine_close(void* self) { ((SineState*)self)->opened = 0; }

/* snprintf-style fill: copy into the host's buffer, return the FULL length. */
static size_t fill(char* buf, size_t cap, const char* s) {
    size_t len = strlen(s);
    if (buf && cap) {
        size_t n = (len < cap - 1) ? len : cap - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
    }
    return len;
}

static size_t sine_now_playing(void* self, char* buf, size_t cap) {
    (void)self; return fill(buf, cap, "Sine - 440Hz");
}
static size_t sine_art_url(void* self, char* buf, size_t cap) {
    (void)self; return fill(buf, cap, "");
}
static size_t sine_last_error(void* self, char* buf, size_t cap) {
    (void)self; return fill(buf, cap, "");
}

static void sine_set_config(void* self, const char* key, const char* value) {
    SineState* s = (SineState*)self;
    if (key && strcmp(key, "prefer_digital") == 0)
        s->prefer_digital = (value && value[0] == '1') ? 1 : 0;
}

/* Designated initializers (C99) — order-independent and additive-growth-friendly. */
static const RemoctPlugin SINE_PLUGIN = {
    .abi_version  = REMOCT_ABI_VERSION,
    .struct_size  = (uint32_t)sizeof(RemoctPlugin),
    .name         = "sine",
    .display_name = "Sine test source (440Hz)",
    .create       = sine_create,
    .destroy      = sine_destroy,
    .handles_url  = sine_handles_url,
    .open         = sine_open,
    .read_frames  = sine_read_frames,
    .get_caps     = sine_get_caps,
    .position_sec = sine_position_sec,
    .duration_sec = sine_duration_sec,
    .seek_to      = sine_seek_to,
    .set_paused   = sine_set_paused,
    .is_buffering = sine_is_buffering,
    .close        = sine_close,
    .now_playing  = sine_now_playing,
    .art_url      = sine_art_url,
    .last_error   = sine_last_error,
    .set_config   = sine_set_config,
};

REMOCT_EXPORT const RemoctPlugin* remoct_plugin_query(void) { return &SINE_PLUGIN; }
