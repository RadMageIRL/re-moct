// xfade_handoff_test.cpp — Slice 0 (Phase 2 prep): thin replay net, file-path half.
//
// Drives the REAL AudioManager end-to-end on generated WAVs through a REAL (muted)
// playback device, exercising exactly the swap-adjacent machinery that Phase 2's
// Slice B restructures — the behavior a diff audit alone cannot protect:
//   1. gapless handoff: EOF short-read -> next_decoder_ fill -> initCrossfade swap
//      on the audio thread -> track_swap_flag_ -> pollEvents installs
//      current_track_ = next_track_info_ -> on_track_end fires (in that order)
//   2. crossfade handoff: trigger window (dur - pos <= crossfade_secs), two
//      decoders mixed in one callback, isCrossfading() visible to the UI,
//      completion -> swap -> preload_next flag (NOT track_ended)
//   3. clearNext() mid-crossfade: the teardownNext device-stop branch — the
//      in-flight next_decoder_ read must be quiesced before the free, playback
//      of the outgoing track continues to its natural end, no swap installed
//   4. track end with NO preload: short read -> silence tail -> track_ended,
//      current track unchanged
//
// Needs a real audio device (WASAPI); output is muted + volume 0. If no device
// can be opened the test SKIPs (exit 77) rather than fails — the pipeline tests
// (cd/hls) remain the always-on net. Real-time cadence is the device's own
// callback clock, so unlike the pipeline tests this half runs at 1x and takes
// ~15s wall clock.
#ifdef _WIN32
#include "AudioManager.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ─── Fixture: canonical 16-bit stereo 44100 WAV, sine at `freq` ────────────────

static bool writeWav(const std::string& path, double secs, double freq) {
    const uint32_t sr = 44100, ch = 2;
    const uint32_t frames     = (uint32_t)(secs * sr);
    const uint32_t data_bytes = frames * ch * 2;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16);
    u16(1); u16((uint16_t)ch); u32(sr); u32(sr * ch * 2); u16((uint16_t)(ch * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(data_bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t s = (int16_t)(0.4 * 32767.0 * std::sin(2.0 * 3.14159265358979 * freq * (double)i / sr));
        u16((uint16_t)s); u16((uint16_t)s);
    }
    std::fclose(f);
    return true;
}

// ─── pollEvents pump (the test main thread plays the app's UI-loop role) ──────

static std::atomic<int> g_end{0}, g_pre{0};

template <typename Pred>
static bool pumpUntil(AudioManager& am, Pred pred, int timeout_ms) {
    DWORD t0 = GetTickCount();
    while ((long)(GetTickCount() - t0) < timeout_ms) {
        am.pollEvents();
        if (pred()) return true;
        Sleep(5);
    }
    am.pollEvents();
    return pred();
}

int main() {
    const std::string A = "xfade_a.wav", B = "xfade_b.wav", C = "xfade_c.wav";   // ctest cwd
    CHECK(writeWav(A, 2.0, 440.0));
    CHECK(writeWav(B, 2.0, 880.0));
    CHECK(writeWav(C, 2.0, 660.0));
    if (g_fail) return 1;

    AudioManager am;
    am.setVolume(0.0f);
    am.toggleMute();                                     // belt and braces: silent run
    am.setTrackEndCallback([]  { ++g_end; });
    am.setPreloadNextCallback([]{ ++g_pre; });

    // Device probe: the first play brings up the (warmed) backend. No device on
    // this machine -> SKIP, not FAIL (the cd/hls pipeline tests are the always-on net).
    if (!am.play(A)) {
        std::printf("xfade_handoff_test: SKIP (no audio device / first play failed)\n");
        return 77;
    }
    CHECK(am.state() == PlaybackState::Playing);
    CHECK(am.currentTrack().path == A);
    am.stop();

    // ── 1. Gapless handoff (crossfade_secs = 0): EOF -> swap -> install -> cb ──
    {
        g_end = 0; g_pre = 0;
        am.crossfade_secs = 0.0f;
        CHECK(am.play(A));
        CHECK(am.preloadNext(B));
        am.seekTo(1.4);                                   // ~0.6s to EOF
        CHECK(pumpUntil(am, []{ return g_end.load() >= 1; }, 6000));
        CHECK(am.currentTrack().path == B);               // swap installed before the cb fired
        CHECK(am.state() == PlaybackState::Playing);      // B is the live track now
        CHECK(pumpUntil(am, []{ return g_end.load() >= 2; }, 6000));  // B plays to its own end
        CHECK(am.currentTrack().path == B);               // no next -> no further swap
        am.stop();
    }

    // ── 2. Crossfade handoff: trigger -> two-decoder mix -> swap -> preload cb ──
    {
        g_end = 0; g_pre = 0;
        am.crossfade_secs = 0.5f;
        CHECK(am.play(A));
        CHECK(am.preloadNext(B));
        am.seekTo(1.2);                                   // trigger at pos >= 1.5
        bool saw_xfade = false;
        {
            DWORD t0 = GetTickCount();
            while ((long)(GetTickCount() - t0) < 4000) {
                am.pollEvents();
                if (am.isCrossfading()) { saw_xfade = true; break; }
                Sleep(1);
            }
        }
        CHECK(saw_xfade);                                 // the UI-visible mix window
        CHECK(pumpUntil(am, []{ return g_pre.load() >= 1; }, 4000));  // completion path
        CHECK(g_end.load() == 0);                         // crossfade swap is NOT a track_end
        CHECK(am.currentTrack().path == B);
        CHECK(pumpUntil(am, []{ return g_end.load() >= 1; }, 6000));  // B's own EOF
        am.stop();
    }

    // ── 3. clearNext() mid-crossfade: quiesce, keep playing, no swap ──────────
    {
        g_end = 0; g_pre = 0;
        am.crossfade_secs = 1.5f;
        CHECK(am.play(A));
        CHECK(am.preloadNext(B));
        am.seekTo(0.9);                                   // already inside the trigger window
        bool saw_xfade = false;
        {
            DWORD t0 = GetTickCount();
            while ((long)(GetTickCount() - t0) < 4000) {
                am.pollEvents();
                if (am.isCrossfading()) { saw_xfade = true; break; }
                Sleep(1);
            }
        }
        CHECK(saw_xfade);
        am.clearNext();                                   // the device-stop teardown branch
        CHECK(am.state() == PlaybackState::Playing);      // A keeps playing
        CHECK(!am.isCrossfading());
        CHECK(pumpUntil(am, []{ return g_end.load() >= 1; }, 6000));  // A's natural end
        CHECK(am.currentTrack().path == A);               // no swap was installed
        CHECK(g_pre.load() == 0);
        am.stop();
    }

    // ── 4. Track end with no preload: short read -> silence -> track_ended ────
    {
        g_end = 0; g_pre = 0;
        am.crossfade_secs = 0.0f;
        CHECK(am.play(A));
        am.seekTo(1.5);
        CHECK(pumpUntil(am, []{ return g_end.load() >= 1; }, 5000));
        CHECK(am.currentTrack().path == A);
        am.stop();
    }

    // ── 5. Replace a preload mid-play: the non-crossfading teardownNext branch ─
    // preloadNext(C) while B is already preloaded discards B (un-publish first,
    // then free — with no crossfade in flight this is the direct-free branch),
    // and C must be the track installed at the gapless swap.
    {
        g_end = 0; g_pre = 0;
        am.crossfade_secs = 0.0f;
        CHECK(am.play(A));
        CHECK(am.preloadNext(B));
        CHECK(am.preloadNext(C));                         // replaces B mid-play
        am.seekTo(1.4);
        CHECK(pumpUntil(am, []{ return g_end.load() >= 1; }, 6000));
        CHECK(am.currentTrack().path == C);               // C installed, B discarded
        CHECK(am.state() == PlaybackState::Playing);
        am.stop();
    }

    std::remove(A.c_str());
    std::remove(B.c_str());
    std::remove(C.c_str());

    if (g_fail == 0) { std::printf("xfade_handoff_test: ALL PASS\n"); return 0; }
    std::printf("xfade_handoff_test: %d FAILURE(S)\n", g_fail);
    return 1;
}
#else
int main() { return 0; }
#endif
