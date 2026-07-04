// plugin_stream_test — Phase 4 slice (b): the streaming source driven through
// the plugin C ABI. Exercises the in-process StreamSource adapter
// (remoct_stream_plugin_query) two ways: (1) the raw C table over a real
// StreamSource `self` (proves the adapter forwards caps/lifecycle/metadata), and
// (2) core::PluginSource, the host driver AudioManager now uses (proves the
// wrapper + host-tracked url/paused + snprintf-grow metadata). No audio fixture:
// a dead-port open() fails fast (no producer thread), so this stays headless and
// quick on both jobs. The end-to-end audio path is the slice-(b) live gate.
#define MINIAUDIO_IMPLEMENTATION   // provide the impl for the linked StreamSource
#include "miniaudio.h"             // (no AudioManager TU here — same as hls_pipeline_test)

#include "PluginSource.h"
#include "PluginHostServices.h"
#include "StreamPluginAdapter.h"
#include "core/remoct_plugin.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) std::printf("  ok   %s\n", msg); \
    else    { std::printf("  FAIL %s  (line %d)\n", msg, __LINE__); ++g_fail; } \
} while (0)

static const char* kDeadUrl = "http://127.0.0.1:1/dead.mp3";  // refused immediately

int main() {
    std::printf("== plugin_stream_test (Phase 4 slice b) ==\n");

    const RemoctPlugin* d = remoct_stream_plugin_query();
    CHECK(d && d->abi_version == REMOCT_ABI_VERSION, "stream descriptor, current ABI");
    CHECK(d && std::strcmp(d->name, "stream") == 0, "descriptor name == \"stream\"");
    if (!d) { std::printf("FAILED\n"); return 1; }

    CHECK(d->handles_url(nullptr, "https://revma.ihrhls.com/zc1469/hls.m3u8") == 1,
          "handles_url: an https stream -> yes");
    CHECK(d->handles_url(nullptr, "/home/user/song.mp3") == 0,
          "handles_url: a local file -> no (host built-in)");

    core::HostServices svc;   // over core::http(); handed to the plugin at create

    // ── (1) drive the raw C table over a real StreamSource self ──────────────
    std::printf("[adapter] C table over a real StreamSource instance\n");
    {
        void* self = d->create(svc.table(), nullptr);
        CHECK(self != nullptr, "create() -> instance");

        RemoctSourceCaps caps; std::memset(&caps, 0, sizeof(caps));
        d->get_caps(self, &caps);
        CHECK(caps.live == 1 && caps.finite == 0 && caps.seekable == 0,
              "caps = live, !finite, !seekable");
        CHECK(d->seek_to(self, 5.0) == 0, "seek_to on a live source -> refused");

        char np[128]; std::size_t n = d->now_playing(self, np, sizeof(np));
        CHECK(n == 0 && np[0] == '\0', "now_playing empty before open");

        d->set_config(self, "prefer_digital", "1");   // config channel, no crash
        int r = d->open(self, kDeadUrl);
        CHECK(r != 0, "open() to a dead endpoint fails fast (nonzero)");

        float buf[64 * 2]; std::memset(buf, 0x7f, sizeof(buf));
        uint32_t got = d->read_frames(self, buf, 64);
        CHECK(got == 64, "read_frames fills the buffer (live-source contract)");

        d->close(self);
        d->destroy(self);
        std::printf("  ok   closed + destroyed\n");
    }

    // ── (2) drive core::PluginSource (the host driver) ───────────────────────
    std::printf("[PluginSource] the host-side driver AudioManager uses\n");
    {
        core::PluginSource ps(d, svc.table());
        CHECK(ps.valid(), "PluginSource created its instance");
        CHECK(ps.open(kDeadUrl) == false, "open() fails fast on a dead endpoint");
        CHECK(ps.url() == kDeadUrl, "url() is host-tracked");
        CHECK(ps.nowPlaying().empty(), "nowPlaying() empty");
        CHECK(ps.currentArtUrl().empty(), "currentArtUrl() empty");

        ps.pause(true);  CHECK(ps.paused() == true,  "paused() host-tracked (set)");
        ps.pause(false); CHECK(ps.paused() == false, "paused() host-tracked (clear)");
        ps.setPreferDigital(true);                    // config channel, no crash

        float buf[64 * 2];
        uint32_t got = ps.readFrames(buf, 64);
        CHECK(got == 64, "readFrames fills (silence while not playing)");

        ps.close();
        std::printf("  ok   closed (instance kept; dtor destroys)\n");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
