// plugin_loader_test.cpp — Phase 4 slice (a): freeze the C ABI against a
// disposable plugin. Loads the pure-C sine plugin from a REAL .dll/.so via the
// core::IPluginLoader seam + core::loadPlugin policy, drives the full lifecycle
// through the C table, and unit-tests the version gate's reject paths with
// hand-built descriptors. Runs on BOTH matrix jobs (like ipc_echo_test).
//
// SINE_PLUGIN_PATH is the built plugin's absolute path, injected by CMake
// ($<TARGET_FILE:plugin_sine>).
#include "PluginHost.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  ok   %s\n", msg); } \
    else      { std::printf("  FAIL %s  (line %d)\n", msg, __LINE__); ++g_fail; } \
} while (0)

// ── stub fns for hand-built descriptors (reject-path coverage) ───────────────
static void*   stub_create(const RemoctHostServices*, void*) { return (void*)1; }
static void    stub_destroy(void*) {}
static int32_t stub_open(void*, const char*) { return 0; }
static uint32_t stub_read(void*, float*, uint32_t n) { return n; }
static void    stub_close(void*) {}

// A descriptor with the five REQUIRED fns non-null, current ABI, full size.
static RemoctPlugin goodDescriptor() {
    RemoctPlugin d;
    std::memset(&d, 0, sizeof(d));
    d.abi_version = REMOCT_ABI_VERSION;
    d.struct_size = (uint32_t)sizeof(RemoctPlugin);
    d.name        = "stub";
    d.create      = stub_create;
    d.destroy     = stub_destroy;
    d.open        = stub_open;
    d.read_frames = stub_read;
    d.close       = stub_close;
    return d;
}

static void testValidation() {
    std::printf("[validate] the version gate's reject paths\n");
    CHECK(core::validatePlugin(nullptr, REMOCT_ABI_VERSION) == core::PluginLoad::NullDescriptor,
          "null descriptor -> NullDescriptor");

    RemoctPlugin good = goodDescriptor();
    CHECK(core::validatePlugin(&good, REMOCT_ABI_VERSION) == core::PluginLoad::Ok,
          "well-formed descriptor -> Ok");

    RemoctPlugin badAbi = goodDescriptor();
    badAbi.abi_version = REMOCT_ABI_VERSION + 1u;   // an incompatible future plugin
    CHECK(core::validatePlugin(&badAbi, REMOCT_ABI_VERSION) == core::PluginLoad::AbiMismatch,
          "wrong abi_version -> AbiMismatch (the major gate)");

    RemoctPlugin tooSmall = goodDescriptor();
    tooSmall.struct_size = 8;   // doesn't reach through the required fields
    CHECK(core::validatePlugin(&tooSmall, REMOCT_ABI_VERSION) == core::PluginLoad::TooSmall,
          "truncated struct_size -> TooSmall");

    RemoctPlugin missing = goodDescriptor();
    missing.read_frames = nullptr;   // a required fn absent
    CHECK(core::validatePlugin(&missing, REMOCT_ABI_VERSION) == core::PluginLoad::MissingRequiredFn,
          "null required fn -> MissingRequiredFn");
}

static void testBogusPath() {
    std::printf("[load] a non-existent module is refused, not crashed\n");
    core::PluginLoad why = core::PluginLoad::Ok;
    auto p = core::loadPlugin("does-not-exist.remoct-plugin", &why);
    CHECK(!p && why == core::PluginLoad::ModuleLoadFailed,
          "bogus path -> nullptr + ModuleLoadFailed");
}

static void testRealSinePlugin() {
    std::printf("[load] the real sine .dll/.so through the seam: %s\n", SINE_PLUGIN_PATH);
    core::PluginLoad why = core::PluginLoad::MissingRequiredFn;
    auto loaded = core::loadPlugin(SINE_PLUGIN_PATH, &why);
    CHECK(loaded != nullptr, "loadPlugin succeeded");
    if (!loaded) { std::printf("  (reason: %s) — cannot continue lifecycle\n",
                               core::pluginLoadName(why)); return; }

    const RemoctPlugin* pl = loaded->plugin();
    CHECK(pl->abi_version == REMOCT_ABI_VERSION, "descriptor abi_version == host");
    CHECK(std::strcmp(pl->name, "sine") == 0, "descriptor name == \"sine\"");

    std::printf("[lifecycle] create -> open -> read_frames -> metadata -> config -> close -> destroy\n");
    void* self = pl->create(nullptr, nullptr);   // sine ignores host services
    CHECK(self != nullptr, "create returned an instance");
    if (!self) return;

    CHECK(pl->handles_url(self, "sine://test") == 1, "handles_url(sine://) == 1");
    CHECK(pl->handles_url(self, "http://x")    == 0, "handles_url(http://) == 0");
    CHECK(pl->open(self, "sine://test") == 0, "open() == 0");

    RemoctSourceCaps caps; std::memset(&caps, 0, sizeof(caps));
    pl->get_caps(self, &caps);
    CHECK(caps.live == 1 && caps.finite == 0 && caps.seekable == 0,
          "caps = {live, !finite, !seekable}");

    // read_frames: deterministic ramp, L==R, always fills the buffer.
    const uint32_t N = 512;
    float buf[512 * 2];
    std::memset(buf, 0x7f, sizeof(buf));
    uint32_t got = pl->read_frames(self, buf, N);
    CHECK(got == N, "read_frames fills the whole buffer (live-source contract)");
    CHECK(buf[0] == 0.0f && buf[1] == 0.0f, "frame 0 == 0 (ramp start), L==R");
    CHECK(buf[2] == 1.0f / 256.0f && buf[3] == buf[2], "frame 1 == 1/256, L==R");
    bool nonzero = false;
    for (uint32_t i = 0; i < N * 2; ++i) if (buf[i] != 0.0f) { nonzero = true; break; }
    CHECK(nonzero, "produced non-silent audio");

    // paused -> silence, still fills.
    pl->set_paused(self, 1);
    std::memset(buf, 0x7f, sizeof(buf));
    got = pl->read_frames(self, buf, N);
    bool allzero = (got == N);
    for (uint32_t i = 0; i < N * 2; ++i) if (buf[i] != 0.0f) { allzero = false; break; }
    CHECK(allzero, "paused -> full buffer of silence");
    pl->set_paused(self, 0);

    // metadata: exact value + the snprintf grow semantics (host-owned buffer,
    // full length returned so the host can grow+retry).
    char big[64];
    size_t need = pl->now_playing(self, big, sizeof(big));
    CHECK(need == 12 && std::strcmp(big, "Sine - 440Hz") == 0,
          "now_playing == \"Sine - 440Hz\", returns full length");
    char tiny[5];
    size_t need2 = pl->now_playing(self, tiny, sizeof(tiny));
    CHECK(need2 == 12 && std::strlen(tiny) == 4 && std::strcmp(tiny, "Sine") == 0,
          "tiny buffer truncates to cap-1 + NUL, still returns full length");

    // config channel + the rest of the transport surface (smoke — no crash).
    pl->set_config(self, "prefer_digital", "1");
    (void)pl->position_sec(self);
    (void)pl->duration_sec(self);
    (void)pl->seek_to(self, 10.0);
    (void)pl->is_buffering(self);
    char err[64]; pl->last_error(self, err, sizeof(err));
    CHECK(err[0] == '\0', "last_error empty on the happy path");

    pl->close(self);
    pl->destroy(self);
    std::printf("  ok   closed + destroyed (no leak/crash)\n");
}

int main() {
    std::printf("== plugin_loader_test (Phase 4 slice a) ==\n");
    testValidation();
    testBogusPath();
    testRealSinePlugin();

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
