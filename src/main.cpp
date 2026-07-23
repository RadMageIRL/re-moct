// RE-MOCT – entry point

#include <iostream>
#include <stdexcept>
#include <csignal>
#include <clocale>
#include <filesystem>

#ifdef _WIN32
#  include <windows.h>
#  include <shlobj.h>      // IShellLinkW, CLSID_ShellLink, IPersistFile
#  include <shobjidl.h>    // SetCurrentProcessExplicitAppUserModelID
#  include <propsys.h>     // IPropertyStore
#  include <objbase.h>     // CoInitializeEx, PropVariant*
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
// THE AppUserModelID string - used for BOTH the shortcut stamp and the process call.
// If the two ever differ, Windows duplicates the taskbar icon, so it is defined exactly
// ONCE. NOTE: the .lnk BASENAME ("RE-MOCT") is also load-bearing - the media card shows
// the display name of the shortcut whose AUMID matches, so a future rename of the file
// would silently change the card name. Keep the constant and the basename in sync.
static const wchar_t* const kRemoctAumid = L"RE-MOCT";

// PKEY_AppUserModel_ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, PID 5. Defined here so
// we don't depend on propkey.h's INITGUID plumbing under MinGW.
static const PROPERTYKEY kPkeyAumid = {
    { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };

// Best-effort: ensure a Start-menu shortcut to THIS exe exists and carries the AUMID, so
// the Windows media card resolves "RE-MOCT" instead of "Unknown app". Check-and-heal
// every launch: create if missing, repoint if the exe moved, else no-op. Cosmetic only -
// every step is HRESULT-guarded and NOTHING here can block or crash startup; on any
// failure (read-only APPDATA, locked .lnk, COM failure) the card simply stays "Unknown
// app", which is the pre-slice behaviour. UI launch never depends on this.
static void ensureStartMenuShortcut() noexcept {
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return;

    wchar_t appdata[MAX_PATH];
    DWORD an = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (an == 0 || an >= MAX_PATH) return;

    wchar_t lnk[MAX_PATH];
    if (_snwprintf(lnk, MAX_PATH,
                   L"%ls\\Microsoft\\Windows\\Start Menu\\Programs\\RE-MOCT.lnk", appdata) < 0)
        return;

    HRESULT hrco = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrco) && hrco != RPC_E_CHANGED_MODE) return;   // COM unavailable -> bail
    const bool weInit = SUCCEEDED(hrco);                      // only uninit what we init'd

    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, (void**)&link))) {
        bool need_write = true;

        // Existing .lnk: load it and decide whether it is already correct (no-op case).
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            if (SUCCEEDED(pf->Load(lnk, STGM_READ))) {
                wchar_t cur[MAX_PATH] = L"";
                link->GetPath(cur, MAX_PATH, nullptr, SLGP_RAWPATH);
                bool target_ok = (_wcsicmp(cur, exe) == 0);
                bool aumid_ok  = false;
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, (void**)&ps))) {
                    PROPVARIANT r; PropVariantInit(&r);
                    if (SUCCEEDED(ps->GetValue(kPkeyAumid, &r)) && r.vt == VT_LPWSTR)
                        aumid_ok = (wcscmp(r.pwszVal, kRemoctAumid) == 0);
                    PropVariantClear(&r);
                    ps->Release();
                }
                if (target_ok && aumid_ok) need_write = false;
            }
            pf->Release();
        }

        if (need_write) {   // create, or repoint/re-stamp a stale one
            link->SetPath(exe);
            wchar_t dir[MAX_PATH]; wcscpy(dir, exe);
            if (wchar_t* slash = wcsrchr(dir, L'\\')) { *slash = 0; link->SetWorkingDirectory(dir); }
            link->SetDescription(L"RE-MOCT - Music On Console Terminal");

            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, (void**)&ps))) {
                PROPVARIANT pv{}; pv.vt = VT_LPWSTR;
                size_t n = wcslen(kRemoctAumid) + 1;
                pv.pwszVal = (LPWSTR)CoTaskMemAlloc(n * sizeof(wchar_t));
                if (pv.pwszVal) {
                    wcscpy(pv.pwszVal, kRemoctAumid);
                    if (SUCCEEDED(ps->SetValue(kPkeyAumid, pv))) ps->Commit();
                }
                PropVariantClear(&pv);   // frees pwszVal via CoTaskMemFree (matches CoTaskMemAlloc)
                ps->Release();
            }
            IPersistFile* pfw = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pfw))) {
                pfw->Save(lnk, TRUE);
                pfw->Release();
            }
        }
        link->Release();
    }

    if (weInit) CoUninitialize();
}

static void win32_console_init() {
    // Media-card identity: heal the Start-menu shortcut, then set the SAME AUMID on the
    // process (before initscr / any window / SMTC). Both best-effort; neither gates UI.
    ensureStartMenuShortcut();
    SetCurrentProcessExplicitAppUserModelID(kRemoctAumid);
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
                // Repeat-one guard, matching the startup preload and
                // UIManager::maybePreloadNext. Without it, draining a queued track
                // under repeat-one arms peekNext(), which in repeat-one is the
                // PLAYLIST current track - a foreign decoder against the queued
                // track now playing.
                if (playlist.queueEmpty() && playlist.repeatMode() != RepeatMode::One) {
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
