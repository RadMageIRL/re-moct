// plugin_hls_parity_test.cpp — Phase 4 slice d: the identical-to-compiled-in gate.
//
// Proves the LOADED remoct_stream.{so,dll} produces byte-identical PCM to the
// compiled-in reference, given identical synthetic HLS/ADTS fed through the REAL
// host HTTP service crossing (NOT the plugin's old core::http(), which no longer
// exists in the plugin — grep core::http() plugins/stream/ = 0 since slice c):
//
//   FakeHls (core::IHttp)
//     -> core::HostServices(fake).table()            (host fills RemoctHostServices)
//        -> plugin create() builds plugin::HostServiceHttp over that table
//           -> StreamSource.fetch() -> HostServiceHttp -> RemoctHostServices.http_fetch
//              -> svc_fetch (PluginHostServices) -> HostServices::http() -> FakeHls
//
// Both the compiled-in reference (remoct_stream_plugin_query, kept for exactly
// this per slice-c D4) and the loaded .so cross the SAME svc.table(), so the ONLY
// variable is compiled-in symbol vs dlopen/dlsym — a one-variable controlled
// experiment. Byte-identical PCM out the far side proves the request/response
// marshal + host-malloc/free round-trip across the C ABI is transparent, not just
// that "some HTTP happened". segment_gets>0 makes the crossing observable.
//
// Three runs, one fixture:
//   refA, refB = compiled-in adapter (SAME descriptor)  -> DETERMINISM self-check
//   loaded     = core::loadPlugin(REMOCT_STREAM_PATH)    -> the .so/.dll
// If refA != refB the protocol flaked (that assert fails FIRST) — a flake can
// never masquerade as a .so fidelity bug.
//
// Determinism rests on: FDK-AAC decode is fixed-point (flag-independent); the ring
// is int16 and at 44100 the resampler is BYPASSED (no float-flag-sensitive stage);
// int16->float is x/32768 (exact, power-of-two); and the drain is a fixed head
// window captured AFTER prebuffer with DRAIN < PREBUFFER, so the ring can never
// underrun during it -> no silence-fill -> the captured head is fixed decoded audio
// regardless of producer thread timing. The RESAMPLE path's byte-identity is
// deliberately NOT asserted (a 48k fixture would be float-flag-sensitive across the
// two separately-compiled targets); it stays covered behaviorally by the live gates.

#define MINIAUDIO_IMPLEMENTATION            // StreamSource.cpp holds decls only
#include "miniaudio.h"

#include "hls_fixture.h"            // encodeAdtsSine / FakeHls / rmsOf / waitFor
#include "StreamPluginAdapter.h"   // remoct_stream_plugin_query() (compiled-in ref, D4)
#include "PluginSource.h"          // core::PluginSource (the driver AudioManager uses)
#include "PluginHostServices.h"    // core::HostServices over an IHttp
#include "PluginHost.h"            // core::loadPlugin / LoadedPlugin / pluginLoadName

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Flat, UNPACED drain through PluginSource::readFrames. Called only after prebuffer
// with frames < PREBUFFER_SAMPLES, so the ring never underruns and the head window
// is fixed decoded audio (no silence-fill) — the determinism crux.
static std::vector<float> drainFlat(core::PluginSource& src, int frames) {
    std::vector<float> all;
    all.reserve((size_t)frames * 2);
    float buf[512 * 2];
    int left = frames;
    while (left > 0) {
        int n = left < 512 ? left : 512;
        src.readFrames(buf, (uint32_t)n);   // live source always fills n
        all.insert(all.end(), buf, buf + (size_t)n * 2);
        left -= n;
    }
    return all;
}

// One run: drive `desc` through the REAL host HTTP service crossing (svc.table()).
// NO core::setHttp — the plugin reaches HTTP only through the injected table.
static std::vector<float> runOne(const RemoctPlugin* desc,
                                 const std::vector<uint8_t>& segAudio,
                                 int drainFrames, int& out_segment_gets) {
    FakeHls fake;
    fake.seg_audio = segAudio;                 // identical fixture bytes every run
    // (no ID3 -> pure-audio segments; nowPlaying is not part of the PCM proof)

    core::HostServices svc(fake);              // FakeHttp AS the host HTTP service
    core::PluginSource src(desc, svc.table()); // create() -> plugin HostServiceHttp over the table
    if (!src.valid()) { out_segment_gets = -1; return {}; }

    src.open(FakeHls::MASTER);
    // Prime keeps 2 of the 4 advertised segments (4s) > PREBUFFER_SEC (3s).
    waitFor(10000, [&]{ return !src.buffering(); });    // prebuffered
    std::vector<float> pcm = drainFlat(src, drainFrames);
    src.close();
    out_segment_gets = fake.segment_gets.load();
    return pcm;
}

static bool bytesEqual(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

int main() {
    // 44100/stereo sine ADTS, ~2s per segment (resampler bypassed -> fixed-point path).
    std::vector<uint8_t> segAudio = encodeAdtsSine(2.0, 440.0, 0.5);
    CHECK(segAudio.size() > 10000);
    CHECK(segAudio.size() >= 2 && segAudio[0] == 0xFF && (segAudio[1] & 0xF6) == 0xF0);

    // DRAIN (~1.5s) < PREBUFFER (3s @ 44100 = 132300) -> head-drain-under-prebuffer.
    const int DRAIN = 66150;   // frames ~= 1.5 s

    int gA = 0, gB = 0, gL = 0;
    std::vector<float> refA   = runOne(remoct_stream_plugin_query(), segAudio, DRAIN, gA);
    std::vector<float> refB   = runOne(remoct_stream_plugin_query(), segAudio, DRAIN, gB);

    core::PluginLoad why = core::PluginLoad::Ok;
    auto lp = core::loadPlugin(REMOCT_STREAM_PATH, &why);
    CHECK(lp != nullptr);
    if (!lp) {
        std::printf("FAIL loadPlugin(%s): %s\n", REMOCT_STREAM_PATH, core::pluginLoadName(why));
        std::printf("plugin_hls_parity_test: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::vector<float> loaded = runOne(lp->plugin(), segAudio, DRAIN, gL);

    // ── Gate ──────────────────────────────────────────────────────────────────
    CHECK(refA.size() == (size_t)DRAIN * 2);              // captured the full head window
    CHECK(gA > 0 && gB > 0 && gL > 0);                    // all three fetched THROUGH the shim
    CHECK(bytesEqual(refA, refB));                        // DETERMINISM (fails first if it flakes)
    CHECK(bytesEqual(refA, loaded));                      // THE THESIS: loaded == compiled-in
    CHECK(rmsOf(refA) > 0.15);                            // real decoded audio, not two silences

    std::printf("  segment_gets refA=%d refB=%d loaded=%d ; rms=%.3f ; frames=%zu ; "
                "refA==refB=%d refA==loaded=%d\n",
                gA, gB, gL, rmsOf(refA), refA.size() / 2,
                (int)bytesEqual(refA, refB), (int)bytesEqual(refA, loaded));

    if (g_fail == 0) { std::printf("plugin_hls_parity_test: ALL PASS\n"); return 0; }
    std::printf("plugin_hls_parity_test: %d FAILURE(S)\n", g_fail);
    return 1;
}
