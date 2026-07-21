// RE-MOCT – entry point

#include <iostream>
#include <stdexcept>
#include <csignal>
#include <clocale>
#include <filesystem>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "UIManager.h"
#include "AudioManager.h"
#include "PlaylistManager.h"
#include "Config.h"
#include "StringUtils.h"
#ifdef _WIN32
#include "CDSource.h"
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
static void win32_console_init() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  m = 0;
    if (GetConsoleMode(h, &m))
        SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    // Silence TagLib warnings (TDAT frame deprecation etc) by redirecting stderr
    freopen("NUL", "w", stderr);
}
#else
static void win32_console_init() {}
#endif

static UIManager*    g_ui    = nullptr;
static AudioManager* g_audio = nullptr;

static void handle_sigsegv(int) {
    // Miniaudio decoder crashed on a corrupt MP3 frame.
    // Signal track end so the UI skips to next track.
    // We can't do much here safely — just set a flag and return.
    if (g_audio) g_audio->signalTrackEnd();
}

int main(int argc, char* argv[]) {
    win32_console_init();

    try {
        AudioManager    audio;
        PlaylistManager playlist;
        DigiConfig      config;

        // ── Load saved config ─────────────────────────────────────────────
        config.load();
        audio.setVolume(config.volume);
        playlist.setRepeat(static_cast<RepeatMode>(config.repeat_mode));
        playlist.setShuffle(config.shuffle);
        // Restore EQ
        audio.setEqEnabled(config.eq_enabled);
        for (int b = 0; b < AudioManager::EQ_BANDS; ++b)
            audio.setEqGain(b, config.eq_gains[b]);

        // Restore saved playlist
        for (const auto& path : config.playlist_paths)
            playlist.addTrack(path);
        // Re-label restored radio entries with their persisted friendly names.
        // addTrack derives a URL-based label ("RADIO: hls.m3u8"); the friendly
        // name lives in config's name map. This mirrors UIManager::stationLabel's
        // "RADIO: <name>" format so the playlist matches the [Radio] pane.
        for (std::size_t i = 0; i < playlist.size(); ++i) {
            const auto& e = playlist.at(i);
            if (e.path.rfind("http://", 0) == 0 || e.path.rfind("https://", 0) == 0) {
                std::string nm = config.radioStationName(e.path);
                if (!nm.empty())
                    playlist.setDisplayTitle(i, "RADIO: " + sanitizeForDisplay(nm));
            }
        }
        if (!config.playlist_paths.empty())
            playlist.selectAt(config.playlist_current);

        // ── Preload-only callback (after crossfade swap) ──────────────────
        audio.setPreloadNextCallback([&]() {
            // Only advance playlist index if no queue override is pending
            if (playlist.queueEmpty()) {
                playlist.next();
                if (playlist.repeatMode() == RepeatMode::One) return;
                if (auto peek = playlist.peekNext(); peek.has_value())
                    if (!isCDTrackPath(peek.value()) && !isStreamPath(peek.value()))
                        audio.preloadNext(peek.value());
            }
            // If queue has items, leave playlist index where it is —
            // on_track_end_ will consume queue and advance correctly
        });

        // ── Auto-advance callback ─────────────────────────────────────────
        audio.setTrackEndCallback([&]() {

            // Podcast episode finished: mark it played and STOP - no auto-advance
            // into the music queue. g_ui is set before ui.run() (this fires during it).
            if (g_ui && g_ui->onEpisodeTrackEnd()) return;

            // Helper: route a path to CD or file playback
            auto play_path = [&](const std::string& p) {
#ifdef _WIN32
                std::string drive; int track_num;
                if (parseCDPath(p, drive, track_num)) {
                    // Slice 3: auto-advance reopen (mid-playback) - fails silently by
                    // design on an empty tray (openCD no-ops, playCDTrack returns false,
                    // advance ends). An ejected disc here is already caught by the reader-
                    // thread detector -> UIManager playing-eject purge; see the queue-pop
                    // reopen below. reopenCDForAction (UIManager) isn't reachable from here.
                    if (!audio.cdMode()) audio.openCD(drive);
                    audio.playCDTrack(track_num);
                    return;
                }
                if (audio.cdMode()) audio.closeCD();
#endif
                audio.play(p);
                if (playlist.queueEmpty()) {
                    if (auto peek = playlist.peekNext(); peek.has_value())
                        if (!isCDTrackPath(peek.value()) && !isStreamPath(peek.value()))
                            audio.preloadNext(peek.value());
                }
            };

            // ── Queue priority ────────────────────────────────────────────
            while (!playlist.queueEmpty()) {
                auto entry = playlist.queuePop();
                if (!entry.has_value()) break;
                const std::string& qpath = entry->path;
                audio.clearNext();
#ifdef _WIN32
                std::string drive; int track_num;
                if (parseCDPath(qpath, drive, track_num)) {
                    // Slice 3: this auto-advance queue-pop is a second reopen shape
                    // (openCD on the !cdMode branch). It fails SILENTLY BY DESIGN on
                    // an empty tray (continue, no row-purge/toast) — unlike the UI
                    // paths' reopenCDForAction. That helper lives in UIManager and
                    // isn't reachable here, and it isn't needed: this runs mid-
                    // playback (a track just ended), so an ejected disc is already
                    // caught by the reader-thread detector (CDSource media_removed_
                    // -> UIManager's playing-eject purge). This path only skips the
                    // dead queue entry and advances.
                    if (audio.cdMode()) {
                        if (track_num > (int)audio.cdTracks().size()) continue;
                        audio.playCDTrack(track_num);
                    } else {
                        if (!audio.openCD(drive)) continue;
                        if (track_num > (int)audio.cdTracks().size()) { audio.closeCD(); continue; }
                        audio.playCDTrack(track_num);
                    }
                    return;
                }
                if (audio.cdMode()) audio.closeCD();
#endif
                play_path(qpath);
                return;
            }

            // ── Normal playlist advance ───────────────────────────────────
#ifdef _WIN32
            if (audio.cdMode()) {
                if (auto peek = playlist.peekNext(); peek.has_value()) {
                    if (isCDTrackPath(peek.value())) {
                        if (playlist.next().has_value()) {
                            if (auto p = playlist.currentPath(); p.has_value()) {
                                std::string drive; int track_num;
                                if (parseCDPath(p.value(), drive, track_num))
                                    audio.playCDTrack(track_num);
                            }
                        }
                        return;
                    }
                } else { return; }
            }
#endif
            if (playlist.repeatMode() == RepeatMode::One) {
                if (auto p = playlist.currentPath(); p.has_value())
                    play_path(p.value());
                return;
            }
            if (playlist.next().has_value()) {
                if (auto p = playlist.currentPath(); p.has_value())
                    play_path(p.value());
            }
        });

        // ── Command-line args ─────────────────────────────────────────────
        for (int i = 1; i < argc; ++i) {
            std::string arg(argv[i]);
            if (PlaylistManager::isSupportedAudio(arg))
                playlist.addTrack(arg);
            else
                playlist.addDirectory(arg);
        }

        // ── Preload next track ────────────────────────────────────────────
        if (playlist.repeatMode() != RepeatMode::One)
            if (auto peek = playlist.peekNext(); peek.has_value())
                if (!isCDTrackPath(peek.value()) && !isStreamPath(peek.value()))
                    audio.preloadNext(peek.value());

        // ── Start UI ──────────────────────────────────────────────────────
        UIManager ui(playlist, audio, config,
                     config.last_dir.empty() ? "" : config.last_dir);
        g_ui    = &ui;
        g_audio = &audio;
        std::signal(SIGSEGV, handle_sigsegv);

        // NOTE deliberately NO custom SIGWINCH handler: ncursesw installs its
        // own at initscr(), which is what turns a terminal resize into
        // KEY_RESIZE through getch() — the resize path UIManager already
        // handles (KEY_RESIZE → relayout). Installing ours DISPLACED that
        // handler, so ncurses never learned the new size and redraws happened
        // at the stale geometry (the Dos-found resize bug on the Debian VM).

        ui.run();

        // ── Save config on exit ───────────────────────────────────────────
        config.last_dir         = ui.currentDir();
        config.volume           = audio.volume();
        config.repeat_mode      = (int)playlist.repeatMode();
        config.shuffle          = playlist.shuffle();
        // toast_enabled is toggled in-place via UIManager's config reference, so
        // it is already current here — no copy needed.
        config.eq_enabled       = audio.eqEnabled();
        for (int b = 0; b < AudioManager::EQ_BANDS; ++b)
            config.eq_gains[b]  = audio.getEqGain(b);
        config.playlist_current = playlist.current();
        config.playlist_paths.clear();
        for (const auto& e : playlist.entries()) {
            if (isCDTrackPath(e.path)) continue;
            config.playlist_paths.push_back(e.path);
        }
        // If we exited during CD playback the saved index would be CD-relative.
        // Reset to 0 so next launch starts from beginning of file playlist.
        if (config.playlist_current >= config.playlist_paths.size())
            config.playlist_current = 0;
        config.save();

    } catch (const std::exception& ex) {
        endwin();
#ifdef _WIN32
        // stderr was redirected to NUL at startup (win32_console_init) to silence
        // TagLib's frame-deprecation chatter. Reopen the console here so this fatal
        // message is actually visible instead of vanishing into NUL.
        freopen("CONOUT$", "w", stderr);
#endif
        std::cerr << "[RE-MOCT] fatal: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
