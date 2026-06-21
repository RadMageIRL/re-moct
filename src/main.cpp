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

#ifndef _WIN32
static void handle_sigwinch(int) { if (g_ui) g_ui->requestRedraw(); }
#endif

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
        if (!config.playlist_paths.empty())
            playlist.selectAt(config.playlist_current);

        // ── Preload-only callback (after crossfade swap) ──────────────────
        audio.setPreloadNextCallback([&]() {
            // Only advance playlist index if no queue override is pending
            if (playlist.queueEmpty()) {
                playlist.next();
                if (playlist.repeatMode() == RepeatMode::One) return;
                if (auto peek = playlist.peekNext(); peek.has_value())
                    if (!isCDTrackPath(peek.value()))
                        audio.preloadNext(peek.value());
            }
            // If queue has items, leave playlist index where it is —
            // on_track_end_ will consume queue and advance correctly
        });

        // ── Auto-advance callback ─────────────────────────────────────────
        audio.setTrackEndCallback([&]() {

            // Helper: route a path to CD or file playback
            auto play_path = [&](const std::string& p) {
#ifdef _WIN32
                std::string drive; int track_num;
                if (parseCDPath(p, drive, track_num)) {
                    if (!audio.cdMode()) audio.openCD(drive);
                    audio.playCDTrack(track_num);
                    return;
                }
                if (audio.cdMode()) audio.closeCD();
#endif
                audio.play(p);
                if (playlist.queueEmpty()) {
                    if (auto peek = playlist.peekNext(); peek.has_value())
                        if (!isCDTrackPath(peek.value()))
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
                audio.preloadNext(peek.value());

        // ── Start UI ──────────────────────────────────────────────────────
        UIManager ui(playlist, audio, config,
                     config.last_dir.empty() ? "" : config.last_dir);
        g_ui    = &ui;
        g_audio = &audio;
        std::signal(SIGSEGV, handle_sigsegv);

#ifndef _WIN32
        std::signal(SIGWINCH, handle_sigwinch);
#endif

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
