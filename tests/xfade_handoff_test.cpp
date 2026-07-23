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
#include "PlaylistManager.h"
#include "NextArm.h"

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

static bool writeWav(const std::string& path, double secs, double freq, double amp = 0.4) {
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
        int16_t s = (int16_t)(amp * 32767.0 * std::sin(2.0 * 3.14159265358979 * freq * (double)i / sr));
        u16((uint16_t)s); u16((uint16_t)s);
    }
    std::fclose(f);
    return true;
}

// ─── Viz-ring peak: the only public window onto the mixed callback output ─────
// copySamples() drains the last n mono samples the callback pushed. The callback
// feeds the ring from the PROCESSED output but BEFORE the mute memset
// (AudioManager.cpp, "Feed visualizer ... but BEFORE mute"), and setVolume() only
// moves the device master volume — neither scales what lands here. So a muted,
// zero-volume run still reports true post-mix sample amplitude.
static float vizPeak(const AudioManager& am) {
    float buf[256];
    int n = am.copySamples(buf, 256);   // const — reads the ring, no mutation
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) { float a = std::fabs(buf[i]); if (a > pk) pk = a; }
    return pk;
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

    // ═══ XF REGRESSIONS (were the XF-P probes) ═══════════════════════════════
    // These began as printf-only probes that measured the two defects. Now that
    // C1/C2 fix them they assert. Each printf is retained: when one of these
    // fails, the measured value is the diagnosis.

    // ── P1 (C1): preloadNext() inside the post-swap, pre-install window ───────
    // Forces: A hits EOF -> the AUDIO thread runs initCrossfade() (swap + set
    // track_swap_flag_) -> WITHOUT pollEvents() draining that flag, the main
    // thread calls preloadNext(C) -> then pollEvents(). Reports which identity
    // survives into current_track_.
    //
    // Swap detection without pumping: initCrossfade stores cached_duration_ =
    // next_track_info_.duration_sec (AudioManager.cpp:382) two statements BEFORE
    // track_swap_flag_.store(true) (:384). durationSec() (:507) returns that
    // atomic directly and is the only swap-visible observable that does not
    // require pollEvents() — currentTrack() cannot serve, since it is written
    // only inside pollEvents(). Distinct per-fixture durations make it a clean
    // discriminator: P1A 2.0s -> P1B 3.0s.
    {
        const std::string P1A = "xfp_a2.wav", P1B = "xfp_b3.wav", P1C = "xfp_c4.wav";
        CHECK(writeWav(P1A, 2.0, 440.0));
        CHECK(writeWav(P1B, 3.0, 880.0));
        CHECK(writeWav(P1C, 4.0, 660.0));

        g_end = 0; g_pre = 0;
        am.crossfade_secs = 0.0f;                 // gapless swap path
        CHECK(am.play(P1A));
        am.pollEvents();                          // install P1A as current
        CHECK(am.preloadNext(P1B));
        std::printf("P1 armed:        current=%s dur_field=%d durationSec=%.2f\n",
                    am.currentTrack().path.c_str(),
                    am.currentTrack().duration_sec, am.durationSec());
        am.seekTo(1.4);                           // ~0.6s to EOF

        bool swapped = false;
        DWORD t0 = GetTickCount();
        while ((long)(GetTickCount() - t0) < 6000) {
            if (am.durationSec() > 2.5) { swapped = true; break; }   // P1B's 3.0s
            Sleep(1);                             // NOTE: no pollEvents() here
        }
        CHECK(swapped);
        Sleep(5);   // close the ~microsecond gap between :382 and :384 with a
                    // ~1000x margin; nothing but pollEvents() drains the flag,
                    // and this thread is the only caller of it.

        std::printf("P1 post-swap:    current=%s dur_field=%d durationSec=%.2f  "
                    "(no pollEvents since swap)\n",
                    am.currentTrack().path.c_str(),
                    am.currentTrack().duration_sec, am.durationSec());

        CHECK(am.preloadNext(P1C));               // ← the contested call
        am.pollEvents();                          // ← the install that may be lost

        const std::string got = am.currentTrack().path;
        const char* which = got == P1A ? "A" : got == P1B ? "B" : got == P1C ? "C"
                          : got.empty() ? "<empty>" : "<other>";
        std::printf("P1 RESULT:       currentTrack=%s [%s]  dur_field=%d  "
                    "durationSec=%.2f  state=%d  end_cb=%d preload_cb=%d\n",
                    got.c_str(), which, am.currentTrack().duration_sec,
                    am.durationSec(), (int)am.state(), g_end.load(), g_pre.load());
        std::printf("P1 COHERENCE:    path=%s dur_field=%d vs durationSec=%.2f -> %s\n",
                    which, am.currentTrack().duration_sec, am.durationSec(),
                    (std::fabs((double)am.currentTrack().duration_sec - am.durationSec()) < 0.6)
                        ? "AGREE" : "MISMATCH (torn identity)");

        // C1: the identity installed must be the track that actually swapped in.
        // Pre-C1 this is P1A (the swap was cancelled by preloadNext's teardown);
        // with the flag clear merely MOVED it is P1C (a track never audible).
        CHECK(got == P1B);
        // C1: and the payload must be coherent — current_track_'s duration field
        // must agree with the audio thread's cached_duration_, both describing B.
        CHECK(std::fabs((double)am.currentTrack().duration_sec - am.durationSec()) < 0.6);
        am.stop();
        std::remove(P1A.c_str()); std::remove(P1B.c_str()); std::remove(P1C.c_str());
    }

    // ── P2: repeat-one crossfade contamination baseline ───────────────────────
    // Claim under test: with setRepeatOne(true) and a next track armed, the
    // trigger at AudioManager.cpp:791 fires regardless, and the next track is
    // AUDIBLE for crossfade_secs before initCrossfade (:358-362) declines the swap.
    // Discriminator: a SILENT current track against a full-scale next, so any
    // non-zero viz energy is unambiguously the next track's contribution.
    {
        const std::string P2S = "xfp_silent.wav", P2L = "xfp_loud.wav";
        CHECK(writeWav(P2S, 2.0, 440.0, 0.0));    // silent A
        CHECK(writeWav(P2L, 3.0, 880.0, 0.9));    // full-scale B

        g_end = 0; g_pre = 0;
        am.crossfade_secs = 1.0f;
        am.setRepeatOne(true);
        CHECK(am.play(P2S));
        am.pollEvents();
        CHECK(am.preloadNext(P2L));
        am.seekTo(0.2);

        Sleep(200);                               // flush the 2048-sample ring (~46ms)
        float baseline = vizPeak(am);
        std::printf("P2 baseline:     viz_peak=%.4f (silent track playing) -> %s\n",
                    baseline, baseline < 0.01f ? "clean" : "DIRTY, result suspect");

        // Sample the ring across several repeat-one loops, segmenting into
        // contamination EPISODES (cold->hot->cold) so the per-episode span can be
        // compared against crossfade_secs directly.
        int   first_ms = -1, hot = 0, total = 0;
        float max_peak = 0.0f;
        int   ep_start[8], ep_end[8], n_ep = 0;
        bool  was_hot = false;
        DWORD t0 = GetTickCount();
        while ((long)(GetTickCount() - t0) < 5000) {
            am.pollEvents();
            float pk = vizPeak(am);
            int   ms = (int)(GetTickCount() - t0);
            if (pk > max_peak) max_peak = pk;
            ++total;
            bool is_hot = (pk > 0.01f);
            if (is_hot) { ++hot; if (first_ms < 0) first_ms = ms; }
            if (is_hot && !was_hot && n_ep < 8) { ep_start[n_ep] = ms; ep_end[n_ep] = ms; ++n_ep; }
            else if (is_hot && n_ep > 0)        { ep_end[n_ep - 1] = ms; }
            was_hot = is_hot;
            Sleep(2);
        }
        std::printf("P2 RESULT:       contamination=%s  max_viz_peak=%.4f  "
                    "first=%dms  hot=%d/%d samples  vs crossfade_secs=%.2fs\n",
                    first_ms >= 0 ? "PRESENT" : "ABSENT", max_peak,
                    first_ms, hot, total, am.crossfade_secs);
        for (int i = 0; i < n_ep; ++i)
            std::printf("P2 episode %d:    %d..%dms  span=%dms  (%.0f%% of crossfade_secs)\n",
                        i + 1, ep_start[i], ep_end[i], ep_end[i] - ep_start[i],
                        100.0 * (ep_end[i] - ep_start[i]) / (am.crossfade_secs * 1000.0));

        // C2 (R1a/R1b): under repeat-one the armed next track must never reach the
        // output. The current track here is digital silence, so ANY energy in the
        // ring is the next track. Measured at 0.8977 before the fix against a
        // written amplitude of 0.90, so this threshold has ~90x margin.
        CHECK(max_peak < 0.01f);
        CHECK(n_ep == 0);
        // C2 (R1b): with the splice guarded, repeat-one falls to the silence-fill
        // branch, which restores track_ended_flag_. That is what re-enables the
        // queue drain in repeat-one (the starvation corollary). Before the fix the
        // crossfade-completion path fires preload_next instead and this stays 0.
        CHECK(g_end.load() >= 1);
        am.stop();
        am.setRepeatOne(false);
        std::remove(P2S.c_str()); std::remove(P2L.c_str());
    }

    // ── P3: PRODUCT-PATH repeat-one (the route the P2 block does not model) ───
    // P2 drives setRepeatOne() directly with counter callbacks. The product's
    // repeat mode can instead arrive via config restore (which sets ONLY the
    // playlist's mode - main.cpp never pushes it to AudioManager), and the
    // product's callbacks arm/advance through PlaylistManager. This block wires
    // the REAL PlaylistManager and main.cpp-shaped callbacks and measures three
    // scenarios Dos's hardware gate exercises. Measurement first, like XF-P.
    {
        const std::string PS = "xfp3_silent.wav", PL = "xfp3_loud.wav";
        CHECK(writeWav(PS, 2.0, 440.0, 0.0));     // silent current track
        CHECK(writeWav(PL, 3.0, 880.0, 0.9));     // full-scale playlist neighbour

        // Fresh manager per scenario - mirrors a fresh app launch.
        auto run_scenario = [&](const char* name, bool desync, double press_at_remaining) {
            // press_at_remaining: seconds before the boundary to execute the
            // r-handler sequence; <0 = never (mode is One from the start).
            AudioManager am2;
            am2.setVolume(0.0f);
            am2.toggleMute();
            am2.crossfade_secs = 1.0f;

            PlaylistManager pl;
            pl.addTrack(PS);                       // index 0 - "A", the loop track
            pl.addTrack(PL);                       // index 1 - "B", the neighbour
            pl.selectAt(0);

            // Product-shaped callbacks. NOTE (C3): main.cpp's preload callback no
            // longer arms inline - the reconcile poll does - but the arm decisions
            // modelled here (successor under All, nothing under One) are exactly
            // what the poll produces in these scenarios, so the engine sees the
            // same sequence either way. The queue loop is elided (queue stays
            // empty here); C2's guard shape is retained.
            am2.setPreloadNextCallback([&]{
                // Product shape: consume a swap-played queue head first; the
                // advance is mode-guarded because next() NAVIGATES under
                // repeat-one since XF item 4.
                if (!pl.queueEmpty() &&
                    pl.queueAt(0).path == am2.currentTrack().path) {
                    pl.queuePop();
                    return;
                }
                if (pl.queueEmpty() && pl.repeatMode() != RepeatMode::One) {
                    pl.next();
                    if (auto peek = pl.peekNext(); peek.has_value())
                        am2.preloadNext(peek.value());
                }
            });
            std::atomic<int> loops{0};
            am2.setTrackEndCallback([&]{
                ++loops;
                // Product shape: a gapless-splice advance is accounting only.
                // (Inert in these scenarios - nothing is armed at any EOF here -
                // but kept faithful to main.cpp.)
                if (am2.takeTrackEndAdvanced()) {
                    if (!pl.queueEmpty() &&
                        pl.queueAt(0).path == am2.currentTrack().path)
                        pl.queuePop();
                    else if (pl.queueEmpty() &&
                             pl.repeatMode() != RepeatMode::One)
                        pl.next();
                    return;
                }
                if (pl.repeatMode() == RepeatMode::One) {
                    if (auto p = pl.currentPath(); p.has_value()) {
                        am2.play(p.value());
                        if (pl.queueEmpty() && pl.repeatMode() != RepeatMode::One) {
                            if (auto peek = pl.peekNext(); peek.has_value())
                                am2.preloadNext(peek.value());
                        }
                    }
                    return;
                }
                if (pl.next().has_value())
                    if (auto p = pl.currentPath(); p.has_value()) am2.play(p.value());
            });

            // Product shape (XF item 1): the repeat-change callback is the ONE
            // sync point. The desync scenario leaves it UNWIRED - that models
            // the pre-item-1 startup this campaign abolished, kept as an
            // engine-robustness case (playlist-side guards must hold even with
            // the audio flag stale).
            if (!desync)
                pl.setRepeatChanged([&](RepeatMode m) {
                    am2.setRepeatOne(m == RepeatMode::One);
                    if (m == RepeatMode::One) am2.clearNext();
                });
            if (press_at_remaining < 0.0) {
                pl.setRepeat(RepeatMode::One);     // config restore
            } else {
                pl.setRepeat(RepeatMode::All);     // in-session: starts on All
            }

            if (!am2.play(PS)) { std::printf("P3 %s: SKIP (no device)\n", name); return; }
            // UIManager::maybePreloadNext, verbatim guard shape:
            if (pl.repeatMode() != RepeatMode::One && pl.queueEmpty())
                if (auto peek = pl.peekNext(); peek.has_value())
                    am2.preloadNext(peek.value());

            bool pressed = false;
            DWORD press_tick = 0;
            // max_total: whole run. after_press: from the press instant - includes
            // ~46ms of PRE-press ring history, so it approximates the fade gain at
            // the press. after_settle: from press+150ms, ring fully refreshed -
            // ONLY genuinely post-press emission. after_settle > 0 would mean the
            // abort failed and the fade kept sounding: a real product defect.
            float max_after_press = 0.0f, max_after_settle = 0.0f, max_total = 0.0f;
            int   swaps_to_loud = 0;
            DWORD t0 = GetTickCount();
            while ((long)(GetTickCount() - t0) < 7000 && loops.load() < 3) {
                am2.pollEvents();
                double rem = am2.durationSec() - am2.positionSec();
                if (!pressed && press_at_remaining >= 0.0 &&
                    am2.durationSec() > 0.0 && rem <= press_at_remaining) {
                    // The r handler, current shape: a bare mode change - the
                    // repeat-change callback pushes the flag and clears the arm.
                    pl.setRepeat(RepeatMode::One);
                    pressed = true;
                    press_tick = GetTickCount();
                }
                float pk = vizPeak(am2);
                if (pk > max_total) max_total = pk;
                if (pressed && pk > max_after_press) max_after_press = pk;
                if (pressed && (long)(GetTickCount() - press_tick) > 150 &&
                    pk > max_after_settle) max_after_settle = pk;
                if (am2.currentTrack().path == PL) ++swaps_to_loud;
                Sleep(2);
            }
            std::printf("P3 %-22s loops=%d  max_viz=%.4f  after_press=%.4f  "
                        "after_settle=%.4f  loud_was_current=%s  final=%s\n",
                        name, loops.load(), max_total, max_after_press, max_after_settle,
                        swaps_to_loud ? "YES" : "no",
                        am2.currentTrack().path == PS ? "A" :
                        am2.currentTrack().path == PL ? "B" : "?");

            // Assertions on the settled facts (all hold on the current tree):
            // the loop must converge on A and never adopt the neighbour...
            CHECK(loops.load() >= 3);
            CHECK(am2.currentTrack().path == PS);
            // ...steady-state repeat-one - INCLUDING the shipping config-restore
            // desync - must be bleed-free (this is the case the P2 block missed)...
            if (press_at_remaining < 0.0) CHECK(max_total < 0.01f);
            // ...and after an in-session r press the abort must actually silence
            // the fade: zero post-settle emission. Pre-press fade audio is the
            // legitimate All-mode fade and is NOT asserted against.
            if (press_at_remaining >= 0.0) CHECK(max_after_settle < 0.01f);
            am2.stop();
        };

        run_scenario("restore-desync",  true,  -1.0);   // shipping startup path
        run_scenario("restore-synced",  false, -1.0);   // with the startup push
        run_scenario("press@0.5s",      false,  0.5);   // gate item 3, mid-fade
        run_scenario("press@0.1s",      false,  0.1);   // gate item 3, race window

        std::remove(PS.c_str()); std::remove(PL.c_str());
    }

    // ── P4 (C3): reconcile convergence - the armed decoder follows the resolver
    // within ONE reconcile call after every queue/playlist mutation ────────────
    // Drives NextArm.h's reconcile core against the REAL AudioManager and REAL
    // PlaylistManager - the same call UIManager makes each tick. This is the
    // harness finally able to represent the playing track and the playlist row
    // diverging (queue pop) and re-converging.
    {
        const std::string P4A = "xfp4_a.wav", P4B = "xfp4_b.wav", P4C = "xfp4_c.wav";
        CHECK(writeWav(P4A, 30.0, 440.0));   // long: no boundary during the test
        CHECK(writeWav(P4B, 5.0, 880.0));
        CHECK(writeWav(P4C, 5.0, 660.0));

        AudioManager am4;
        am4.setVolume(0.0f);
        am4.toggleMute();
        am4.crossfade_secs = 2.0f;

        PlaylistManager pl;
        pl.addTrack(P4A); pl.addTrack(P4B); pl.addTrack(P4C);
        pl.selectAt(0);

        std::string armed, failed;
        auto tick = [&]{ am4.pollEvents(); reconcileNextArm(am4, pl, armed, failed); };

        if (!am4.play(P4A)) { std::printf("P4: SKIP (no device)\n"); }
        else {
            // 1. steady state: successor armed in one call
            tick();
            CHECK(am4.hasNextArmed()); CHECK(armed == P4B);

            // 2. queueAdd overrides peekNext in one call
            PlaylistEntry qc; qc.path = P4C;
            pl.queueAdd(qc);
            tick();
            CHECK(am4.hasNextArmed()); CHECK(armed == P4C);

            // 3. stacking a second queue entry does NOT re-arm (FIFO head rule)
            PlaylistEntry qb; qb.path = P4B;
            pl.queueAdd(qb);
            const std::string before = armed;
            tick();
            CHECK(armed == before && armed == P4C);

            // 4. queueClear falls back to the playlist successor
            pl.queueClear();
            tick();
            CHECK(am4.hasNextArmed()); CHECK(armed == P4B);

            // 5. repeat-one clears the arm entirely
            pl.setRepeat(RepeatMode::One);
            tick();
            CHECK(!am4.hasNextArmed()); CHECK(armed.empty());

            // 6. leaving repeat-one re-arms
            pl.setRepeat(RepeatMode::Off);
            tick();
            CHECK(am4.hasNextArmed()); CHECK(armed == P4B);

            // 7. a stream queue head arms NOTHING (hard cut preserved)
            PlaylistEntry qs; qs.path = "https://example.com/live.m3u8";
            pl.queueAdd(qs);
            tick();
            CHECK(!am4.hasNextArmed());
            pl.queueClear();
            tick();
            CHECK(am4.hasNextArmed()); CHECK(armed == P4B);

            // 8. failure latch: an unopenable queue head does not arm, does not
            // wedge, and recovery is immediate once the queue clears
            PlaylistEntry qbad; qbad.path = "C:\\xr\\does-not-exist.wav";
            pl.queueAdd(qbad);
            tick();
            CHECK(!am4.hasNextArmed()); CHECK(failed == qbad.path);
            tick();                              // latched: no re-attempt storm
            CHECK(!am4.hasNextArmed());
            pl.queueClear();
            tick();
            CHECK(am4.hasNextArmed()); CHECK(armed == P4B); CHECK(failed.empty());

            // 9. shuffle toggle re-resolves (arm follows the shuffle successor)
            pl.setShuffle(true);
            tick();
            const std::string want = resolveNextPath(pl);
            if (!want.empty()) CHECK(armed == want);
            pl.setShuffle(false);

            // 10. removeAt of the armed successor re-resolves in one call
            pl.selectAt(0);
            tick();
            pl.removeAt(1);                      // drop P4B, successor becomes P4C
            tick();
            CHECK(armed == P4C);

            am4.stop();
        }
        std::remove(P4A.c_str()); std::remove(P4B.c_str()); std::remove(P4C.c_str());
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
