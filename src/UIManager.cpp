// UIManager.cpp

// windows.h must precede the curses seam (PDCurses/windows.h symbol-order rule);
// it is also used directly by the Windows-only code below (ShellExecute etc.).
#ifdef _WIN32
#  include <windows.h>
#endif
#include "CursesSeam.h"

#include "UIManager.h"
#include "CoverArt.h"
#include "PortUtil.h"   // port::exeDir — locate a bundled wingui font beside the exe
#ifdef _WIN32
#include <shellapi.h>   // ShellExecuteA for Last.fm browser auth
#else
#include <termios.h>    // IXON off so ^Q/^S reach curses (ctor block below)
#include <unistd.h>
#endif
#include "StringUtils.h"
#include "EncoderQuality.h"
#include "AudioManager.h"
#include "PlaylistManager.h"
#include "Mp4Chapters.h"
#include "Config.h"
#include "LrcData.h"
#include "Toast.h"
// IHeartDeepLog moved into the streaming plugin (slice c) — the host no longer
// includes it. Ctrl+A toggles it across the ABI via audio_.setDeepLog().

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <string>
#include <cmath>
#include <array>
#include <numeric>
#include <cwchar>
#include <clocale>
#include <cstdint>
#include <climits>   // INT_MAX (help-pane End: draw-time clamp pins it to max_scroll)
#include <ctime>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include "Log.h"
#include "Version.h"
// Version tag shown in the status line and About panel. The version is
// single-sourced in Version.h (REMOCT_VERSION); only the honest per-platform
// suffix differs here.
#ifdef _WIN32
static const char* const kVersionTag = "v" REMOCT_VERSION "-win";
#else
static const char* const kVersionTag = "v" REMOCT_VERSION "-linux";
#endif

// [THEME:name] cwd-line tag lifetime, in ~80ms loop ticks (timeout(80)):
// bold up to the fade point, dimmed after it, removed at the end (~10s).
static constexpr int kThemeTagFadeTick = 106;   // ~8.5s: bold -> dim
static constexpr int kThemeTagGoneTick = 125;   // ~10s : removed

// Awesome-mode full-width Spectrum strip: height incl. frame, and the minimum
// pane height below which the strip is dropped (small-terminal guard).
static constexpr int kVizStripRows = 7;         // 2 frame + ~5 bar rows
static constexpr int kMinPaneRows  = 3;         // panes need at least this many
// Minimum spectrum bar height (in cells) so a low-activity band still shows a
// small nub instead of vanishing. Classic's tall pane uses ~1/3 cell; the short
// Awesome strip uses a taller floor (it only has ~5 rows to work with).
static constexpr float kVizFloorCells      = 0.35f;   // Classic overlay
static constexpr float kVizStripFloorCells = 1.19f;   // Awesome strip (~24% of ~5 rows; +15% floor)

static void sclog(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log::write("scrob", buf);
}
#include <cstdio>
#include <chrono>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <taglib/tvariant.h>      // complexProperties("PICTURE") -> embedded cover
#include <taglib/tbytevector.h>

namespace fs = std::filesystem;

#ifdef PDCURSES
#include <dwmapi.h>   // DwmSetWindowAttribute — OS-matching (dark) title bar
// wingui global (pdcdisp.c), UNICODE build => wchar_t[128]. Declared at global
// scope: an extern "C" inside an anonymous namespace is a linkage contradiction.
extern "C" TCHAR PDC_font_name[];
// wingui (pdcscrn.c): registers a callback invoked on every WM_SIZE, including
// inside Windows' modal resize-drag loop where our getch() loop is blocked.
extern "C" void PDC_set_window_resized_callback(void (*)(void));
extern "C" HWND PDC_hWnd;   // wingui's GDI window handle (pdcscrn.c)
extern "C" int PDC_cxChar, PDC_cyChar;   // wingui glyph cell size in px (pdcscrn.c)
extern "C" int PDC_skip_size_snap;       // wingui: suppress the WM_SIZE cell-snap (pdcscrn.c)

namespace {
// Trampoline for the C resize callback -> the live UIManager (single UI instance).
UIManager* g_wingui_ui = nullptr;
void winguiResizeTrampoline() { if (g_wingui_ui) g_wingui_ui->onWinguiLiveResize(); }

// Make the wingui GDI window's title bar / border follow the OS light/dark theme
// (Windows 10 20H1+). Reads HKCU AppsUseLightTheme (0 = dark) and applies
// DWMWA_USE_IMMERSIVE_DARK_MODE - attribute 20 on current builds, 19 on older
// Win10; setting both is harmless where one is ignored. Without this the window
// keeps the default light chrome even when the OS is in dark mode.
void applyOsTitleBarTheme() {
    if (!PDC_hWnd) return;
    BOOL dark = TRUE;   // default to dark; flip to light only if the OS says so
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD v = 1, sz = sizeof(v), ty = 0;
        if (RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, &ty,
                             reinterpret_cast<LPBYTE>(&v), &sz) == ERROR_SUCCESS)
            dark = (v == 0) ? TRUE : FALSE;
        RegCloseKey(hk);
    }
    DwmSetWindowAttribute(PDC_hWnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(PDC_hWnd, 19, &dark, sizeof(dark));
}

// PDCursesMod wingui owns its GDI font (unlike a terminal, where the font is the
// terminal's). Point it at a glyph-complete face so box-drawing corners (rounded
// ╭╮╰╯), viz blocks ▁▂▇█ and Nerd icons render instead of tofu. MUST run BEFORE
// initscr() - wingui measures the font while opening the screen. Any .ttf/.otf in
// <exeDir>/fonts/ is added PROCESS-PRIVATE (FR_PRIVATE), so a bundled font need
// not be installed system-wide ("runs anywhere"). The face is config-overridable
// (config_.wingui_font); default is a Mono Nerd Font.
void initWinguiFont(const std::string& configured_face) {
    // PDCursesMod wingui persists the last font+window-size to
    // HKCU\SOFTWARE\PDCurses\<exeBaseName> and RESTORES it inside initscr(),
    // which overrides the face we set here (e.g. a stale "Courier New" saved by a
    // pre-font-fix run stays sticky forever). Clear our value so OUR choice wins
    // every launch - the wingui_font config key is the persistence layer, not the
    // registry. Cost: the wingui window's size/position is not remembered across
    // launches (a deterministic default is fine; RE-MOCT reflows).
    {
        wchar_t mod[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, mod, MAX_PATH)) {
            std::wstring name = mod;                 // basename sans extension ==
            size_t slash = name.find_last_of(L"\\/"); // PDCurses' get_app_name()
            if (slash != std::wstring::npos) name.erase(0, slash + 1);
            size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos) name.erase(dot);
            RegDeleteKeyValueW(HKEY_CURRENT_USER, L"SOFTWARE\\PDCurses", name.c_str());
        }
    }
    std::error_code ec;
    fs::path fdir = fs::path(port::exeDir()) / "fonts";
    if (fs::is_directory(fdir, ec)) {
        for (const auto& e : fs::directory_iterator(fdir, ec)) {
            const auto ext = e.path().extension();
            if (ext == ".ttf" || ext == ".otf" || ext == ".TTF" || ext == ".OTF")
                AddFontResourceExW(e.path().wstring().c_str(), FR_PRIVATE, nullptr);
        }
    }
    std::wstring face;
    if (!configured_face.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, configured_face.c_str(), -1, nullptr, 0);
        if (n > 1) { face.resize(n - 1);
            MultiByteToWideChar(CP_UTF8, 0, configured_face.c_str(), -1, face.data(), n); }
    }
    if (face.empty()) face = L"JetBrainsMono NFM";   // Mono Nerd Font: single-cell glyphs
    if (face.size() < 128) wcscpy(PDC_font_name, face.c_str());
}
} // namespace
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────
// ── rip-format-select helpers ────────────────────────────────────────────────
// config_.rip_formats ("flac,mp3") -> ordered selection. Case-insensitive
// label match against kRipFormats, unknown tokens ignored (a future format
// name degrades gracefully on an older build), duplicates dropped, empty
// result -> the full default set (absent config = pre-slice behavior).
static std::vector<RipFormat> parseRipFormats(const std::string& s) {
    std::vector<RipFormat> sel;
    std::string tok;
    auto flush = [&] {
        if (tok.empty()) return;
        for (const auto& r : kRipFormats) {
            if (tok.size() == strlen(r.label) &&
                std::equal(tok.begin(), tok.end(), r.label,
                           [](char a, char b) { return std::toupper((unsigned char)a) == b; }) &&
                std::find(sel.begin(), sel.end(), r.id) == sel.end())
                sel.push_back(r.id);
        }
        tok.clear();
    };
    for (char c : s) {
        if (c == ',') flush();
        else if (!std::isspace((unsigned char)c)) tok += c;
    }
    flush();
    if (sel.empty())
        for (const auto& r : kRipFormats) sel.push_back(r.id);
    return sel;
}

// config_.mp3 "V0".."V9" -> LAME VBR_q int; anything unexpected -> 0 (V0,
// the pre-config literal). Config::load already validates, this is defense.
static int parseMp3VbrQ(const std::string& s) {
    if (s.size() == 2 && (s[0] == 'V' || s[0] == 'v') && s[1] >= '0' && s[1] <= '9')
        return s[1] - '0';
    return 0;
}

UIManager::UIManager(PlaylistManager& playlist, AudioManager& audio,
                     DigiConfig& config, const std::string& initial_dir,
                     core::INotify* notify, core::IMediaControl* media)
    : notify_(notify ? notify : &core::notifier()),
      media_(media),   // default resolved AFTER initscr (SMTC needs a live HWND)
      playlist_(playlist), audio_(audio), config_(config)
{
    // Seed the session rip-format selection from the config default (config
    // is load-once; the modal toggles rip_sel_ and never writes it back).
    rip_sel_ = parseRipFormats(config_.rip_formats);
    // Stream-record R2: seed the [Rec] panel settings the same way (config is
    // load-once; the panel mutates session state and never writes back).
    rec_fmt_      = config_.rec_format == "mp3" ? RipFormat::Mp3
                  : config_.rec_format == "m4a" ? RipFormat::M4a : RipFormat::Opus;
    rec_copy_     = (config_.rec_format == "copy");   // 4th radio (slice B); falls
                                                      // back live if the plugin lacks it
    rec_split_on_ = config_.rec_split;
    rec_dir_      = config_.rec_dir;
    rec_offset_ms_ = std::clamp(config_.split_offset_ms, 0, 5000);  // lead reserved: negative -> 0
    rec_ads_discard_ = (config_.rec_ads == "discard");
#ifdef PDCURSES
    // wingui measures its font during initscr(), so choose the face FIRST.
    initWinguiFont(config_.wingui_font);
#endif
    if (!initscr()) throw std::runtime_error("initscr() failed");
#ifdef PDCURSES
    // PDCursesMod wingui (Option C): the GDI port opens its own window - give it
    // the RE-MOCT caption. Colour / transparent-bg parity (PDC_ORIGINAL_COLORS
    // vs use_default_colors) is tuned on the Task-4 visual re-probe, not here.
    PDC_set_title("RE-MOCT");
    applyOsTitleBarTheme();   // dark/light window chrome to match the OS
    // Hide wingui's "Font" menu bar (a light Win32 menu that ignores the dark
    // title bar; the font is config-driven now) via wingui's OWN toggle rather
    // than a raw SetMenu: WM_TOGGLE_MENU keeps the curses grid unchanged and just
    // shrinks the window to match, so there's no size desync (raw SetMenu grew the
    // client area and mis-sized/relocated the window). SendMessage is synchronous;
    // menu_shown starts at 1 (initWinguiFont cleared the registry), so one toggle
    // hides it. WM_USER+4 is wingui's private WM_TOGGLE_MENU command id, dispatched
    // under WM_COMMAND. The grid doesn't change, so this can't re-enter our
    // resize callback (it only fires on a row/col change).
    if (PDC_hWnd) SendMessageW(PDC_hWnd, WM_COMMAND, WM_USER + 4, 0);
    // Restore the last window size (persisted in OUR config since we wipe the
    // PDCurses registry to keep our font - see initWinguiFont). Do it BEFORE
    // registering the resize callback so this programmatic resize_term can't
    // re-enter it while the pane windows don't exist yet. getmaxyx below then
    // reflects the restored grid, so createWindows lays out at the right size.
    if (config_.wingui_cols >= 40 && config_.wingui_rows >= 9)
        resize_term(config_.wingui_rows, config_.wingui_cols);
    // Repaint live during the modal resize-drag (see onWinguiLiveResize).
    g_wingui_ui = this;
    PDC_set_window_resized_callback(&winguiResizeTrampoline);
#endif
    setlocale(LC_ALL, "");
    cbreak();
    noecho();
#ifndef _WIN32
    // Linux ttys ship with XON/XOFF flow control (IXON): the driver consumes
    // ^Q (XON) and ^S (XOFF) before curses ever sees them — so Ctrl+Q (quit,
    // key 17) never arrived and ^S would freeze output. cbreak() doesn't turn
    // this off (raw() would, but it also takes ISIG — too big a hammer).
    // Windows consoles have no tty flow control, which is why the baseline
    // never needed this. (Slice-1 follow-up, Dos-found on the Debian VM.)
    // NOTE: this is the ONLY control char the Linux tty steals in cbreak
    // mode — a minimal-curses key probe confirmed ^B/^G/^U/^N/^T (2/7/21/
    // 14/20) all reach getch() with just this. The slice-2 "dead keys" were
    // an #ifdef _WIN32 span in handleInput's key switch, not the terminal.
    {
        struct termios tio{};
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {
            tio.c_iflag &= ~static_cast<tcflag_t>(IXON);
            tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }
    }
#endif
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(80);
#ifndef PDCURSES
    // ncurses waits ESCDELAY ms (default 1000!) after a lone ESC to disambiguate
    // it from an escape sequence (arrows / function keys are ESC-prefixed). That
    // made every Esc-to-close-a-pane feel like a ~1s freeze on Linux. keypad(TRUE)
    // already decodes the real sequences, so a short window is plenty; 25ms is
    // imperceptible yet still catches a multibyte sequence on a slow link. PDCurses
    // reads discrete key events (no ESC ambiguity), so this is ncurses-only.
    set_escdelay(25);
#endif
    initColours();
    getmaxyx(stdscr, screen_rows_, screen_cols_);
    createWindows();

    // Use saved dir if valid, else cwd
    namespace fs = std::filesystem;
    if (!initial_dir.empty() && fs::exists(initial_dir))
        current_dir_ = initial_dir;
    else
        current_dir_ = fs::current_path().string();

    refreshDir();
    audio_.setPreferDigital(config_.prefer_digital_stream);   // apply saved stream-mode pref
    audio_.setProbeMinted(config_.iheart_probe_minted);       // apply saved identity A/B arm (probe)

    // OS media control (osmedia-seam): resolve the production default HERE, at the
    // END of the ctor - after initscr() so the SMTC impl has the live wingui HWND
    // (PDC_hWnd), which does not exist until the screen is open. Tests inject a
    // fake, so mediaControl() (SMTC/MPRIS) is never constructed under test. Then
    // register the inbound sinks + handler; outbound publish rides updateScrobbler
    // and the loop pumps+drains.
    if (!media_) media_ = &core::mediaControl();
    wireMediaControl();

    running_ = true;
}

// All toast call sites land here (member hides the 4-arg Toast.h adapter);
// content mapping stays in Toast.h, delivery behind the injected notifier.
void UIManager::showTrackToast(const std::string& title, const std::string& artist,
                               const std::string& album) {
    ::showTrackToast(title, artist, album, *notify_);
#ifndef _WIN32
    // Cmdline echo (KEPT past slice 5 — see UIManager.h): a real notify-send
    // toast now lands on a Linux desktop with a daemon, but headless/no-daemon
    // Linux (WSL2, CI, SSH) renders nothing — so mirror the adapter's title
    // mapping (artist prepends) into the cmdline bar as the always-visible
    // graceful-degradation surface. Sanitized: the bar draws via the narrow API
    // and metadata can carry non-ASCII.
    status_msg_ = sanitizeForDisplay(artist.empty() ? title
                                                    : artist + " - " + title);
    status_msg_ticks_ = 0;
    redraw_needed_.store(true);
#endif
}

UIManager::~UIManager() {
    if (media_) media_->clear();   // drop the OS now-playing surface on exit
    flushBookProgress();       // persist resume position if quitting mid-book
    running_ = false;
    lf_poll_active_.store(false);
    if (lf_poll_thread_.joinable()) lf_poll_thread_.join();
    lb_validate_active_.store(false);
    if (lb_validate_thread_.joinable()) lb_validate_thread_.join();
    if (discord_art_thread_.joinable()) discord_art_thread_.join();
    if (radio_art_thread_.joinable())   radio_art_thread_.join();
    if (info_art_thread_.joinable())    info_art_thread_.join();
    if (mb_search_win_) { delwin(mb_search_win_); mb_search_win_ = nullptr; }
#ifdef PDCURSES
    // Remember the wingui window size for next launch (screen_rows_/cols_ track the
    // live size via resizeWindows). Only rewrite config when it actually changed.
    if (screen_cols_ >= 40 && screen_rows_ >= 9 &&
        (screen_cols_ != config_.wingui_cols || screen_rows_ != config_.wingui_rows)) {
        config_.wingui_cols = screen_cols_;
        config_.wingui_rows = screen_rows_;
        config_.save();
    }
#endif
#ifdef _WIN32
    // MBLookup/CDRipper cancel stay gated: CD is slice 6.
    mb_lookup_.cancel();
    cd_ripper_.cancel();
#endif
    destroyWindows();
    endwin();
}

void UIManager::onWinguiLiveResize() {
    // Fires from wingui's WM_SIZE handler during the modal resize-drag, on the UI
    // thread while getch() is blocked in the drag loop. Rebuild + repaint so the
    // window shows live content instead of blanking until the drag is released.
    // Reentrancy guard: a nested WM_SIZE (should not happen mid-drag, but be safe)
    // must not recurse into a second rebuild. Same-thread, so a plain bool suffices.
    static bool in_live_resize = false;
    if (in_live_resize) return;
    in_live_resize = true;
    resizeWindows();
    in_live_resize = false;
}

#ifdef PDCURSES
// Alt+Enter: toggle a borderless window that fills the current monitor, and back.
// Stripping WS_OVERLAPPEDWINDOW + covering the monitor is the standard Raymond-Chen
// borderless-fullscreen; wingui's WM_SIZE then reflows the curses grid through the
// resize callback, exactly like a drag. State is a single-window latch, so file-
// local statics are enough.
void UIManager::toggleWinguiFullscreen() {
    if (!PDC_hWnd) return;
    static bool             fs = false;
    static LONG_PTR         saved_style = 0;
    static LONG_PTR         saved_exstyle = 0;
    static WINDOWPLACEMENT  saved_wp = { sizeof(WINDOWPLACEMENT) };
    HWND h = PDC_hWnd;
    if (!fs) {
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (!GetWindowPlacement(h, &saved_wp) ||
            !GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        saved_style   = GetWindowLongPtr(h, GWL_STYLE);
        saved_exstyle = GetWindowLongPtr(h, GWL_EXSTYLE);
        // Use the EXACT monitor rect - not rounded to a cell multiple. wingui would
        // normally snap a non-multiple window down to a multiple (shrinking it under
        // the monitor: taskbar re-exposed / an edge clipped, worst on 4K where the
        // remainder is biggest), so suppress that snap for the duration. The curses
        // grid just uses the whole cells that fit; the <1-cell strip at the far edge
        // is harmless window background, and NOTHING is clipped off-screen.
        PDC_skip_size_snap = 1;
        // Strip BOTH the frame (WS_OVERLAPPEDWINDOW) AND the extended client edge that
        // wingui creates the window with (WS_EX_CLIENTEDGE). Leaving the client edge on
        // kept the client area inset from the window, so the window never read as a true
        // fullscreen app - which is what makes Windows auto-hide the taskbar.
        SetWindowLongPtr(h, GWL_STYLE,   saved_style   & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowLongPtr(h, GWL_EXSTYLE, saved_exstyle & ~(LONG_PTR)WS_EX_CLIENTEDGE);
        SetWindowPos(h, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        BringWindowToTop(h);
        SetForegroundWindow(h);   // ensure we're the active fullscreen window over the taskbar
        fs = true;
    } else {
        PDC_skip_size_snap = 0;   // restore stock drag-resize snapping
        SetWindowLongPtr(h, GWL_STYLE,   saved_style);
        SetWindowLongPtr(h, GWL_EXSTYLE, saved_exstyle);
        // Drop topmost, then restore the saved framed placement.
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetWindowPlacement(h, &saved_wp);
        fs = false;
    }
    // The style change can drop the dark chrome; re-apply so windowed mode matches
    // the OS theme again (harmless while borderless).
    applyOsTitleBarTheme();
}
#endif

void UIManager::resizeWindows() {
#if defined(REMOCT_PDCURSES)
    // PDCursesMod wingui draws its OWN GDI window - there is NO console, so the
    // GetConsoleWindow/CONOUT$ measuring used by the ncursesw-on-console build
    // (below) reports the wrong geometry here and would drive resize_term to a
    // size that mismatches the actual window: no reflow, plus an intermittent
    // crash from drawing into a stale-sized cell buffer. wingui has already
    // stored the new window size (PDC_n_rows/cols) and queued KEY_RESIZE;
    // resize_term(0, 0) adopts that pending size and refreshes LINES/COLS/stdscr
    // in one call (the canonical PDCurses idiom - its own getch.c does the same).
    resize_term(0, 0);
    getmaxyx(stdscr, screen_rows_, screen_cols_);
#elif defined(_WIN32)
    {
        int new_cols = screen_cols_, new_rows = screen_rows_;
        // Try GetConsoleWindow pixel method first
        HWND hwnd = GetConsoleWindow();
        if (hwnd) {
            RECT rect {};
            GetClientRect(hwnd, &rect);
            CONSOLE_FONT_INFOEX fi {}; fi.cbSize = sizeof(fi);
            HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
            if (GetCurrentConsoleFontEx(h, FALSE, &fi) &&
                fi.dwFontSize.X > 0 && fi.dwFontSize.Y > 0 &&
                rect.right > 0 && rect.bottom > 0) {
                new_cols = (rect.right - rect.left) / fi.dwFontSize.X;
                new_rows = (rect.bottom - rect.top) / fi.dwFontSize.Y;
            }
        }
        // Fallback: CONOUT$
        if (new_cols == screen_cols_ && new_rows == screen_rows_) {
            HANDLE hc = CreateFileA("CONOUT$", GENERIC_READ|GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hc != INVALID_HANDLE_VALUE) {
                CONSOLE_SCREEN_BUFFER_INFO csbi {};
                if (GetConsoleScreenBufferInfo(hc, &csbi)) {
                    new_cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
                    new_rows = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
                }
                CloseHandle(hc);
            }
        }
        if (new_cols > 0) screen_cols_ = new_cols;
        if (new_rows > 0) screen_rows_ = new_rows;
    }
    resize_term(screen_rows_, screen_cols_);
#else
    getmaxyx(stdscr, screen_rows_, screen_cols_);
    resize_term(screen_rows_, screen_cols_);
#endif
    destroyWindows();

    // Blank stdscr so stale content outside the new bounds is wiped; clearok
    // forces a full (non-diffed) repaint on the next doupdate.
    clearok(stdscr, TRUE);
    werase(stdscr);
#if !defined(REMOCT_PDCURSES)
    // Console/ncurses: paint + FLUSH a blank frame first (belt-and-suspenders on
    // shrink). On wingui this intermediate doupdate() shows a blank frame on
    // every WM_SIZE during a live resize-drag = visible flicker; there we skip it
    // and let the single composed doupdate() below repaint cleanly (wingui
    // double-buffers its WM_PAINT, so one full repaint per tick is flicker-free).
    for (int r = 0; r < screen_rows_; ++r)
        for (int c = 0; c < screen_cols_; ++c)
            mvaddch(r, c, ' ');
    wnoutrefresh(stdscr);
    doupdate();
#endif

    if (screen_rows_ < 9 || screen_cols_ < 40) {
        wnoutrefresh(stdscr);
        doupdate();
        redraw_needed_.store(true);
        return;
    }

    createWindows();
    redraw_needed_.store(false);
    drawAll();
    doupdate();  // extra flush to guarantee frame hits the terminal
}

#ifdef REMOCT_PROBE
// Slice-1 Probe A1: dump the curses colour budget once colour init completes, on
// BOTH the Awesome and Classic paths. Temporary; removed when the slice closes.
static void probeLogCaps(const char* mode) {
#ifdef _WIN32
    const char* term = "(wingui/n-a)";
#else
    const char* term = std::getenv("TERM") ? std::getenv("TERM") : "(null)";
#endif
    Log::writef("probe",
        "A1 caps mode=%s COLORS=%d COLOR_PAIRS=%d has_colors=%d can_change_color=%d TERM=%s",
        mode, COLORS, COLOR_PAIRS, (int)has_colors(), (int)can_change_color(), term);
}
#endif

void UIManager::initColours() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();

    // start_color()/use_default_colors() reset the whole palette, art slots 64+ and
    // pairs 20+ included. Any pair indices we handed out are now stale, so disown the
    // art pair table; the next blit reallocates. One line here covers every caller
    // (startup, Ctrl+T, ~ reload, and whatever site is added next).
    art_pairs_key_.clear();

    // Split by mode (design spec section 6.5): theme.conf governs Classic; the named
    // truecolor palettes fully own Awesome. No layering, no 8-colour degradation of a
    // truecolor palette. Awesome sets its own pairs (incl. solid base bg) and returns.
    if (config_.awesome_mode) {
        applyAwesomeTheme();
#ifdef REMOCT_PROBE
        probeLogCaps("awesome");
#endif
        return;
    }

    // Built-in defaults (index 0 unused; slots 1..13 map to CP_*). These reproduce
    // the previous hard-coded pairs exactly, so a missing theme.conf changes nothing.
    short fg[14] = { 0,
        COLOR_CYAN,   COLOR_WHITE,  COLOR_WHITE,  COLOR_WHITE,   // title focused selected progress
        COLOR_GREEN,  COLOR_RED,    COLOR_CYAN,   COLOR_WHITE,   // status_ok status_err border dim
        COLOR_GREEN,  COLOR_YELLOW, COLOR_CYAN,   COLOR_WHITE,   // viz_low viz_mid viz_high viz_peak
        COLOR_BLACK };                                           // selected_unfocused
    short bg[14] = { 0,
        -1,           COLOR_BLUE,   COLOR_BLUE,   COLOR_BLUE,
        -1,           -1,           -1,           -1,
        COLOR_GREEN,  COLOR_YELLOW, COLOR_CYAN,   COLOR_WHITE,
        COLOR_WHITE };

    loadTheme(fg, bg);   // override in place from theme.conf (emits default on first run)

    for (short s = 1; s <= 13; ++s)
        init_pair(s, fg[s], bg[s]);

    // Visualizer fractional-tip pair: peak colour on the DEFAULT background. The
    // themed viz pairs are fg==bg solid fills (a partial block would be invisible),
    // so the sub-cell lower-block glyphs (▁..▇) need a real bg to show their height.
    init_pair(CP_VIZ_TIP, fg[CP_VIZ_PEAK], -1);
    // Segmented-LED band colours on the default bg (see header). Same fg as the
    // solid viz pairs, but a real bg so the half-block glyph shows its gap.
    init_pair(CP_VIZ_LOW_B,  fg[CP_VIZ_LOW],  -1);
    init_pair(CP_VIZ_MID_B,  fg[CP_VIZ_MID],  -1);
    init_pair(CP_VIZ_HIGH_B, fg[CP_VIZ_HIGH], -1);
#ifdef REMOCT_PROBE
    probeLogCaps("classic");
#endif

    // Reset the root screen to transparent so Classic re-inherits the terminal bg
    // (undoes the Awesome stdscr base fill above). Pair 0 = terminal default under
    // use_default_colors().
    bkgd(COLOR_PAIR(0));
}

void UIManager::applyAwesomeTheme() {
    if (!has_colors()) return;
    // start_color()/use_default_colors() are guaranteed by the callers (initColours
    // routes here; the F8 handler re-applies after they have already run). We only
    // re-init pairs — no window rebuild — so a live cycle is just a recolour.
    const AwesomeTheme& t = kAwesomeThemes[config_.awesome_theme];

    // Semantic -> colour-pair mapping (design spec section 4.2). Every otherwise
    // transparent role takes bg = base so glyphs never sit on terminal-bg patches
    // inside a base-filled pane. The four viz pairs stay fg==bg solid fills; the
    // derived viz tip keeps a base bg so its fractional lower-block glyph shows.
    struct PairDef { short pair; unsigned fg; unsigned bg; };
    const PairDef defs[] = {
        { CP_TITLE,              t.title,    t.base     },
        { CP_FOCUSED,            t.selfg,    t.focus    },
        { CP_SELECTED,           t.selfg,    t.accent   },
        { CP_PROGRESS,           t.prog,     t.base     },
        { CP_STATUS_OK,          t.ok,       t.base     },
        { CP_STATUS_ERR,         t.err,      t.base     },
        { CP_BORDER,             t.border,   t.base     },
        { CP_DIM,                t.dim,      t.base     },
        { CP_VIZ_LOW,            t.viz_low,  t.viz_low  },
        { CP_VIZ_MID,            t.viz_mid,  t.viz_mid  },
        { CP_VIZ_HIGH,           t.viz_high, t.viz_high },
        { CP_VIZ_PEAK,           t.viz_peak, t.viz_peak },
        { CP_SELECTED_UNFOCUSED, t.text,     t.border   },
        { CP_VIZ_TIP,            t.viz_peak, t.base     },
        { CP_VIZ_LOW_B,          t.viz_low,  t.base     },   // segmented-LED band colours
        { CP_VIZ_MID_B,          t.viz_mid,  t.base     },   // on the base bg (gap above
        { CP_VIZ_HIGH_B,         t.viz_high, t.base     },   // each half-block segment)
    };

    if (COLORS >= 256 && can_change_color()) {
        // Truecolor path: dedupe the palette's unique hex values, assign custom slot
        // ids starting at 16 (0..15 reserved for base ANSI), init_color() each, then
        // reference them by slot id. Re-applying a theme re-init_colors the same slot
        // pool with new values — no leak, no window rebuild.
        std::vector<std::pair<unsigned, short>> slots;   // hex -> assigned slot id
        short next = 16;
        auto slotFor = [&](unsigned hex) -> short {
            for (const auto& s : slots) if (s.first == hex) return s.second;
            const short id = next++;
            const int r = (int)((hex >> 16) & 0xff);
            const int g = (int)((hex >> 8)  & 0xff);
            const int b = (int)( hex        & 0xff);
            auto sc = [](int c) -> short { return (short)((c * 1000 + 127) / 255); };  // 0..255 -> 0..1000
            init_color(id, sc(r), sc(g), sc(b));
            slots.push_back({ hex, id });
            return id;
        };
        for (const auto& d : defs)
            init_pair(d.pair, slotFor(d.fg), slotFor(d.bg));
    } else {
        // Fallback: map each hex to the nearest of the first min(COLORS,16) basic
        // colours by weighted RGB distance (2dr^2 + 4dg^2 + 3db^2). Graceful path for
        // TERM=xterm / terminals without can_change_color().
        struct RGB { int r, g, b; };
        static const RGB kBasic16[16] = {
            {0x00,0x00,0x00},{0x80,0x00,0x00},{0x00,0x80,0x00},{0x80,0x80,0x00},
            {0x00,0x00,0x80},{0x80,0x00,0x80},{0x00,0x80,0x80},{0xc0,0xc0,0xc0},
            {0x80,0x80,0x80},{0xff,0x00,0x00},{0x00,0xff,0x00},{0xff,0xff,0x00},
            {0x00,0x00,0xff},{0xff,0x00,0xff},{0x00,0xff,0xff},{0xff,0xff,0xff},
        };
        const int limit = (COLORS >= 16) ? 16 : (COLORS >= 8 ? 8 : (COLORS > 0 ? COLORS : 8));
        auto nearest = [&](unsigned hex) -> short {
            const int r = (int)((hex >> 16) & 0xff);
            const int g = (int)((hex >> 8)  & 0xff);
            const int b = (int)( hex        & 0xff);
            long best = -1; short bi = 0;
            for (int i = 0; i < limit; ++i) {
                const long dr = r - kBasic16[i].r, dg = g - kBasic16[i].g, db = b - kBasic16[i].b;
                const long d  = 2*dr*dr + 4*dg*dg + 3*db*db;
                if (best < 0 || d < best) { best = d; bi = (short)i; }
            }
            return bi;
        };
        for (const auto& d : defs)
            init_pair(d.pair, nearest(d.fg), nearest(d.bg));
    }

    // stdscr base fill: the pane/title/cwd/cmdline subwindows get a base bg via
    // wbkgd(CP_DIM) in createWindows(), but the inter-pane gutter, outer insets, and
    // any cell no subwindow covers are stdscr itself — still at the default -1, which
    // renders black. Fill the root with the same base-bg pair so no black seam shows
    // through on non-near-black bases (Zero Cool, Nord, Gruvbox). CP_DIM.bg == base in
    // Awesome; F8 re-runs applyAwesomeTheme(), so the fill tracks each theme.
    bkgd(COLOR_PAIR(CP_DIM));
}

void UIManager::loadTheme(short* fg, short* bg) {
    const std::string path = DigiConfig::themePath();

    // element name -> colour-pair slot
    struct Row { const char* name; short slot; };
    static const Row rows[] = {
        { "title",      CP_TITLE      }, { "focused",    CP_FOCUSED    },
        { "selected",   CP_SELECTED   }, { "progress",   CP_PROGRESS   },
        { "status_ok",  CP_STATUS_OK  }, { "status_err", CP_STATUS_ERR },
        { "border",     CP_BORDER     }, { "dim",        CP_DIM        },
        { "viz_low",    CP_VIZ_LOW    }, { "viz_mid",    CP_VIZ_MID    },
        { "viz_high",   CP_VIZ_HIGH   }, { "viz_peak",   CP_VIZ_PEAK   },
        { "selected_unfocused", CP_SELECTED_UNFOCUSED },
    };

    auto colourFromName = [](std::string s, short fallback) -> short {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        if (s == "default") return -1;
        if (s == "black")   return COLOR_BLACK;
        if (s == "red")     return COLOR_RED;
        if (s == "green")   return COLOR_GREEN;
        if (s == "yellow")  return COLOR_YELLOW;
        if (s == "blue")    return COLOR_BLUE;
        if (s == "magenta") return COLOR_MAGENTA;
        if (s == "cyan")    return COLOR_CYAN;
        if (s == "white")   return COLOR_WHITE;
        return fallback;    // unknown token -> keep current value
    };

    std::ifstream f(path);
    if (!f) {
        // First run (or user deleted it): emit a commented default built from the
        // defaults already in fg[]/bg[], so the file always matches the program.
        auto nameOf = [](short c) -> const char* {
            switch (c) {
                case COLOR_BLACK:   return "black";
                case COLOR_RED:     return "red";
                case COLOR_GREEN:   return "green";
                case COLOR_YELLOW:  return "yellow";
                case COLOR_BLUE:    return "blue";
                case COLOR_MAGENTA: return "magenta";
                case COLOR_CYAN:    return "cyan";
                case COLOR_WHITE:   return "white";
                default:            return "default";   // -1 and anything else
            }
        };
        std::ofstream o(path, std::ios::trunc);
        if (o) {
            o << "# RE-MOCT theme - colour-pair assignments (auto-generated default)\n"
                 "#\n"
                 "# Format:  element   fg   bg\n"
                 "# Colours: black red green yellow blue magenta cyan white default\n"
                 "#          'default' = the terminal's own foreground/background.\n"
                 "#\n"
                 "# NOTE: ncursesw on this build exposes only the 8 ANSI colours through\n"
                 "#       the API - no hex / RGB / 256-colour. Bold/dim/reverse are applied\n"
                 "#       by the program per-context and are not configurable here.\n"
                 "#\n"
                 "# NOTE: the viz_* rows intentionally use fg == bg. That paints a solid\n"
                 "#       filled cell for the visualizer bars - it is not a mistake.\n"
                 "#\n"
                 "# Reload live with '~' after editing.\n\n";
            for (const auto& r : rows)
                o << std::left << std::setw(20) << r.name
                  << std::setw(9) << nameOf(fg[r.slot])
                  << nameOf(bg[r.slot]) << '\n';
        }
        return;   // defaults already populated in fg[]/bg[]
    }

    auto slotFor = [&](const std::string& n) -> short {
        for (const auto& r : rows) if (n == r.name) return r.slot;
        return -1;
    };

    std::string line;
    while (std::getline(f, line)) {
        if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
        std::istringstream iss(line);
        std::string elem, fgs, bgs;
        if (!(iss >> elem >> fgs >> bgs)) continue;   // need all three tokens
        short slot = slotFor(elem);
        if (slot < 0) {
            Log::write("theme", "unknown element '" + elem + "' ignored");
            continue;
        }
        fg[slot] = colourFromName(fgs, fg[slot]);
        bg[slot] = colourFromName(bgs, bg[slot]);
    }
}

bool UIManager::vizStripShown() const {
    // The Awesome full-width Spectrum strip is on screen iff we're in Awesome mode
    // and win_viz_ was created as the strip (createWindows leaves it null in Awesome
    // when the strip is toggled off or the terminal is too short). In Classic,
    // win_viz_ is the right-pane overlay, not a strip, so this is false there.
    return config_.awesome_mode && win_viz_ != nullptr;
}

void UIManager::createWindows() {
    // Awesome theme insets the two panes: a 1-col gutter on each outer edge and
    // a 1-col gap between them, giving the padded "floating panel" look. Classic
    // keeps the panes flush (gut = 0), so its geometry is byte-identical to before.
    const bool aw  = config_.awesome_mode;
    const int  gut = aw ? 1 : 0;

    // Awesome mode: a full-width Spectrum strip lives below the two panes (toggle
    // 'v', default on), shortening them by kVizStripRows. Classic keeps its
    // right-pane visualizer overlay instead - no strip. Small-terminal guard:
    // if the panes would be crushed, drop the strip and give the rows back.
    bool strip = aw && awesome_viz_strip_;
    int  viz_h = strip ? kVizStripRows : 0;
    int  pane_rows = screen_rows_ - 4 - viz_h;
    if (strip && pane_rows < kMinPaneRows) {
        strip = false; viz_h = 0; pane_rows = screen_rows_ - 4;
    }

    const int  avail_cols = screen_cols_ - (aw ? 3 : 0);   // left + mid + right gutters
    const int  left_cols  = avail_cols / 2;
    const int  right_cols = avail_cols - left_cols;
    const int  dir_x      = gut;                    // 0 classic, 1 awesome
    const int  right_x    = gut + left_cols + gut;  // == left_cols when gut == 0
    win_title_    = newwin(1,         screen_cols_, 0,              0);
    win_cwd_      = newwin(1,         screen_cols_, 1,              0);
    win_dir_      = newwin(pane_rows, left_cols,    2,              dir_x);
    win_playlist_ = newwin(pane_rows, right_cols,   2,              right_x);
    if (strip)
        // Awesome full-width Spectrum strip, below the panes.
        win_viz_ = newwin(viz_h, screen_cols_, 2 + pane_rows, 0);
    else if (!aw)
        // Classic right-pane visualizer overlay (same geometry as the queue).
        win_viz_ = newwin(pane_rows, right_cols, 2, right_x);
    else
        // Awesome with the strip hidden: no viz window this layout.
        win_viz_ = nullptr;
    win_progress_ = newwin(1,         screen_cols_, screen_rows_-2, 0);
    win_cmdline_  = newwin(1,         screen_cols_, screen_rows_-1, 0);
    // Set black background on all windows so colors render correctly
    for (WINDOW* w : {win_title_, win_cwd_, win_dir_, win_playlist_,
                      win_viz_, win_progress_, win_cmdline_}) {
        if (w) wbkgd(w, COLOR_PAIR(CP_DIM));
    }
}

void UIManager::destroyWindows() {
    auto del = [](WINDOW*& w){ if (w) { delwin(w); w = nullptr; } };
    del(win_title_); del(win_cwd_); del(win_dir_); del(win_playlist_);
    del(win_viz_);   del(win_progress_); del(win_cmdline_);
}

// Theme-aware panel border.
//  • Classic theme: behaves exactly like the previous box(w,0,0) call — the
//    caller's current attributes apply, so no migrated pane changes look.
//  • Awesome theme: draws the straight edges, then rounds the four corners with
//    ╭ ╮ ╰ ╯ in an accent colour (bright cyan when focused, dim when not). The
//    corners are drawn with the wide-char API (setcchar/add_wch) so they render
//    as single cells; the bottom-right uses ins_wch so writing the last cell can
//    never advance the cursor off-window (which would scroll/ERR). An optional
//    title is laid into the top edge, inset two cells.
void UIManager::panelFrame(WINDOW* w, const std::string& title, bool focused,
                           wchar_t icon) {
    if (!w) return;
    int rows, cols;
    getmaxyx(w, rows, cols);
    if (rows < 2 || cols < 2) return;

    if (!config_.awesome_mode) {
        box(w, 0, 0);                 // classic — identical to prior behaviour
        return;
    }

    const short  pair = focused ? CP_TITLE : CP_BORDER;  // both cyan
    const attr_t at   = focused ? A_BOLD  : A_NORMAL;     // bold vs plain (no grey dim)

    wattron(w, COLOR_PAIR(pair) | at);
    box(w, 0, 0);                                          // straight edges (ACS)
    // Corners as true wide chars — single cells, no UTF-8 byte mojibake. The
    // bottom-right uses ins_wch (insert, not add) so writing the final cell can
    // never advance the cursor off-window (which would scroll/ERR, dropping it).
    auto corner = [&](int y, int x, wchar_t glyph, bool insert) {
        cchar_t cc;
        wchar_t s[2] = { glyph, 0 };
        setcchar(&cc, s, at, pair, nullptr);
        if (insert) mvwins_wch(w, y, x, &cc);
        else        mvwadd_wch(w, y, x, &cc);
    };
    corner(0,        0,        L'\u256d', false);          // ╭ top-left
    corner(0,        cols - 1, L'\u256e', false);          // ╮ top-right
    corner(rows - 1, 0,        L'\u2570', false);          // ╰ bottom-left
    corner(rows - 1, cols - 1, L'\u256f', true);           // ╯ bottom-right
    wattroff(w, COLOR_PAIR(pair) | at);

    // Title sits in the top edge. Trim surrounding padding so callers may pass an
    // already space-padded header string and still get a clean inset label.
    if (cols > 6) {
        int tx = 2;
        // Optional Nerd Font icon — drawn via the wide API (the narrow path on
        // this build doesn't decode UTF-8, so a glyph in a char string mojibakes).
        if (config_.nerd_icons && icon) {
            cchar_t cc;
            wchar_t s[2] = { icon, 0 };
            setcchar(&cc, s, A_BOLD, pair, nullptr);
            mvwadd_wch(w, 0, 2, &cc);
            tx = 4;   // glyph + one space
        }
        size_t b = title.find_first_not_of(' ');
        size_t e = title.find_last_not_of(' ');
        if (b != std::string::npos && tx < cols - 2) {
            std::string t = " " + title.substr(b, e - b + 1) + " ";
            if ((int)t.size() > cols - tx - 2) t = t.substr(0, (size_t)(cols - tx - 2));
            wattron(w, COLOR_PAIR(pair) | A_BOLD);
            mvwaddnstr(w, 0, tx, t.c_str(), (int)t.size());
            wattroff(w, COLOR_PAIR(pair) | A_BOLD);
        }
    }
}

int UIManager::paneVisibleRows(WINDOW* w) const {
    int r, c;
    getmaxyx(w, r, c);
    (void)c;
    return config_.awesome_mode ? r - 2 : r - 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main loop
// ─────────────────────────────────────────────────────────────────────────────
void UIManager::maybePreloadNext() {
    if (playlist_.repeatMode() == RepeatMode::One) return;
    if (!playlist_.queueEmpty()) return;  // queue handles next
    if (auto peek = playlist_.peekNext(); peek.has_value())
        if (!isCDTrackPath(peek.value()) && !isStreamPath(peek.value()))
            audio_.preloadNext(peek.value());
}

void UIManager::flushPendingSeek() {
    if (!seek_.pending()) return;
    // The coalescer resolves to a DELTA (absolute target - live position, or the
    // accumulated relative delta) and drops it if the track changed since the seek
    // was requested (n/p or auto-advance inside the window) so it can't leak onto
    // a new track. Absolute is resolved HERE against the live position - never
    // folded at receive time (a drag stream would accumulate against a stale pos).
    auto d = seek_.resolve((int)playlist_.current(), audio_.positionSec());
    if (!d) return;
    last_seek_apply_ = std::chrono::steady_clock::now();
    audio_.seekBy(*d);              // the one seekBy home (the MP3 bit-reservoir fix)
    redraw_needed_.store(true);
}

void UIManager::requestSeek(double delta) {
    seek_.addRelative(delta, (int)playlist_.current());
    // Apply immediately when outside the cooldown (so a single tap is instant);
    // otherwise accumulate and let the run loop flush once the window elapses.
    using namespace std::chrono;
    if (steady_clock::now() - last_seek_apply_ >= milliseconds(100))
        flushPendingSeek();
}

void UIManager::requestSeekAbs(double target) {
    // OS scrubber SetPosition. Last-write-wins absolute lane; NOT folded to a
    // delta now (a drag emits many SetPositions in one window). Same immediate/
    // coalesce timing as the relative path.
    seek_.setAbsolute(target, (int)playlist_.current());
    using namespace std::chrono;
    if (steady_clock::now() - last_seek_apply_ >= milliseconds(100))
        flushPendingSeek();
}

// ─── OS media control (osmedia-seam) ──────────────────────────────────────────
core::MediaStatus UIManager::mediaStatus() const {
    switch (audio_.state()) {
        case PlaybackState::Playing: return core::MediaStatus::Playing;
        case PlaybackState::Paused:  return core::MediaStatus::Paused;
        default:                     return core::MediaStatus::Stopped;
    }
}

// Register the inbound split sink + the command handler. Each sink routes to the
// EXACT function the keyboard uses (no parallel transport); seek goes through the
// coalescer, never a raw seekTo. The handler only enqueues (the marshal).
void UIManager::wireMediaControl() {
    MediaRouter::Sinks s;
    s.play        = [this]{ if (audio_.state() == PlaybackState::Paused) audio_.togglePause(); };
    s.pause       = [this]{ if (audio_.state() == PlaybackState::Playing) audio_.togglePause(); };
    s.togglePause = [this]{ if (audio_.state() != PlaybackState::Stopped) audio_.togglePause(); };
    s.stop        = [this]{ audio_.stop(); };
    s.next        = [this]{ manualNext(); };
    s.prev        = [this]{ manualPrevious(); };
    s.seekAbs     = [this](double t){ requestSeekAbs(t); };
    s.seekRel     = [this](double d){ requestSeek(d); };
    media_router_.setSinks(std::move(s));
    media_->setCommandHandler([this](core::MediaEvent e){ media_router_.post(e); });
    // Hand the impl the bundled logo bytes ONCE for the empty-art floor
    // (osmedia-art-floor). logoBytes() lazy-loads remoct_logo.jpg here.
    media_->setDefaultArt(logoBytes());
}

// Publish now-playing to the OS on a real change. Called from updateScrobbler's
// tail with the SAME resolved values Discord uses (no second assembler). The
// change-trigger is independent of the Discord toggle.
void UIManager::publishMedia(const std::string& artist, const std::string& track,
                             const std::string& album, const std::string& art_url,
                             const std::vector<uint8_t>& art_bytes, int pos, int dur) {
    if (!config_.os_media_control) return;
    // Art precedence (osmedia-art-floor): resolved cover BYTES first (a local
    // file's embedded picture, or radio's resolved cover from the shared
    // radio-art machinery), then a URL, then the RE-MOCT logo floor. The
    // DECISION lives once, consumer-side.
    std::string art_eff = art_bytes.empty() ? core::floorArt(art_url) : std::string();
    // The art size joins the change trigger so a cover that RESOLVES a tick after
    // the title (the radio song-entity lookup) re-publishes on the still-current
    // song - the Discord path's deferred-art-commit, expressed via the trigger.
    std::string art_id = std::to_string(art_bytes.size()) + "|" + art_eff;
    if (artist != media_last_artist_ || track != media_last_track_ || art_id != media_last_art_) {
        media_last_artist_ = artist; media_last_track_ = track; media_last_art_ = art_id;
        media_last_valid_  = true;
        core::MediaMeta m;
        m.artist = artist; m.title = track; m.album = album;
        m.art = art_eff; m.art_bytes = art_bytes;
        media_->updateNowPlaying(m, mediaStatus(), (double)pos, (double)dur);
    }
}

// Manual next/previous, extracted VERBATIM from the case 'n'/'p' bodies so the
// keyboard AND an OS Next/Previous command route through the IDENTICAL logic
// (queue priority, playlist next/prev, CD reopen, follow-mode cursor, preload).
// One home; the switch cases now just call these. (The two inner switch-breaks
// on the CD-ejected path became returns.)
void UIManager::manualNext() {
    auto play_next = [&](const std::string& path, bool from_queue = false) {
        if (!from_queue && config_.follow_playing) pl_cursor_ = (int)playlist_.current();
        std::string drive; int track_num;
        if (parseCDPath(path, drive, track_num)) {
            if (!reopenCDForAction(drive)) return;   // ejected while stopped
            audio_.playCDTrack(track_num);
            return;
        }
        if (audio_.cdMode()) audio_.closeCD();
        audio_.play(path);
        maybePreloadNext();
    };
    if (!playlist_.queueEmpty()) {
        if (auto qe = playlist_.queuePop(); qe.has_value()) {
            audio_.clearNext();
            const std::string& qpath = qe->path;
            std::string drive; int track_num;
            if (parseCDPath(qpath, drive, track_num)) {
                if (!reopenCDForAction(drive)) return;   // ejected while stopped
                audio_.playCDTrack(track_num);
                return;                                  // queue item has no playlist row
            }
            if (audio_.cdMode()) audio_.closeCD();
            play_next(qpath, true);  // from_queue=true — don't move cursor
        }
    } else if (auto p = playlist_.next(); p.has_value())
        play_next(p.value());
    else
        audio_.stop();
}

void UIManager::manualPrevious() {
    auto play_prev = [&](const std::string& path) {
        if (config_.follow_playing) pl_cursor_ = (int)playlist_.current();   // gated like manual next
        std::string drive; int track_num;
        if (parseCDPath(path, drive, track_num)) {
            if (!reopenCDForAction(drive)) return;   // ejected while stopped
            audio_.playCDTrack(track_num);
            return;
        }
        if (audio_.cdMode()) audio_.closeCD();
        audio_.play(path);
    };
    if (auto p = playlist_.previous(); p.has_value())
        play_prev(p.value());
}

// Remove every playlist + queued row for a CD drive letter and keep the cursor
// in range. Shared by the reader-thread eject path (playback) and the lazy-
// reopen eject path (stopped). (Slice 3)
void UIManager::purgeCDRows(const std::string& drive) {
    std::string prefix = drive + ":CD Track ";
    playlist_.removeIf([&](const PlaylistEntry& e) {
        return e.path.substr(0, prefix.size()) == prefix;
    });
    playlist_.queueRemoveIf([&](const PlaylistEntry& e) {
        return e.path.substr(0, prefix.size()) == prefix;
    });
    pl_cursor_ = std::min(pl_cursor_, std::max(0, (int)playlist_.size() - 1));
}

// Shift+E in [Drives]: eject the highlighted CD drive. See UIManager.h for the
// contract. drive_entry is the dir_entries_ value ("F:\" / "/dev/sr0").
void UIManager::ejectDrive(const std::string& drive_entry) {
#ifdef _WIN32
    const std::string spec = drive_entry.substr(0, 1);      // "F"
#else
    const std::string spec = drive_entry.rfind("/dev/", 0) == 0
                                 ? drive_entry.substr(5)    // "sr0"
                                 : drive_entry;
#endif
    const bool is_loaded = !cd_drive_letter_.empty() && cd_drive_letter_ == spec;

    // A rip owns the disc - ejecting mid-rip would yank the platter out from
    // under CDRipper's reads. Refuse; the user cancels the rip first (Ctrl+Y).
    if (is_loaded && cd_ripper_.isActive()) {
        showTrackToast("Rip in progress", "Cancel it first (Ctrl+Y)", "");
        redraw_needed_.store(true);
        return;
    }

    if (is_loaded) {
        // Stop-first (strict drives like the HL-DT require it), then the full
        // slice-3 unload so no stale "loaded" state survives the eject: handle
        // closed (the media-removal unlock runs in the device destructor), rows
        // purged, letter + MB metadata cleared - the same cleanup as the
        // reader-thread eject path.
        audio_.stop();          // CD branch closes the handle + tears down the device
        audio_.closeCD();       // idempotent after stop(); covers the stopped case
        audio_.clearTrackEnd();
        purgeCDRows(spec);
        cd_drive_letter_.clear();
        mb_lookup_.cancel();
        mb_fetching_.store(false);
        {
            std::lock_guard<std::mutex> lk(mb_mutex_);
            mb_album_.clear();
            mb_error_.clear();
            mb_release_ = {};
        }
    }
    // Send the physical eject on a fresh seam handle - for a drive RE-MOCT never
    // loaded this is the only device access (no TOC parse, open + eject + close).
    bool ok = false;
    if (auto dev = core::cdio().open(spec)) ok = dev->eject();
    // On failure, do NOTHING but toast. A "wedge-breaker" that re-read the
    // platter on the failure path was tried and REVERTED (hardware-tested): on
    // the HL-DT family raw reads RE-ASSERT the soft media-removal lock - the
    // very lock a failed eject leaves stuck - so the recovery deepened the
    // wedge and poisoned the play->stop release too. The honest state: on this
    // firmware a failed software eject leaves the button dead until the next
    // real play->stop cycle (its settle-time is what actually releases the
    // drive; reads alone reproduce the lock, not the release).

    // Clear the hint immediately - the disc is gone (or the user is about to take
    // it via the button after a failed command); F12 re-scans either way.
    cd_drives_with_media_.erase(
        std::remove(cd_drives_with_media_.begin(), cd_drives_with_media_.end(),
                    drive_entry),
        cd_drives_with_media_.end());
    showTrackToast(ok ? ("Ejected " + drive_entry)
                      : "This drive won't eject by software - stop playback and use the drive button", "", "");
    redraw_needed_.store(true);
}

// Ensure the CD handle is open & ready before a play / rip / MB action on a
// stopped-but-loaded disc. See the declaration in UIManager.h for the full
// contract. Returns true iff ready; on false (empty tray) the disc has been
// fully unloaded and the caller must bail. (Slice 3)
bool UIManager::reopenCDForAction(const std::string& drive) {
    if (audio_.cdMode()) return true;          // handle already open & active
    if (drive.empty())   return false;         // no disc known to reopen

    // Bounded retry + failure discrimination (Slice 3 re-gate): a reopen right
    // after file playback can hit a TRANSIENT openCD failure (WASAPI uninit->init
    // settling). A bare single attempt escalated that blip into the destructive
    // eject purge below, which unloaded a physically-present disc on a
    // CD->file->CD round-trip. checkMedia() cannot discriminate here - after a
    // failed open() dev_ is null, so it short-circuits false without asking the
    // drive. Two discriminators instead:
    //   1) retry: a transient busy clears within an attempt or two; a real empty
    //      tray fails every attempt. 3 attempts / ~200 ms apart, only inside an
    //      explicit user action (never a poll) - two brief retries cannot become
    //      an audible seek-storm. Do not grow these bounds.
    //   2) failure point: CDSource::lastOpenFail(). DeviceOpen = the OS handle
    //      couldn't be acquired (busy/contention - NOT proof of an empty tray);
    //      TocRead/NoAudioTracks = the drive answered and there is no readable
    //      audio disc (confirmed empty).
    // Purge only on a CONFIRMED empty tray. When in doubt, leave the rows alone:
    // a stale row self-heals on the next action; nuking a loaded disc does not.
    // Every failed attempt is logged so a misbehaving drive is diagnosed from
    // the log, not from catching a flashing toast by eye.
    bool confirmed_no_disc = false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (attempt > 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (audio_.openCD(drive)) {          // reopened: TOC re-read, device re-init'd
            if (attempt > 1)
                Log::writef("cd", "reopen %s: recovered on attempt %d/3 (transient)",
                            drive.c_str(), attempt);
            return true;
        }
        auto fail = audio_.cdSource().lastOpenFail();
        const char* why =
            (fail == CDSource::OpenFail::DeviceOpen)    ? "io.open (device handle)" :
            (fail == CDSource::OpenFail::TocRead)       ? "readToc"                 :
            (fail == CDSource::OpenFail::NoAudioTracks) ? "no audio tracks"         :
                                                          "unknown";
        Log::writef("cd", "reopen %s: attempt %d/3 failed at %s",
                    drive.c_str(), attempt, why);
        // Latest attempt wins: only a drive that ANSWERED (handle ok, no TOC /
        // no audio) confirms an empty tray. A DeviceOpen failure leaves it
        // unconfirmed even if an earlier attempt said otherwise.
        confirmed_no_disc = (fail == CDSource::OpenFail::TocRead ||
                             fail == CDSource::OpenFail::NoAudioTracks);
    }

    if (!confirmed_no_disc) {
        // Device busy / handle contention - not proof the disc is gone. Keep the
        // rows, keep cd_drive_letter_, tell the user, and let the next action
        // retry. (Pre-Slice-3 behaviour for a failed openCD was exactly this
        // benign: playCDTrack returned false and nothing was destroyed.)
        Log::writef("cd", "reopen %s: all attempts busy - disc state kept", drive.c_str());
        showTrackToast("CD drive busy", "Try again", "");
        redraw_needed_.store(true);
        return false;
    }

    // Confirmed empty tray -> ejected while stopped. This is the only
    // eject-while-stopped detection point now that the poll purge is gone.
    // Unload fully so no stale "loaded" state (rows / [CD] tag / album /
    // cached release) survives for a disc that is physically gone.
    Log::writef("cd", "reopen %s: confirmed no disc - unloading", drive.c_str());
    purgeCDRows(drive);
    cd_drive_letter_.clear();
    cd_poll_ticks_ = 0;
    cd_fail_count_ = 0;
    mb_lookup_.cancel();
    mb_fetching_.store(false);
    {
        std::lock_guard<std::mutex> lk(mb_mutex_);
        mb_album_.clear();
        mb_error_.clear();
        mb_release_ = {};
    }
    cd_ripper_.cancel();
    showTrackToast("CD ejected", "Disc removed", "");
    redraw_needed_.store(true);
    return false;
}

void UIManager::run() {
    last_playlist_current_for_sync_ = (int)playlist_.current();
    // Directory watch — poll every ~2s for changes
    constexpr int DIR_POLL_INTERVAL = 25;
    try { dir_mtime_ = fs::last_write_time(current_dir_); } catch (...) {}

    while (running_) {
        audio_.pollEvents();
        // OS media control (osmedia-seam): service the transport (Linux MPRIS bus;
        // Windows no-op) then drain queued transport commands on THIS (UI) thread,
        // beside the seek flush. Commands from an OS callback are applied here, not
        // inline in the callback - the marshal. pump()/drain() are cheap no-ops when
        // media is disabled or idle.
        if (config_.os_media_control) {
            media_->pump();
            // Transport applies regardless of any open modal - a media key should
            // work whatever the TUI is showing - EXCEPT during an active CD rip,
            // where the ripper owns the drive and a background track-switch
            // (manualNext -> closeCD/playCDTrack) would corrupt it. Drop queued
            // commands for that window only (osmedia-seam, the modal confirm).
            if (cd_ripper_.isActive()) media_router_.clear();
            else                       media_router_.drain();
        }
        // Flush a coalesced seek once the user stops hammering [/] (the cooldown
        // makes single taps instant and turns held repeats into a few seeks, not many).
        if (seek_.pending() &&
            std::chrono::steady_clock::now() - last_seek_apply_ >= std::chrono::milliseconds(100))
            flushPendingSeek();
        updateScrobbler();   // publishes now-playing to the OS on change (publishMedia)
        // OS media control: per-tick position refresh (cheap; the impl throttles
        // its own signal emission) + clear on the transition to stopped so the OS
        // surface does not keep a dead track. Metadata itself is pushed on change
        // by publishMedia from updateScrobbler.
        if (config_.os_media_control) {
            core::MediaStatus st = mediaStatus();
            if (st == core::MediaStatus::Stopped) {
                if (media_last_valid_) {
                    media_->clear();
                    media_last_valid_ = false;
                    media_last_artist_.clear(); media_last_track_.clear(); media_last_art_.clear();
                }
            } else {
                double pos = audio_.streamMode() ? (double)audio_.streamPositionSec()
                                                 : audio_.positionSec();
                double dur = audio_.streamMode() ? 0.0 : audio_.durationSec();
                media_->updatePosition(pos, dur, st);
            }
        }
        // Stream-record R2: hand the polled now-playing to the recorder —
        // onTitle dedups internally (a repeat can never cut twice) and
        // no-ops when idle/split-off, so the unconditional per-tick call is
        // safe; the fetch is one the scrobbler/art paths already make. The
        // panel refreshes its live state at ~2 Hz (80 ms ticks x 6).
        if (audio_.streamMode()) {
            const bool recording = audio_.streamRecorder().recording();
            // Drive the SAME radio-art machinery the Info pane uses (single-
            // in-flight guard, negative cache, provider order: station cover
            // then an iTunes/Deezer song-entity lookup) whenever radio art is
            // WANTED - for the recorder OR the OS media card - so radio_bytes_
            // resolves even with the pane closed and nothing recording. This is
            // the same resolution the Discord path does; radio_bytes_ is the one
            // home, reused here instead of a second lookup.
            if (recording || config_.os_media_control) {
                std::string np = audio_.streamNowPlaying();
                std::string artist, title;
                std::string key = radioSongIdentity(np, artist, title);
                radioArtPickup(key);
                if (!key.empty()) radioArtKick(artist, title, key);
                else radioArtFloor(key);   // metadata dip: reset so the song's
                                           // return re-kicks (radio-art-refresh-fix)
                if (recording) {
                    audio_.streamRecorder().onTitle(np);
                    // Hand the resolved image to the recorder keyed by the RAW
                    // now-playing (matches cut_raw_ at tag time). Once per title.
                    if (!key.empty() && radio_bytes_key_ == key && radio_bytes_resolved_ &&
                        !radio_bytes_.empty() && np != rec_art_pushed_key_) {
                        rec_art_pushed_key_ = np;
                        audio_.streamRecorder().onArt(np, radio_bytes_);
                    }
                }
            }
            if (recording && ui_overlay_ == UIOverlay::RecPanel && (++rec_panel_tick_ % 6) == 0)
                redraw_needed_.store(true);
        }
        // [REC] pulse driver: a redraw every ~0.64s while recording so the
        // badge breathes (one atomic read per tick when idle).
        if (audio_.streamRecorder().recording() && (++rec_pulse_tick_ % 8) == 0) {
            ++rec_pulse_;
            redraw_needed_.store(true);
        }
        // batch-r128: scan progress on the status line (~1 Hz) + the summary
        // toast when the worker finishes. All reads are cheap atomics.
        if (gain_scan_.running()) {
            static int rg_tick = 0;
            if ((++rg_tick % 12) == 0) {
                rip_status_ = "ReplayGain " + std::to_string(gain_scan_.index()) + "/" +
                              std::to_string(gain_scan_.total()) + "  " +
                              sanitizeForDisplay(gain_scan_.currentFile());
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            }
        }
        if (gain_scan_.takeFinished()) {
            std::string sum = std::to_string(gain_scan_.tagged()) + " tagged, " +
                              std::to_string(gain_scan_.skipped()) + " already had gain, " +
                              std::to_string(gain_scan_.errors()) + " error(s)";
            if (gain_scan_.wavNoted() > 0)
                sum += ", " + std::to_string(gain_scan_.wavNoted()) + " wav skipped";
            showTrackToast(gain_scan_.cancelled() ? "ReplayGain scan cancelled"
                                                  : "ReplayGain scan done", sum, "");
            rip_status_.clear();
            redraw_needed_.store(true);
        }
        // convert-core: the same status-line/toast shape as the RG scan.
        if (convert_job_.running()) {
            static int cv_tick = 0;
            if ((++cv_tick % 12) == 0) {
                rip_status_ = "Converting " + std::to_string(convert_job_.index()) + "/" +
                              std::to_string(convert_job_.total()) + "  " +
                              sanitizeForDisplay(convert_job_.currentFile());
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            }
        }
        if (convert_job_.takeFinished()) {
            std::string sum = std::to_string(convert_job_.converted()) + " converted, " +
                              std::to_string(convert_job_.skipped()) + " skipped, " +
                              std::to_string(convert_job_.errors()) + " error(s)";
            showTrackToast(convert_job_.cancelled() ? "Convert cancelled"
                                                    : "Convert done", sum, "");
            rip_status_.clear();
            redraw_needed_.store(true);
        }
        updateBookProgress();
        // Async stream connect/fail confirmation toasts. Un-gated at slice 5:
        // this was #ifdef _WIN32 slice-1 scaffolding from when notify was a
        // Linux no-op. Streams are ported (slices 2/3) and every dep here is
        // portable — takeStreamConnected/Failed are plain atomics, stationLabel/
        // streamUrl already run on Linux via drawProgress. Windows is a
        // preprocessed non-event (the block already compiled under _WIN32); this
        // only ADDS the toasts to the Linux build. The file-track toast below
        // deliberately skips streams (curIsStream), so these own stream toasts —
        // no double toast. (Dos-found closing slice 5: "Streaming"/"FAILED"
        // toasts were dead on Linux — same class as 4f0b240 / 91caf7a.)
        if (audio_.takeStreamConnected()) {
            // Label from the URL actually streaming, not playlist_.current(): a
            // station launched from the override queue has no playlist row, so
            // current() is stale (the prior file) and would mislabel the toast.
            std::string t = stationLabel(audio_.streamUrl());
            showTrackToast("Streaming", t, "");
        }
        if (audio_.takeStreamFailed())
            showTrackToast("Radio stream connect FAILED", "", "");

        // Windows-console size poll + forced ~80ms repaint. This whole heartbeat is
        // a Windows-ONLY workaround: ConPTY (Windows Terminal/conhost) doesn't
        // deliver KEY_RESIZE reliably, so we poll the window rect and force a full
        // repaint. wingui (REMOCT_PDCURSES) repaints its own GDI window and Linux
        // ncursesw gets real KEY_RESIZE via SIGWINCH - neither needs this, and the
        // unconditional redraw_needed_ every tick just churns the panes (flicker /
        // input-timing glitches). So gate it to the Windows console build only;
        // everywhere else, KEY_RESIZE + the per-change redraw triggers + the
        // lightweight per-tick draw (title/cwd/progress/viz) cover everything.
#if !defined(REMOCT_PDCURSES) && defined(_WIN32)
        static int resize_poll = 0;
        if (++resize_poll >= 1) {
            resize_poll = 0;
            int new_cols = screen_cols_, new_rows = screen_rows_;

            // Windows Terminal ConPTY workaround:
            // GetConsoleScreenBufferInfo returns stale data.
            // Use the actual Win32 window rect + font metrics instead.
            HWND hwnd = GetConsoleWindow();
            if (hwnd) {
                RECT rect {};
                GetClientRect(hwnd, &rect);
                int px_w = rect.right  - rect.left;
                int px_h = rect.bottom - rect.top;
                if (px_w > 0 && px_h > 0) {
                    // Get font size from current console font
                    CONSOLE_FONT_INFOEX fi {};
                    fi.cbSize = sizeof(fi);
                    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
                    if (GetCurrentConsoleFontEx(h, FALSE, &fi) &&
                        fi.dwFontSize.X > 0 && fi.dwFontSize.Y > 0) {
                        new_cols = px_w / fi.dwFontSize.X;
                        new_rows = px_h / fi.dwFontSize.Y;
                    }
                }
            }

            // Fallback: try CONOUT$
            if (new_cols == screen_cols_ && new_rows == screen_rows_) {
                HANDLE hc = CreateFileA("CONOUT$", GENERIC_READ|GENERIC_WRITE,
                    FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, 0, nullptr);
                if (hc != INVALID_HANDLE_VALUE) {
                    CONSOLE_SCREEN_BUFFER_INFO csbi {};
                    if (GetConsoleScreenBufferInfo(hc, &csbi)) {
                        new_cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
                        new_rows = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
                    }
                    CloseHandle(hc);
                }
            }

            if (new_cols > 0 && new_rows > 0 &&
                (new_cols != screen_cols_ || new_rows != screen_rows_)) {
                screen_cols_ = new_cols;
                screen_rows_ = new_rows;
                resizeWindows();
            }
            redraw_needed_.store(true);
        }
#endif

        // Force redraw when lyrics active (sync with playback position)
        if (right_pane_ == RightPane::Lyrics)
            redraw_needed_.store(true);

        // Force redraw when marquee is scrolling so title updates every tick
        {
            const auto& track = audio_.currentTrack();
            std::string np = track.artist.empty() ? track.title : track.artist + " - " + track.title;
            std::string right_approx = "  RE-MOCT v" REMOCT_VERSION " ";
            int max_np = screen_cols_ - (int)right_approx.size() - 4;
            if (!np.empty() && max_np > 0 && (int)np.size() > max_np)
                redraw_needed_.store(true);
        }

        // Advance shared text scroll offset on wall-clock timer (~300ms per step)
        // Using clock time instead of tick count so keypress rate doesn't affect speed
        {
            static auto last_scroll = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_scroll).count() >= 300) {
                last_scroll = now;
                ++text_scroll_offset_;
                redraw_needed_.store(true);
            }
        }

        // Sleep timer check
        if (sleep_minutes_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::steady_clock::now() - sleep_start_).count();
            if (elapsed >= sleep_minutes_) {
                audio_.stop();
                sleep_minutes_ = 0;
                redraw_needed_.store(true);
            }
        }

        // Slice 3: the old stopped-state media poll here was removed. Slice 1
        // proved it never checked real media — stop() closes the handle, so it
        // only ever ran checkMedia() on a null handle (always false) and became a
        // 6-second timer that purged the CD rows + MB metadata of a stopped-but-
        // loaded disc. A stopped-but-loaded disc is now a normal, persistent state
        // (rows survive; [CD] tag + album key off cd_drive_letter_). We cannot
        // poll an open idle drive either (probe B2: it spins the drive up audibly,
        // and the syscall returns before the motor is at speed so latency can't
        // see it). Eject-while-stopped is instead detected on the next lazy reopen
        // (reopenCDForAction -> openCD fails on an empty tray).

        // When CD is playing, use the reader thread's media_removed_ flag
        if (audio_.cdMode() && audio_.cdSource().mediaRemoved()) {
            purgeCDRows(cd_drive_letter_);
            audio_.closeCD();
            audio_.clearTrackEnd();
            cd_drive_letter_.clear();
            cd_fail_count_ = 0;
            cd_poll_ticks_ = 0;
            mb_lookup_.cancel();
            mb_fetching_.store(false);
            {
                std::lock_guard<std::mutex> lk(mb_mutex_);
                mb_album_.clear();
                mb_error_.clear();
                mb_release_ = {};
            }
            cd_ripper_.cancel();
            ui_overlay_ = UIOverlay::None;
            pl_cursor_ = std::min(pl_cursor_, std::max(0, (int)playlist_.size() - 1));
            redraw_needed_.store(true);   // scroll: invariant reveals the clamped cursor
        }
        if (playlist_.drainPending())
            redraw_needed_.store(true);
        // Clear MB error message after ~5 seconds (62 ticks * 80ms)
        static int mb_err_ticks = 0;
        {
            std::lock_guard<std::mutex> lk(mb_mutex_);   // mb_error_ written by worker threads
            if (!mb_error_.empty()) {
                if (++mb_err_ticks > 62) {
                    mb_error_.clear(); mb_err_ticks = 0; redraw_needed_.store(true);
                }
            } else mb_err_ticks = 0;
        }
        if (mb_titles_pending_.exchange(false)) {
            // A worker cached a release; apply its titles to playlist_ here on the
            // UI thread. Snapshot under the lock so a concurrent worker write to
            // mb_release_ can't tear the copy.
            MBRelease rel_snapshot;
            { std::lock_guard<std::mutex> lk(mb_mutex_); rel_snapshot = mb_release_; }
            applyReleaseTitles(rel_snapshot);
            redraw_needed_.store(true);
        }
        if (mb_search_close_pending_.load()) {
            mb_search_close_pending_.store(false);
            mb_search_ = {};
            if (mb_search_win_) { delwin(mb_search_win_); mb_search_win_ = nullptr; }
            ui_overlay_ = UIOverlay::None;
            redraw_needed_.store(true);
        }
        {
            std::lock_guard<std::mutex> lk(mb_mutex_);   // mb_status_ written by worker threads
            if (!mb_status_.empty()) {
                if (++mb_status_ticks_ > MB_STATUS_LIFETIME) {
                    mb_status_.clear();
                    mb_status_ticks_ = 0;
                    redraw_needed_.store(true);
                }
            }
        }
        if (!rip_status_.empty() && !cd_ripper_.isActive()) {
            if (++rip_msg_ticks_ > 100) {
                rip_status_.clear();
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            }
        } else if (cd_ripper_.isActive()) {
            rip_msg_ticks_ = 0;
        }
        // Age the [THEME:name] cwd tag (~10s). Repaint only at the fade point
        // (bold -> dim) and when it is removed - the tag is static in between, so
        // no per-tick repaint (stays flash-free on wingui).
        if (theme_tag_ticks_ < kThemeTagGoneTick) {
            ++theme_tag_ticks_;
            if (theme_tag_ticks_ == kThemeTagFadeTick ||
                theme_tag_ticks_ == kThemeTagGoneTick)
                redraw_needed_.store(true);
        }
        // Expire the transient cmdline warning after ~5s (63 ticks * 80ms).
        if (!warn_msg_.empty() && ++warn_msg_ticks_ > 62) {
            warn_msg_.clear();
            warn_msg_ticks_ = 0;
            redraw_needed_.store(true);
        }
#ifndef _WIN32
        // Expire the toast-fallback status line (same cadence as rip_status_).
        if (!status_msg_.empty() && ++status_msg_ticks_ > 60) {
            status_msg_.clear();
            status_msg_ticks_ = 0;
            redraw_needed_.store(true);
        }
#endif
        {
            int cur = (int)playlist_.current();
            if (cur != last_playlist_current_for_sync_) {
                last_playlist_current_for_sync_ = cur;
                // Toast for a new FILE track. Skip when the newly current entry is a
                // radio stream: streams aren't reflected in audio_.currentTrack()
                // (it still holds the last file), so toasting here re-announces the
                // stale file — the double toast on file->radio. The radio path
                // (beginStream -> "Negotiating/Streaming") owns stream toasts.
                bool curIsStream = (cur >= 0 && (size_t)cur < playlist_.size()
                                    && isStreamPath(playlist_.at((size_t)cur).path));
                const auto& track = audio_.currentTrack();
                if (!curIsStream && !track.path.empty()) {
                    config_.recordPlay(track.path);
                    if (config_.toast_enabled)
                        showTrackToast(track.title, track.artist, track.album);
                }
            }
        }

        // Follow-the-playing-row (F3): snap the cursor on ANY now-playing change
        // (file, CD, stream), not just playlist_.current() changes - starting a
        // stream never moves current(), so a song->radio or CD->radio transition
        // was invisible to the cur-gated block above (launch sites set pl_cursor_
        // directly, which is why launching a station worked). Keyed off
        // nowPlayingRow() as before: nullopt (queue-launched station, nothing
        // playing) resets the marker and leaves the cursor put. Runs every tick;
        // nowPlayingRow() is one playlist scan. The cursor snap has exactly one
        // owner - this block; scroll follows via the draw-time invariant (slice 5).
        if (config_.follow_playing) {
            if (auto row = nowPlayingRow(); row) {
                if ((int)*row != last_now_playing_row_) {
                    last_now_playing_row_ = (int)*row;
                    if (pl_cursor_ != (int)*row) {
                        pl_cursor_ = (int)*row;
                        redraw_needed_.store(true);
                    }
                }
            } else {
                last_now_playing_row_ = -1;
            }
        }

        // ── 2. GRAB INPUT after geometry is confirmed valid ──
        int ch = getch();

        // KEY_RESIZE or Ctrl+L: force layout recalculation and redraw
        if (ch == KEY_RESIZE || ch == 12) {
            resizeWindows();
            continue;
        }

        // ── 3. HANDLE INPUT before drawing ──
        // Draw AFTER the key is applied, so a keypress takes effect in this SAME
        // iteration. (Previously the draw ran first and a keypress only showed on
        // the next loop - and the next getch() blocks up to ~80ms first, so every
        // pane switch / action carried up to ~80ms latency. Very sluggish on Linux
        // where nothing else was forcing an interim repaint.) Modal-open handlers
        // set redraw_needed_ themselves, so the draw block below paints them too.
        if (ch != ERR) {
            if (goto_active_)
                handleGotoInput(ch);
            else if (ui_overlay_ == UIOverlay::MBSearch)
                handleMBSearchInput(ch);
            else
                handleInput(ch);
            if (ui_overlay_ == UIOverlay::None)
                redraw_needed_.store(true);
        }

        // Periodically check if the current directory changed on disk
        if (!in_drive_list_ && ++dir_poll_ticks_ >= DIR_POLL_INTERVAL) {
            dir_poll_ticks_ = 0;
            try {
                auto mtime = fs::last_write_time(current_dir_);
                if (mtime != dir_mtime_) {
                    dir_mtime_ = mtime;
                    int saved_cursor = dir_cursor_;
                    int saved_scroll = dir_scroll_;
                    refreshDir();
                    // Restore cursor position if still valid
                    dir_cursor_ = std::min(saved_cursor, (int)dir_entries_.size() - 1);
                    dir_scroll_ = std::min(saved_scroll, std::max(0, (int)dir_entries_.size() - 1));
                    redraw_needed_.store(true);
                }
            } catch (...) {}
        }

        // Always recompute viz bins so bars animate smoothly (Classic right-pane
        // overlay OR the Awesome full-width strip).
        if (right_pane_ == RightPane::Visualizer || vizStripShown())
            computeVizBins();

        // Animate the breathing progress head — only in Awesome mode while playing,
        // so Classic mode keeps its repaint-on-change cadence (no extra cost).
        if (config_.awesome_mode && audio_.state() == PlaybackState::Playing)
            redraw_needed_.store(true);

        // Terminal too small: resizeWindows() destroyed the pane windows and
        // returned without recreating them (see its size guard). Both draw paths
        // below dereference those windows (getmaxyx / wnoutrefresh), which crashes
        // on a null WINDOW*. Paint a safe notice straight onto stdscr and skip all
        // pane drawing until the terminal grows back to a usable size.
        if (!win_dir_ || !win_title_ || !win_playlist_ ||
            !win_progress_ || !win_cmdline_) {
            werase(stdscr);
            if (screen_rows_ > 0 && screen_cols_ > 0) {
                const char* m1 = "Terminal too small";
                const char* m2 = "(min 40 x 9)";
                int y  = screen_rows_ / 2;
                int x1 = (screen_cols_ - (int)strlen(m1)) / 2; if (x1 < 0) x1 = 0;
                int x2 = (screen_cols_ - (int)strlen(m2)) / 2; if (x2 < 0) x2 = 0;
                mvaddnstr(y, x1, m1, screen_cols_);
                if (y + 1 < screen_rows_) mvaddnstr(y + 1, x2, m2, screen_cols_);
            }
            wnoutrefresh(stdscr);
            doupdate();
            redraw_needed_.store(false);
        } else
        if (redraw_needed_.load()) {
            // slice 6: overlay dispatch is common — RipConfirm is live on Linux
            // (^Y rip). MBSearch draw/input handlers are portable too; the overlay
            // just never opens on Linux because ^F (case 6) stays gated.
            if (ui_overlay_ != UIOverlay::None) {
                if (ui_overlay_ == UIOverlay::RipConfirm) drawRipConfirm();
                else if (ui_overlay_ == UIOverlay::MBSearch) drawMBSearch();
                else if (ui_overlay_ == UIOverlay::RecPanel) drawRecPanel();
                else if (ui_overlay_ == UIOverlay::ConvertScope) drawConvertScope();
                else if (ui_overlay_ == UIOverlay::ConvertConfirm) drawConvertConfirm();
                redraw_needed_.store(false);
            } else {
            drawAll();
            redraw_needed_.store(false);
            }
        } else {
            if (ui_overlay_ != UIOverlay::None) {
                // No change — modal stays on screen, do nothing
            } else {
            drawTitleBar();
            drawCwd();
            drawProgress();
            // Animate the spectrum: Classic right-pane overlay OR the Awesome strip.
            if (right_pane_ == RightPane::Visualizer || vizStripShown()) {
                drawVisualizer();
                wnoutrefresh(win_viz_);
            }
            wnoutrefresh(win_title_);
            wnoutrefresh(win_cwd_);
            wnoutrefresh(win_progress_);
            doupdate();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Visualizer DSP – FFT magnitude integrated into log-spaced frequency bins
// ─────────────────────────────────────────────────────────────────────────────
void UIManager::computeVizBins() {
    static constexpr int N = AudioManager::VIZ_BUF_SIZE;
    static float samples[N];

    // ── viz-normalize tunables (dial on hardware, no rebuild-per-guess) ──────
    // Deliberately MODE-AGNOSTIC: this is shared DSP feeding viz_smoothed_[],
    // which Classic, Awesome, and both F2 styles all consume. If the fixed
    // output makes Classic feel too busy, dial THESE to a house setting that
    // serves both modes - do NOT add an awesome_mode branch here (decided with
    // Dos; a mode gate would knowingly leave the bug live in one consumer).
    static constexpr float VIZ_TILT          = 0.75f;   // treble lift exponent (0=off, ~1 = +6 dB/oct)
    static constexpr float VIZ_TILT_PIVOT_HZ = 500.0f;  // freq that stays put under tilt
                                                        // (was 1000: everything below the
                                                        // pivot is ATTENUATED by the tilt,
                                                        // which dipped the low-mids; at 500
                                                        // only true sub-bass sits below)
    static constexpr float VIZ_PEAK_COUPLE   = 0.15f;   // per-band peak floor, fraction of global peak
                                                        // (was 0.20. NOTE the direction: quiet bands
                                                        // display mag/(COUPLE*global), so LOWERING the
                                                        // couple LIFTS them; raising it deadens them
                                                        // and also suppresses silence-pump. Dial DOWN
                                                        // for livelier quiet bands, UP if silence pumps)

    int got = audio_.copySamples(samples, N);
    if (got == 0) {
        viz_smoothed_.fill(0.0f);
        return;
    }

    // Apply Hann window
    for (int i = 0; i < N; ++i) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * i / (N - 1)));
        samples[i] *= w;
    }

    // Slice C: ONE real FFT per frame -> honest, alias-free magnitudes for
    // k in [0, N/2). The stride-4 DFT this replaced was only coherent up to
    // ~sr/8 (~5.5 kHz @ 44.1k) - the top ~10 bars were aliased noise, which
    // no amount of A+B tuning could restore. Also ~30x cheaper (measured in
    // viz_fft_test: N log N once vs N^2-ish per band).
    viz_fft_.magnitude(samples, viz_fft_mag_.data());

    // Log-spaced frequency bin edges (20 Hz – 18 kHz mapped across VIZ_BINS)
    // For each band integrate the FFT magnitude over that freq range
    const float sr     = (audio_.currentTrack().sample_rate > 0)
                       ? (float)audio_.currentTrack().sample_rate
                       : 44100.0f;
    const float f_min  = 20.0f;
    const float f_max  = 18000.0f;
    const float log_min = std::log(f_min);
    const float log_max = std::log(f_max);

    for (int b = 0; b < VIZ_BINS; ++b) {
        float f_lo = std::exp(log_min + (log_max - log_min) *  b      / VIZ_BINS);
        float f_hi = std::exp(log_min + (log_max - log_min) * (b + 1) / VIZ_BINS);

        int k_lo = (int)(f_lo / sr * N);
        int k_hi = (int)(f_hi / sr * N);
        k_lo = std::clamp(k_lo, 1, N/2);
        k_hi = std::clamp(k_hi, 1, N/2);
        if (k_hi <= k_lo) {
            if (k_lo < N/2) {
                // A LOW band whose log-spaced range collapsed onto a single FFT
                // bin (many fine bins pack into the coarse low-k region). Use that
                // one bin instead of decaying to nothing - otherwise low bands drop
                // out entirely (worse with more VIZ_BINS).
                k_hi = k_lo + 1;
            } else {
                // Genuinely at/above Nyquist (low-sample-rate source): nothing maps
                // here; decay and skip so the divisor below can't be zero (NaN).
                viz_smoothed_[b] *= 0.5f;
                continue;
            }
        }

        // Band magnitude = AVERAGE of the FFT bins in [k_lo, k_hi). Average,
        // not sum: log bands widen with frequency, and summing would bake a
        // second width-proportional tilt on top of VIZ_TILT (the dialed house
        // tilt would silently stop meaning what it says). Not max: peak-biased
        // and jumpy under smoothing. Average also preserves the old code's
        // semantics (it, too, divided by the k-count), so A+B consumes this
        // exactly as before - only the magnitude source changed.
        float mag = 0.0f;
        for (int k = k_lo; k < k_hi; ++k)
            mag += viz_fft_mag_[k];
        mag /= (float)(k_hi - k_lo);

        // Perceptual tilt (B): lift treble toward realistic display balance while
        // preserving relative dynamics. Applied BEFORE peak tracking so the
        // per-band peak tracks the tilted magnitude (tilting after normalization
        // would double-count).
        const float f_center = std::sqrt(f_lo * f_hi);   // geometric band centre
        mag *= std::pow(f_center / VIZ_TILT_PIVOT_HZ, VIZ_TILT);

        // Coupled per-band peak normalization (A). The old code was a loop-scope
        // `static float peak_mag` - ONE global AGC shared by all 64 bands, so
        // every band divided by the (bass-dominated) loudest band's peak and the
        // top pinned low. Now each band self-scales against its own rolling peak,
        // but that peak is floored to a fraction of the GLOBAL rolling peak so a
        // near-silent band cannot normalize its own noise up to full height.
        // Both peaks are members (persist across calls, slow 0.9995 decay - the
        // old scalar's rule); the global one is updated per band visit exactly as
        // the old scalar was, converging on the loudest band's magnitude.
        if (mag > viz_global_peak_) viz_global_peak_ = mag;
        else                        viz_global_peak_ *= 0.9995f;

        float& pk = viz_peak_[b];
        if (mag > pk) pk = mag;
        else          pk *= 0.9995f;

        float eff_peak = std::fmax(pk, VIZ_PEAK_COUPLE * viz_global_peak_);
        float val = (eff_peak > 0.0f) ? (mag / eff_peak) : 0.0f;
        // Power curve: raises quiet parts, keeps peaks near top
        val = std::pow(val, 0.6f);
        val = std::clamp(val, 0.0f, 0.95f);

        // Exponential smoothing: very fast attack, moderate decay. Slow the
        // fall-off a touch more in Awesome (only one viz mode is on screen at a
        // time, so Classic's locked feel is unchanged).
        float prev  = viz_smoothed_[b];
        float decay = config_.awesome_mode ? 0.178f : 0.25f;   // Awesome: +10% falloff
        float alpha = (val > prev) ? 0.85f : decay;
        viz_smoothed_[b] = prev + alpha * (val - prev);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Drawing
// ─────────────────────────────────────────────────────────────────────────────
void UIManager::drawAll() {
    // Backdrop: refresh stdscr first so the inter-pane gutter, outer insets, and any
    // cell no subwindow covers show the themed base (Awesome) or terminal default
    // (Classic) set on stdscr by applyAwesomeTheme()/initColours(). The subwindow
    // refreshes below overlay it in the virtual screen; doupdate flushes once, no
    // flicker. Without this, the Awesome stdscr base fill would not repaint on an F8
    // cycle (no resize runs there to push stdscr).
    wnoutrefresh(stdscr);
    drawTitleBar();
    drawCwd();
    drawDirBrowser();
    if      (right_pane_ == RightPane::Playlist)   drawPlaylist();
    else if (right_pane_ == RightPane::Visualizer)  drawVisualizer();
    else if (right_pane_ == RightPane::TrackInfo)   drawTrackInfo();
    else if (right_pane_ == RightPane::Bookmarks)   drawBookmarks();
    else if (right_pane_ == RightPane::SearchResults) drawSearchResults();
    else if (right_pane_ == RightPane::Lyrics)      drawLyrics();
    else if (right_pane_ == RightPane::About)       drawAbout();
    else if (right_pane_ == RightPane::Devices)     drawDevices();
    else if (right_pane_ == RightPane::EQ)          drawEq();
    else if (right_pane_ == RightPane::Queue)        drawQueue();
    else if (right_pane_ == RightPane::Chapters)     drawChapters();
    else                                             drawHelp();
    // Awesome full-width Spectrum strip: its own region below the panes; the right
    // pane keeps whatever mode it's in. (Classic uses the RightPane::Visualizer
    // overlay handled by the dispatch above instead.)
    if (vizStripShown())
        drawVisualizer();
    drawProgress();
    drawCmdLine();
    wnoutrefresh(win_title_);
    wnoutrefresh(win_cwd_);
    wnoutrefresh(win_dir_);
    // Classic can swap the right pane for the visualizer overlay; Awesome always
    // shows the list pane there and the strip (below) separately.
    if (!config_.awesome_mode && right_pane_ == RightPane::Visualizer)
        wnoutrefresh(win_viz_);
    else
        wnoutrefresh(win_playlist_);
    if (vizStripShown())
        wnoutrefresh(win_viz_);
    wnoutrefresh(win_progress_);
    wnoutrefresh(win_cmdline_);
    doupdate();
}

void UIManager::drawRipConfirm() {   // slice 6: common (ncurses + portable CDSource)
    const int BOX_W = 68;
    // rip-format-select: the format block is data-driven, so the box and
    // everything below it grow with the table (2 rows -> 19, 3 -> 20, ...).
    const int BOX_H = 17 + kRipFormatCount;
    int y0 = (screen_rows_ - BOX_H) / 2;
    int x0 = (screen_cols_ - BOX_W) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;

    WINDOW* w = newwin(BOX_H, BOX_W, y0, x0);
    if (!w) return;
    // Match the panes' background so Awesome's frame colours and the wide-char
    // rounded corners actually render on this newwin - wingui needs the window bkgd
    // set for COLOR_PAIR() to take on a fresh window, and CP_DIM carries the themed
    // base fill (same wbkgd the panes get in createWindows). Classic: plain default.
    wbkgd(w, config_.awesome_mode ? COLOR_PAIR(CP_DIM) : COLOR_PAIR(0));
    werase(w);

    // Frame + title. Route the title through panelFrame so Awesome themes it (rounded
    // cyan frame + inset label); panelFrame draws no title in Classic, so keep the
    // manual centered one there.
    const char* title = " SECURE AUDIO EXTRACTION ";
    panelFrame(w, title, true);
    if (!config_.awesome_mode)
        mvwaddstr(w, 0, (BOX_W - (int)strlen(title)) / 2, title);

    // Drive / disc info
    const auto& cd = audio_.cdSource();
    int ntracks    = (int)cd.tracks().size();
    std::string out_dir;
    std::string album_str;
    std::string drive_model;
    int drive_offset = 0;
    {
        std::lock_guard<std::mutex> lk(mb_mutex_);
        out_dir      = CDRipper::buildOutputDir(mb_release_);
        album_str    = mb_release_.artist.empty() ? mb_release_.title
                     : mb_release_.artist + " - " + mb_release_.title;
    }
    drive_model  = audio_.cdSource().driveModel();
    drive_offset = audio_.cdSource().driveOffset();

    const int MAX_DIR = BOX_W - 8;
    std::string disp_dir = out_dir;
    if ((int)disp_dir.size() > MAX_DIR)
        disp_dir = "..." + disp_dir.substr(disp_dir.size() - (MAX_DIR - 3));

    mvwprintw(w, 2, 3, "Drive  %s\\   offset %+d samples",
              cd.driveLetter().c_str(), drive_offset);
    mvwprintw(w, 3, 3, "Disc   %d tracks", ntracks);

    // Album line + the live selection summary (or the deselect-all hint)
    // right-aligned on the same row. The album truncates to keep the
    // right-hand text intact (the MAX_DIR idiom above).
    const bool none_selected = rip_sel_.empty();
    std::string summary;
    if (none_selected) {
        summary = "select at least one format to rip";
    } else {
        summary = "Out: ";
        bool first = true;
        for (const auto& r : kRipFormats) {
            if (std::find(rip_sel_.begin(), rip_sel_.end(), r.id) == rip_sel_.end()) continue;
            if (!first) summary += " + ";
            summary += r.label;
            first = false;
        }
    }
    {
        int sum_x = BOX_W - (int)summary.size() - 3;
        int max_album = sum_x - 5;
        std::string a = album_str;
        if ((int)a.size() > max_album && max_album > 3)
            a = a.substr(0, (size_t)max_album - 3) + "...";
        if (!a.empty()) mvwprintw(w, 4, 3, "%s", a.c_str());
        if (none_selected) wattron(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        mvwaddstr(w, 4, sum_x, summary.c_str());
        if (none_selected) wattroff(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
    }

    // ── Output formats (digit-toggled; data-driven from kRipFormats) ──────
    mvwaddstr(w, 6, 3, "Output formats");
    {
        char hint[24];
        snprintf(hint, sizeof(hint), "(1-%d toggle)", kRipFormatCount);
        mvwaddstr(w, 6, BOX_W - (int)strlen(hint) - 3, hint);
    }
    for (int i = 0; i < kRipFormatCount; ++i) {
        const auto& r = kRipFormats[i];
        bool on = std::find(rip_sel_.begin(), rip_sel_.end(), r.id) != rip_sel_.end();
        // encoder-bitrate-mode: the ONE label home, fed the RIP config fields.
        // alt_mode = "is CBR" (MP3 mp3_cbr; Opus !opus_vbr; M4A !aac_vbr).
        bool alt = r.id == RipFormat::Mp3  ? config_.mp3_cbr
                 : r.id == RipFormat::Opus ? !config_.opus_vbr
                 : r.id == RipFormat::M4a  ? !config_.aac_vbr : false;
        std::string quality = encoderQualityLabel(
            r.id, alt, config_.mp3,
            r.id == RipFormat::Mp3 ? config_.mp3_cbr_bitrate
            : r.id == RipFormat::M4a ? config_.aac_cbr_bitrate : config_.opus_bitrate,
            config_.flac_level, config_.wavpack_mode, config_.aac_vbr_level);
        // Focus cursor (>) marks the row Left/Right/[M] edit. Marker and note are
        // two separate signals (lossless master; format limitation).
        const char* cur = (i == rip_focus_) ? "> " : "  ";
        mvwprintw(w, 7 + i, 3, "%s[%c] %d  %-8s %-14s %-3s%s",
                  cur, on ? 'x' : ' ', i + 1, r.label, quality.c_str(),
                  r.lossless ? "*" : "", r.note);
    }
    // Axis hint for the focused row (the blank line above the mode block); only
    // lossy rows have an editable axis, so FLAC/WAV/WavPack focus shows nothing.
    if (rip_focus_ >= 0 && rip_focus_ < kRipFormatCount) {
        const auto& fr = kRipFormats[rip_focus_];
        bool alt = fr.id == RipFormat::Mp3  ? config_.mp3_cbr
                 : fr.id == RipFormat::Opus ? !config_.opus_vbr
                 : fr.id == RipFormat::M4a  ? !config_.aac_vbr : false;
        if (const char* hint = encoderAxisHint(fr.id, alt))
            mvwaddstr(w, 7 + kRipFormatCount, 5, hint);
    }

    // Mode options — plain text; dimmed while nothing is selected (commit
    // keys are inert then; N stays live). Cancel row never dims. Rows below
    // the format block are table-size-relative.
    const int mode_y = 8 + kRipFormatCount;
    mvwaddstr(w, mode_y, 3, "Select ripping mode");
    struct { const char* key; const char* label; const char* desc; } opts[] = {
        { "[A]", "AccurateRip ", "Network CRC verify + offset correction" },
        { "[C]", "CUETools    ", "Disc-wide CRC32, no network required" },
        { "[Y]", "Local       ", "Best-effort rip, fast, no verification" },
        { "[B]", "Local 2-pass", "Best-effort + read-twice determinism check" },
        { "[N]", "Cancel      ", "Go back" },
    };
    for (int i = 0; i < 5; ++i) {
        bool dim = none_selected && i < 4;
        if (dim) wattron(w, A_DIM);
        mvwprintw(w, mode_y + 1 + i, 3, "%s %-12s  %s",
                  opts[i].key, opts[i].label, opts[i].desc);
        if (dim) wattroff(w, A_DIM);
    }

    // Footer divider + output path
    mvwhline(w, mode_y + 6, 1, ACS_HLINE, BOX_W - 2);
    mvwprintw(w, mode_y + 7, 3, "Out  %s", disp_dir.c_str());

    wrefresh(w);
    delwin(w);
}

// Stream-record R2: the [Rec] panel (^E). The rip modal's window mechanics
// cloned (centered newwin, themed bkgd, panelFrame + Classic manual title,
// MAX_DIR truncation); rip-specific rows dropped. Two views on one overlay:
// SETTINGS while idle, live STATE while recording (^E reopens to it). Every
// lifecycle datum is read from the recorder's atomic accessors per render —
// no UI-side copy exists to drift.
void UIManager::drawRecPanel() {
    const int BOX_W = 68;
    const int BOX_H = 23;   // split-trim [T] + ad-aware [A] + the M4A and Copy rows
                            // (slice B) and its no-RG note
    int y0 = (screen_rows_ - BOX_H) / 2;
    int x0 = (screen_cols_ - BOX_W) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;

    WINDOW* w = newwin(BOX_H, BOX_W, y0, x0);
    if (!w) return;
    wbkgd(w, config_.awesome_mode ? COLOR_PAIR(CP_DIM) : COLOR_PAIR(0));
    werase(w);

    const char* title = " STREAM RECORDING ";
    panelFrame(w, title, true);
    if (!config_.awesome_mode)
        mvwaddstr(w, 0, (BOX_W - (int)strlen(title)) / 2, title);

    auto& rec = audio_.streamRecorder();

    // Station display name (the "RADIO: " prefix is a pane affordance, not a name)
    std::string st = stationLabel(audio_.streamUrl());
    if (st.rfind("RADIO: ", 0) == 0) st = st.substr(7);

    // Effective output dir: the recorder appends <Station>/ itself, so show
    // the resolved station dir the cuts will actually land in.
    const std::string base = rec_dir_.empty() ? CDRipper::recordingsDir() : rec_dir_;
#ifdef _WIN32
    const std::string dir  = base + "\\" + sanitizePathComponent(st);
#else
    const std::string dir  = base + "/" + sanitizePathComponent(st);
#endif
    // Both fields' content starts at column 12 (col 3 + the 9-char prefix); the
    // last interior column is BOX_W - 2, so the budget from col 12 is BOX_W - 12 - 1.
    const int FIELD_W = BOX_W - 12 - 1;
    std::string disp_dir = dir;
    if ((int)disp_dir.size() > FIELD_W)
        disp_dir = "..." + disp_dir.substr(disp_dir.size() - (FIELD_W - 3));

    // Station marquees through the shared scrollToWidth tick (pads if it fits,
    // scrolls if it does not) so a long station name stays inside the border. Out
    // keeps the head-truncation idiom (the meaningful tail of a path) at the
    // corrected budget - a scrolling path in a settings modal is noise.
    mvwprintw(w, 2, 3, "Station  %s", scrollToWidth(st, FIELD_W, text_scroll_offset_).c_str());
    mvwprintw(w, 3, 3, "Out      %s", disp_dir.c_str());

    if (!rec.recording()) {
        // ── SETTINGS view ────────────────────────────────────────────────
        mvwaddstr(w, 5, 3, "Format");
        mvwaddstr(w, 5, BOX_W - 15, "(1-4 select)");
        // encoder-bitrate-mode: the shared label home, fed the REC fields
        // (rec_*), independent of the rip knobs. alt_mode = "is CBR".
        struct { RipFormat id; const char* label; std::string quality; } rows[] = {
            { RipFormat::Opus, "Opus",
              encoderQualityLabel(RipFormat::Opus, !config_.rec_opus_vbr,
                                  config_.rec_mp3, config_.rec_opus_bitrate) },
            { RipFormat::Mp3,  "MP3",
              encoderQualityLabel(RipFormat::Mp3, config_.rec_mp3_cbr,
                                  config_.rec_mp3, config_.rec_mp3_cbr_bitrate) },
            { RipFormat::M4a,  "M4A",
              encoderQualityLabel(RipFormat::M4a, !config_.rec_aac_vbr,
                                  config_.rec_mp3, config_.rec_aac_cbr_bitrate,
                                  5, "normal", config_.rec_aac_vbr_level) },
        };
        for (int i = 0; i < 3; ++i) {
            const char* cur = (i == rec_focus_) ? "> " : "  ";
            mvwprintw(w, 6 + i, 3, "%s(%c) %d  %-6s %s",
                      cur, (!rec_copy_ && rec_fmt_ == rows[i].id) ? '*' : ' ', i + 1,
                      rows[i].label, rows[i].quality.c_str());
        }
        // Copy row (abi-cluster slice B): as-broadcast capture. Quality
        // column = the LIVE codec from the plugin's encoded_caps; greyed
        // with a dim reason when the plugin lacks the tee (old plugin).
        {
            const bool copy_ok = audio_.recordingCopySupported();
            std::string q;
            if (!copy_ok) {
                q = "unavailable (plugin lacks copy)";
            } else {
                int32_t caps = audio_.streamEncodedCaps();
                q = caps == 2 ? "AAC as broadcast (no re-encode)"
                  : caps == 1 ? "MP3 as broadcast (no re-encode)"
                              : "as broadcast (no re-encode)";
            }
            if (!copy_ok) wattron(w, A_DIM);
            mvwprintw(w, 9, 3, "%s(%c) 4  %-6s %s",
                      (rec_focus_ == 3) ? "> " : "  ",
                      rec_copy_ ? '*' : ' ', "Copy", q.c_str());
            if (!copy_ok) wattroff(w, A_DIM);
        }
        // Axis hint for the focused rec row (encoder-bitrate-mode); the Copy row
        // has no quality axis, so it shows nothing (like FLAC/WAV in the rip modal).
        {
            RipFormat ff = rec_focus_ == 1 ? RipFormat::Mp3
                         : rec_focus_ == 0 ? RipFormat::Opus
                         : rec_focus_ == 2 ? RipFormat::M4a : RipFormat::Wav;
            bool alt = ff == RipFormat::Mp3  ? config_.rec_mp3_cbr
                     : ff == RipFormat::Opus ? !config_.rec_opus_vbr
                     : ff == RipFormat::M4a  ? !config_.rec_aac_vbr : false;
            if (const char* hint = encoderAxisHint(ff, alt))
                mvwaddstr(w, 14, 5, hint);
        }
        mvwprintw(w, 10, 3, "[S] Split on track change : %s",
                  rec_split_on_ ? "ON" : "OFF (one continuous file)");
        mvwprintw(w, 11, 3, "[T] Split hold : %d ms", rec_offset_ms_);
        mvwprintw(w, 12, 3, "[A] Ad segments : %s",
                  rec_ads_discard_ ? "DISCARD (not written)" : "Save (to ads/ folder)");
        mvwaddstr(w, 13, 3, "[D] Output dir (edit)");

        // The pause note — driven by the plugin's keep-draining capability
        // (abi-cluster slice A): with a capable plugin, pausing mutes playback
        // while the recording continues gaplessly; with an old plugin the R1
        // pause-gap truth stays. Plus the Discard cost AT THE TOGGLE and the
        // copy trade AT THE SELECTION.
        wattron(w, A_DIM);
        if (audio_.recordingDrainSupported()) {
            mvwaddstr(w, 15, 3, "Note: pausing mutes playback - the recording");
            mvwaddstr(w, 16, 3, "continues (you rejoin the live broadcast on resume).");
        } else {
            mvwaddstr(w, 15, 3, "Note: pausing playback while recording leaves a");
            mvwaddstr(w, 16, 3, "silence gap - the paused-over airtime is not captured.");
        }
        mvwaddstr(w, 17, 3, "Cuts split on station metadata (boundaries are +/-1-2s).");
        if (rec_copy_)
            mvwaddstr(w, 18, 3, "Copy keeps the broadcast bytes exactly - no ReplayGain tags.");
        if (rec_ads_discard_)
            mvwaddstr(w, 19, 3, "Discard trusts station metadata - a mislabeled song can be lost.");
        wattroff(w, A_DIM);

        mvwhline(w, BOX_H - 3, 1, ACS_HLINE, BOX_W - 2);
        mvwaddstr(w, BOX_H - 2, 3, "[R] Record        [N/Esc] Close");
    } else {
        // ── RECORDING view (live state, all atomic reads) ───────────────
        std::string np = audio_.streamNowPlaying();
        if ((int)np.size() > BOX_W - 14) np = np.substr(0, BOX_W - 17) + "...";
        wattron(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        mvwaddstr(w, 5, 3, "* REC");
        wattroff(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        mvwprintw(w, 5, 10, "%s", np.c_str());

        int es = rec.elapsedSec();
        mvwprintw(w, 7, 3, "elapsed %d:%02d    cuts %d    written %.1f MB",
                  es / 60, es % 60, rec.cutIndex(),
                  (double)rec.bytesWritten() / (1024.0 * 1024.0));
        // ad-aware: the skip counter is the trust surface for Discard — the
        // only evidence the mode is working, and the tell when it over-fires.
        if (int skipped = rec.adsSkipped(); skipped > 0)
            mvwprintw(w, 6, 3, "ads skipped: %d (Discard is on)", skipped);
        uint64_t dropped = rec.droppedFrames();
        if (dropped > 0) {
            wattron(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
            mvwprintw(w, 8, 3, "dropped frames: %llu", (unsigned long long)dropped);
            wattroff(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        }
        std::string err = rec.lastError();
        if (!err.empty()) {
            wattron(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
            mvwprintw(w, 9, 3, "error: %s", err.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        }

        mvwhline(w, BOX_H - 3, 1, ACS_HLINE, BOX_W - 2);
        mvwaddstr(w, BOX_H - 2, 3, "[X] Stop          [N/Esc] Close (keeps recording)");
    }

    wrefresh(w);
    delwin(w);
}


// Apply MusicBrainz/Discogs track titles to the current CD playlist. UI THREAD
// ONLY: this reads and mutates playlist_ (its entries_ vector and the std::strings
// inside it), which has no internal locking, so it must never run on a worker
// thread. Worker callbacks cache the release and raise mb_titles_pending_; the run
// loop drains that flag and calls this with a snapshot taken under mb_mutex_.
void UIManager::applyReleaseTitles(const MBRelease& rel) {
    // Multi-disc: scope titles to the disc currently in the drive.
    int n_phys_md = 0;
    for (std::size_t k = 0; k < playlist_.size(); ++k)
        if (isCDTrackPath(playlist_.at(k).path)) ++n_phys_md;
    const int cur_disc = pickDiscForTrackCount(rel, n_phys_md);
    for (std::size_t i = 0; i < playlist_.size(); ++i) {
        int tnum = cdTrackNumber(playlist_.at(i).path);
        if (tnum < 0) continue;
        for (const auto& mt : rel.tracks) {
            if (mt.number == tnum && mt.disc == cur_disc && !mt.title.empty()) {
                std::string dt = mt.artist.empty()
                    ? mt.title : mt.artist + " - " + mt.title;
                playlist_.setDisplayTitle(i, sanitizeForDisplay(dt));
                break;
            }
        }
    }
}


void UIManager::refreshChaptersIfNeeded(const std::string& path) {
    if (path == chapters_for_path_) return;        // already reflects this path
    chapters_for_path_ = path;
    current_chapters_.clear();
    if (path.empty()) return;
    // Any MP4-family container can carry chapters: .m4b books and chaptered
    // .m4a/.mp4 podcasts. parseMp4Chapters returns empty for everything else,
    // so non-chaptered files simply show no chapter.
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".m4b" || ext == ".m4a" || ext == ".mp4" || ext == ".m4v")
        current_chapters_ = parseMp4Chapters(path);
}

int UIManager::currentChapterIndex() const {
    if (current_chapters_.empty()) return -1;
    double pos = audio_.positionSec();
    int idx = 0;
    for (size_t i = 0; i < current_chapters_.size(); ++i) {
        if (current_chapters_[i].start_sec <= pos + 0.05) idx = (int)i;
        else break;
    }
    return idx;
}

void UIManager::jumpChapter(int dir) {
    // ,/. jump by the PLAYING track's chapters. current_chapters_ may hold a
    // browsed (not playing) book's list while the Chapters pane is open; foreign
    // chapter boundaries must not seek the playing file, so treat that - and a
    // genuinely unchaptered file - as a plain +/-5s seek.
    if (current_chapters_.empty() ||
        chapters_for_path_ != audio_.currentTrack().path) {
        audio_.seekBy(dir < 0 ? -5.0 : 5.0);
        redraw_needed_.store(true);
        return;
    }
    int ci = currentChapterIndex();
    if (ci < 0) ci = 0;
    if (dir > 0) {
        if (ci + 1 < (int)current_chapters_.size())
            audio_.seekTo(current_chapters_[(size_t)ci + 1].start_sec);
        // else already in the last chapter -> stay put
    } else {
        double curStart = current_chapters_[(size_t)ci].start_sec;
        double pos      = audio_.positionSec();
        if (ci == 0 || pos - curStart > 3.0)
            audio_.seekTo(curStart);                                       // restart current
        else
            audio_.seekTo(current_chapters_[(size_t)ci - 1].start_sec);    // previous
    }
    redraw_needed_.store(true);
}

void UIManager::flushBookProgress() {
    if (book_progress_path_.empty()) return;
    double pos = book_progress_pos_;
    // Near the start -> treat as not-started; within 15s of the end -> finished,
    // so a replay begins fresh. Otherwise store the resume point.
    if (pos < 5.0 || (book_progress_dur_ > 0.0 && pos > book_progress_dur_ - 15.0))
        config_.setBookPos(book_progress_path_, 0.0);
    else
        config_.setBookPos(book_progress_path_, pos);
    config_.save();
}

void UIManager::updateBookProgress() {
    const auto& tr = audio_.currentTrack();
    bool playing = (audio_.state() != PlaybackState::Stopped);
    bool isBook  = playing && !tr.path.empty() && PlaylistManager::isAudiobook(tr.path);
    std::string active = isBook ? tr.path : std::string();

    if (active != book_progress_path_) {           // transition: flush old, latch new
        flushBookProgress();
        book_progress_path_  = active;
        book_progress_dur_   = isBook ? (double)tr.duration_sec : 0.0;
        book_resume_pending_ = isBook;             // one-shot resume on the new book
    }
    if (isBook) {
        if (book_resume_pending_) {
            double saved = config_.bookPos(tr.path);
            double pos   = audio_.positionSec();
            if (saved <= 1.0)      book_resume_pending_ = false;            // nothing saved
            else if (pos < 2.0)  { audio_.seekTo(saved); book_resume_pending_ = false; }
            else if (pos > 3.0)    book_resume_pending_ = false;            // already moved on
            // else 2..3s: wait a tick for the decoder to settle
        }
        book_progress_pos_ = audio_.positionSec();
        if (tr.duration_sec > 0) book_progress_dur_ = (double)tr.duration_sec;
    }
}

void UIManager::drawTitleBar() {
    werase(win_title_);
    wattron(win_title_, COLOR_PAIR(CP_TITLE) | A_BOLD);
    const auto& track = audio_.currentTrack();
    std::string np;
    if (audio_.cdMode() && audio_.cdCurrentTrack() > 0) {
        int t = audio_.cdCurrentTrack();
        // Check if MB has populated the track name in the playlist
        std::string cd_title;
        for (std::size_t i = 0; i < playlist_.size(); ++i) {
            const auto& e = playlist_.at(i);
            if (cdTrackNumber(e.path) == t) { cd_title = e.display_title; break; }
        }
        // Fall back to generic "CD Track 02 [F:]" if no MB title yet
        if (cd_title.empty() || isCDTrackPath(cd_title))
            np = "CD Track " + (t < 10 ? std::string("0") : "") + std::to_string(t)
               + " [" + cd_drive_letter_ + ":]";
        else
            np = cd_title + " [" + cd_drive_letter_ + ":]";
    } else
    if (audio_.streamMode()) {
        // Live stream: show the station identity (its RADIO: label) so the top
        // line reflects the playing station, not the stale last-file track held
        // in currentTrack(). The song itself (ICY/HLS now-playing) stays on the
        // bottom progress row. Derive the label from the URL actually streaming
        // (stationLabel -> config name or derived), NOT playlist_.current(): a
        // station launched from the play queue never moves the playlist cursor
        // (queue items have no playlist row), so current() still points at the
        // previous song and would paint its stale title here. URL-based labeling
        // is cursor-independent and still upgrades when a user-supplied station
        // name lands in config. (Common since the top-bar radio gate came off -
        // stationLabel/streamUrl are portable.)
        std::string label = stationLabel(audio_.streamUrl());
        np = label.empty() ? "(live stream)" : label;
    } else
    if (audio_.state() != PlaybackState::Stopped && !track.path.empty()) {
        np = track.artist.empty() ? track.title : track.artist + " - " + track.title;
        if (np.empty()) np = fs::path(track.path).filename().string();
        // Don't clobber a manually-browsed chapter list (a highlighted, not-playing
        // book) while the Chapters pane is open - this draw runs every tick, so an
        // ungated refresh would replace the browsed list on the next frame. Re-syncs
        // to the playing track the moment the pane closes; when the pane shows the
        // playing book anyway, the same-path refresh is a no-op.
        if (right_pane_ != RightPane::Chapters || chapters_for_path_ == track.path)
            refreshChaptersIfNeeded(track.path);
        // The chapter suffix trusts current_chapters_ only when it belongs to the
        // playing track (it may hold a browsed book's list while the pane is open).
        if (chapters_for_path_ == track.path) {
            int ci = currentChapterIndex();
            if (ci >= 0) np += "  |  " + current_chapters_[(size_t)ci].title;
        }
    }
    std::string badge;
    if (audio_.cdMode()) {
        badge = audio_.cdSource().paused() ? "| " : (audio_.cdCurrentTrack() > 0 ? "> " : "  ");
    } else
    switch (audio_.state()) {
        case PlaybackState::Playing: badge = "> "; break;
        case PlaybackState::Paused:  badge = "| "; break;
        case PlaybackState::Stopped: badge = "  "; break;
    }
    // Build MOC-style mode indicators — only show active modes
    std::string modes;
    switch (playlist_.repeatMode()) {
        case RepeatMode::One: modes += " [REPEAT ONE]"; break;
        case RepeatMode::All: modes += " [REPEAT ALL]"; break;
        default: break;
    }
    if (playlist_.shuffle()) modes += " [SHUFFLE]";
    // Stream-record R2: DERIVED from the recorder's own state (one relaxed
    // atomic) — no stored UI flag exists, so a stale badge is impossible:
    // every engine teardown path (stop/station switch/file/CD/quit) already
    // clears recording() via the R1 hooks.
    if (audio_.streamRecorder().recording()) modes += " [REC]";
    if (!playlist_.queueEmpty())
        modes += " [Q:" + std::to_string(playlist_.queueSize()) + "]";
    if (config_.toast_enabled) modes += " [TOAST]";
    float spd = audio_.speed();
    if (std::abs(spd - 1.0f) > 0.005f) {
        char spdbuf[16];
        int pct = (int)std::round((spd - 1.0f) * 100.0f);
        std::snprintf(spdbuf, sizeof(spdbuf), " [%+d%%]", pct);
        modes += spdbuf;
    }
    if (audio_.muted())      modes += " [MUTE]";
    // Slice 3: the [CD] tag and MB album track "a disc is loaded" (cd_drive_letter_),
    // not "CD is the active source" (cdMode). A stopped-but-loaded disc keeps its
    // tag + album so stop() no longer visibly forgets the disc.
    if (!cd_drive_letter_.empty()) modes += " [CD]";
    if (mb_fetching_.load())  modes += " [MB...]";
    else {
        std::lock_guard<std::mutex> lk(mb_mutex_);
        if (!mb_album_.empty() && !cd_drive_letter_.empty())
            modes += " [" + mb_album_ + "]";
    }
    if (sleep_minutes_ > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - sleep_start_).count();
        int remaining = sleep_minutes_ - (int)elapsed;
        char sstr[24];
        std::snprintf(sstr, sizeof(sstr), " [SLEEP %dm]", remaining);
        modes += sstr;
    }
    float bal = audio_.balance();
    if (std::abs(bal) > 0.05f) {
        char bstr[16];
        std::snprintf(bstr, sizeof(bstr), " [BAL %+.0f%%]", bal * 100.0f);
        modes += bstr;
    }
    if (audio_.replayGain()) modes += " [RG]";
    if (audio_.eqEnabled())  modes += " [EQ]";
    // Wall clock
    if (show_clock_) {
        std::time_t now = std::time(nullptr);
        std::tm tmbuf{}; localtimeSafe(now, tmbuf); std::tm* tm = &tmbuf;
        char clk[12];
        std::strftime(clk, sizeof(clk), " %H:%M:%S", tm);
        modes += clk;
    }
    std::string right = modes + " RE-MOCT " + kVersionTag + " ";

    int max_np = screen_cols_ - (int)right.size() - (int)badge.size() - 2;
    std::string line = " " + badge;

    if (!np.empty() && max_np > 0) {
        if (dispWidth(np) <= max_np) {
            // Fits — no scroll needed, reset state
            if (marquee_last_path_ != track.path) {
                marquee_offset_ = 0;
                marquee_ticks_  = 0;
                marquee_last_path_ = track.path;
            }
            line += np;
        } else {
            // Doesn't fit — marquee scroll
            // Reset on track change
            if (marquee_last_path_ != track.path) {
                marquee_offset_ = 0;
                marquee_ticks_  = 0;
                marquee_last_path_ = track.path;
            }

            // Build scrollable string with padding at each end
            // Column-aware: marquee_offset_ counts codepoints, the visible window is
            // measured in display columns (scrollToWidth uses the same np + 3-space
            // gap), and the line is drawn via the wide API below. The pause/speed
            // timing state machine is unchanged.
            const int cyc = cpCount(np) + 3;   // np + 3-space gap, in codepoints
            if (marquee_offset_ > cyc) marquee_offset_ = 0;

            line += scrollToWidth(np, max_np, marquee_offset_);

            // Advance scroll offset on timer
            ++marquee_ticks_;
            int pause = (marquee_offset_ == 0) ? MARQUEE_PAUSE * 2 : MARQUEE_PAUSE;
            if (marquee_ticks_ >= pause + MARQUEE_SPEED) {
                marquee_ticks_ = MARQUEE_PAUSE;
                ++marquee_offset_;
                if (marquee_offset_ >= cyc) marquee_offset_ = 0;
            }
        }
    } else {
        if (marquee_last_path_ != track.path) {
            marquee_offset_ = 0; marquee_ticks_ = 0;
            marquee_last_path_ = track.path;
        }
        line += "RE-MOCT - Music On Console Terminal";
    }
    // Pad in display columns so the right-aligned status sits at the true edge
    // regardless of any multibyte glyphs in the title.
    {
        int target = screen_cols_ - dispWidth(right);
        int cur    = dispWidth(line);
        if (cur < target) line.append((size_t)(target - cur), ' ');
    }
    line += right;
    std::wstring wtitle = utf8_to_wide(padToWidth(line, screen_cols_));
    mvwaddnwstr(win_title_, 0, 0, wtitle.c_str(), (int)wtitle.size());
    wattroff(win_title_, COLOR_PAIR(CP_TITLE) | A_BOLD);

    // [REC] pulse: overpaint the badge in slow-pulsing red (phase advanced by
    // the tick loop ~every 0.64s while recording). Column math: `right` is
    // right-aligned at the true edge, and everything BEFORE [REC] in it is
    // ASCII (repeat/shuffle tags), so byte offset == columns for the prefix;
    // dispWidth handles any UTF-8 (MB album) that follows. ASCII overpaint,
    // both curses builds, both themes (CP_STATUS_ERR is the red pair).
    if (audio_.streamRecorder().recording()) {
        size_t p = right.find(" [REC]");
        if (p != std::string::npos) {
            int col = screen_cols_ - dispWidth(right) + (int)p + 1;
            if (col >= 0 && col + 5 <= screen_cols_) {
                static const attr_t kPulse[4] = { A_BOLD, A_NORMAL, A_DIM, A_NORMAL };
                attr_t a = COLOR_PAIR(CP_STATUS_ERR) | kPulse[rec_pulse_ & 3];
                wattron(win_title_, a);
                mvwaddstr(win_title_, 0, col, "[REC]");
                wattroff(win_title_, a);
            }
        }
    }
}

void UIManager::drawCwd() {
    werase(win_cwd_);

    // Show full path, right-aligned truncation if too long
    // Format: " >> /full/path/to/current/directory "
    std::string path = in_drive_list_ ? "[Drives]" : current_dir_;

    // On a theme switch (Ctrl+T / F8) show a [THEME:<name>] tag right-aligned here,
    // directly below the RE-MOCT version in the title bar - then fade it out after
    // ~10s (bold, then dim, then gone). Replaces the transient theme toast.
    std::string theme_tag;
    bool        theme_tag_dim = false;
    if (theme_tag_ticks_ < kThemeTagGoneTick) {
        theme_tag = config_.awesome_mode
            ? std::string(" [THEME:") + kAwesomeThemes[config_.awesome_theme].name + "] "
            : std::string(" [THEME:Classic] ");
        theme_tag_dim = (theme_tag_ticks_ >= kThemeTagFadeTick);
    }
    const int tag_w = (int)dispWidth(theme_tag);

    const int cwx   = config_.awesome_mode ? 1 : 0;   // align with inset pane frames
    const int avail = screen_cols_ - cwx - tag_w;     // width left for the path line

    // If path is wider than the available width, show only the tail
    // Keep a leading "<<" indicator to show it's truncated
    const int maxw = avail - 4;  // 4 = " >> " prefix
    std::string display = path;
    std::string prefix  = " >> ";
    if (dispWidth(display) > maxw) {
        display = truncateToWidthRight(display, maxw);   // column-aware tail
        prefix  = " << ";  // indicate left side is cut
    }

    std::string line = prefix + display;
    std::wstring wcwd = utf8_to_wide(padToWidth(line, avail));

    wattron(win_cwd_, COLOR_PAIR(CP_DIM));
    mvwaddnwstr(win_cwd_, 0, cwx, wcwd.c_str(), (int)wcwd.size());
    wattroff(win_cwd_, COLOR_PAIR(CP_DIM));

    if (tag_w > 0) {
        std::wstring wtag = utf8_to_wide(theme_tag);
        const attr_t a = COLOR_PAIR(CP_TITLE) | (theme_tag_dim ? A_DIM : A_BOLD);
        wattron(win_cwd_, a);
        mvwaddnwstr(win_cwd_, 0, screen_cols_ - tag_w, wtag.c_str(), (int)wtag.size());
        wattroff(win_cwd_, a);
    }
}

void UIManager::drawDirBrowser() {
    werase(win_dir_);
    int rows, cols;
    getmaxyx(win_dir_, rows, cols);
    (void)rows;
    bool focused = (focus_ == Pane::DirBrowser);
    const bool aw = config_.awesome_mode;
    const int  cx = aw ? 1 : 0;             // content left column (inside frame)
    const int  cw = aw ? cols - 2 : cols;   // content width
    std::string hdr;
    if (in_drive_list_)   hdr = " [Drives] (Enter:open  F12:refresh  E:eject) ";
    else if (in_recent_)  hdr = " [Recently Played] ";
    else if (in_favs_)    hdr = " [FAVs] (f:fav/unfav  Enter:play  Del:remove) ";
    else if (in_radio_)   hdr = " [Radio] (Enter:play  d/Del:remove) ";
    else if (in_books_)   hdr = " [Books] (Enter:play  Del:remove) ";
    else {
        std::string leaf = fs::path(current_dir_).filename().string();
        if (leaf.empty()) leaf = current_dir_;
        const char* sortname = (browser_sort_ == BrowserSort::Modified) ? " modified"
                             : (browser_sort_ == BrowserSort::Size)     ? " size"
                             : "";
        hdr = " Dir: " + leaf + sortname + (show_hidden_ ? " [.hidden]" : "") + " ";
    }
    // convert-core: a visible marked count (any browser mode).
    if (!marked_.empty())
        hdr += "[" + std::to_string((int)marked_.size()) + " marked] ";
    if (!aw) {   // Classic: full-width coloured header bar on row 0
        wattron(win_dir_, focused ? (COLOR_PAIR(CP_FOCUSED)|A_BOLD)
                                  : (COLOR_PAIR(CP_BORDER)|A_BOLD));
        std::string bar = hdr; bar.resize((size_t)cols, ' ');
        mvwaddnstr(win_dir_, 0, 0, bar.c_str(), cols);
        wattroff(win_dir_, A_BOLD|COLOR_PAIR(CP_FOCUSED)|COLOR_PAIR(CP_BORDER));
    }
    int visible = paneVisibleRows(win_dir_);
    for (int i = 0; i < visible; ++i) {
        int idx = dir_scroll_ + i;
        if (idx >= (int)dir_entries_.size()) break;
        const auto& name    = dir_entries_[(size_t)idx];
        std::string display = (idx < (int)dir_display_.size())
                            ? dir_display_[(size_t)idx] : name;
        bool cursor = (idx == dir_cursor_);
        // Inline eject hint: highlighted row only, only for a CD drive with media
        // (cached at enumeration - see enterDriveList). Rows go through the wide
        // path (utf8_to_wide -> mvwaddnwstr), so the eject glyph renders for real.
        if (in_drive_list_ && cursor && driveHasMedia(name))
            display += "  ⏏ Shift+E";
        bool is_dir = in_drive_list_
                    || name == ".." || name == "[Drives]" || name == "[Recent]"
                    || name == "[FAVs]" || name == "[Bookmarks]" || name == "[Back]"
                    || name == "[Radio]" || name == "[Books]"
                    || (!in_recent_ && !in_favs_ && !in_radio_ && !in_books_ && fs::is_directory(fs::path(current_dir_) / name));

        // Dead path check for FAVs — grey out missing files
        bool dead_path = false;
        if ((in_favs_ || in_books_) && name != "[Back]" && !name.empty()) {
            dead_path = !fs::exists(name);
        }

        short  rpair = CP_DIM; attr_t rattr = A_BOLD;
        if      (cursor && focused) { rpair = CP_SELECTED; rattr = A_BOLD; }
        else if (cursor)            { rpair = CP_SELECTED_UNFOCUSED; rattr = A_BOLD; }
        else if (dead_path)         { rpair = CP_DIM;      rattr = 0; }
        else if (is_dir)            { rpair = CP_TITLE;    rattr = A_BOLD; }
        else                        { rpair = CP_DIM;      rattr = A_BOLD; }
        wattron(win_dir_, COLOR_PAIR(rpair) | rattr);

        // convert-core: is this row a marked file? (path-keyed; skip the lookup
        // entirely when nothing is marked - the common case).
        bool marked = false;
        if (!marked_.empty() && !is_dir) {
            std::string ep = browserEntryPath(idx);
            marked = !ep.empty() && marked_.contains(ep);
        }
        const bool ico = config_.nerd_icons;
        // With icons on, reserve the prefix cell (the glyph replaces the "+"/space).
        // A marked file shows "* " (non-icon) or a star glyph (icon mode).
        std::string prefix = ico ? "  " : (is_dir ? "+ " : (marked ? "* " : "  "));
        int avail = cw - 1;
        std::string d;
        if (ico) {
            // Keep the icon cell fixed: marquee only the name, not the prefix, so
            // a long scrolling filename never slides under the overlaid glyph.
            int name_w = avail - (int)prefix.size();
            std::string nm = (name_w > 0) ? scrollToWidth(display, name_w, text_scroll_offset_) : "";
            d = prefix + nm;
        } else {
            d = scrollToWidth(prefix + display, avail, text_scroll_offset_);
        }
        std::wstring wd = utf8_to_wide(padToWidth(d, cw));
        mvwaddnwstr(win_dir_, i+1, cx, wd.c_str(), (int)wd.size());
        if (ico) {   // overlay glyph on the reserved cell (wide API → real glyph)
            cchar_t cc;
            wchar_t s[2] = { is_dir ? L'\uf07b' : (marked ? L'\uf005' : L'\uf001'), 0 };  // folder / star / music
            setcchar(&cc, s, rattr, rpair, nullptr);
            mvwadd_wch(win_dir_, i+1, cx, &cc);
        }

        wattroff(win_dir_, A_BOLD|A_REVERSE|COLOR_PAIR(CP_SELECTED)
                         |COLOR_PAIR(CP_SELECTED_UNFOCUSED)
                         |COLOR_PAIR(CP_TITLE)|COLOR_PAIR(CP_DIM));
    }
    if (aw) {
        panelFrame(win_dir_, hdr, focused, L'\uf07b');
    } else if (!focused) {
        wattron(win_dir_, COLOR_PAIR(CP_BORDER));
        box(win_dir_, 0, 0);
        wattroff(win_dir_, COLOR_PAIR(CP_BORDER));
    }
}

std::optional<std::size_t> UIManager::nowPlayingRow() const {
    if (audio_.state() == PlaybackState::Stopped) return std::nullopt;
    // CD mode: currentTrack() carries no CD identity (openCD clears it), so map
    // the playing track number to its playlist row directly - the same
    // cdTrackNumber(path) match drawTitleBar uses for the CD title. This is what
    // lets the lit row and the F3 follow-sync track CD playback (including CD
    // auto-advance and file->CD returns) like every other source.
    if (audio_.cdMode()) {
        int t = audio_.cdCurrentTrack();
        if (t <= 0) return std::nullopt;
        for (std::size_t i = 0; i < playlist_.size(); ++i)
            if (cdTrackNumber(playlist_.at(i).path) == t) return i;
        return std::nullopt;   // queue-launched CD track: no row, lights nothing
    }
    const bool stream = audio_.streamMode();
    const std::string want = stream ? audio_.streamUrl()
                                    : audio_.currentTrack().path;
    if (want.empty()) return std::nullopt;
    for (std::size_t i = 0; i < playlist_.size(); ++i)
        if (playlist_.at(i).path == want) return i;
    return std::nullopt;   // queue-launched station: no row, lights nothing
}

const TrackInfo* UIManager::nowPlayingTrack() const {
    if (audio_.streamMode()) return nullptr;
    if (audio_.state() == PlaybackState::Stopped) return nullptr;
    return &audio_.currentTrack();
}

void UIManager::ensurePlaylistCursorVisible() {
    const int n = (int)playlist_.size();
    if (n == 0) { pl_cursor_ = 0; pl_scroll_ = 0; return; }
    pl_cursor_ = std::clamp(pl_cursor_, 0, n - 1);
    const int visible = win_playlist_ ? paneVisibleRows(win_playlist_) : 0;
    if (visible <= 0) return;                       // pane not built yet
    if (pl_cursor_ < pl_scroll_)                    pl_scroll_ = pl_cursor_;
    else if (pl_cursor_ >= pl_scroll_ + visible)    pl_scroll_ = pl_cursor_ - visible + 1;
    pl_scroll_ = std::clamp(pl_scroll_, 0, std::max(0, n - visible));
}

// Short uppercase type tag for the optional F11 filetype column (MOC parity).
// "" for non-files (CD tracks, streams) and unrecognised extensions - a blank
// tag also zeroes the column width for that row, so the title reclaims the
// space (see the ftw math in drawPlaylist).
static std::string fileTypeTag(const std::string& path) {
    if (isCDTrackPath(path) || isStreamPath(path)) return "";
    std::string ext = fs::path(path).extension().string();
    if (ext.size() < 2) return "";
    ext.erase(0, 1);                                     // drop the dot
    for (char& c : ext) c = (char)std::toupper((unsigned char)c);
    if (ext == "AIF") ext = "AIFF";
    if (ext == "MP4") ext = "M4A";
    static const char* known[] = { "FLAC", "MP3", "OGG", "OPUS", "WAV",
                                   "AIFF", "M4A", "M4B", "WMA", "AAC", "WV" };
    for (const char* k : known)
        if (ext == k) return ext;
    return "";   // unknown extension: no tag (safer than guessing)
}

void UIManager::drawPlaylist() {
    werase(win_playlist_);
    int rows, cols;
    getmaxyx(win_playlist_, rows, cols);
    (void)rows;
    bool focused = (focus_ == Pane::Playlist);
    // Total playlist duration
    int total_secs = 0;
    for (std::size_t ii = 0; ii < playlist_.size(); ++ii)
        total_secs += playlist_.at(ii).duration_sec;
    std::string total_str;
    if (total_secs > 0) {
        int th = total_secs / 3600;
        int tm = (total_secs % 3600) / 60;
        int ts = total_secs % 60;
        if (th > 0)
            total_str = std::to_string(th) + "h " + std::to_string(tm) + "m";
        else
            total_str = std::to_string(tm) + "m " + std::to_string(ts) + "s";
    }

    const bool aw = config_.awesome_mode;
    const int  cx = aw ? 1 : 0;
    const int  cw = aw ? cols - 2 : cols;
    // [cursor/total], 1-based - "where am I" on a long list. [0] when empty
    // (never [1/0]); cursor clamped so a stale value can't print past the size.
    std::string hdr = " Playlist [";
    if (playlist_.size() == 0) {
        hdr += "0]";
    } else {
        hdr += std::to_string(std::min(pl_cursor_ + 1, (int)playlist_.size()))
             + "/" + std::to_string(playlist_.size()) + "]";
    }
    if (playlist_.isLoading()) {
        hdr += "  [loading " + std::to_string(playlist_.pendingCount()) + "...]";
    } else if (!total_str.empty()) {
        hdr += "  " + total_str;
    }
    const char* slbl = playlist_.sortLabel();
    if (slbl && slbl[0]) hdr += std::string("  sort:") + slbl;
    hdr += " ";
    if (!aw) {
        wattron(win_playlist_, focused ? (COLOR_PAIR(CP_FOCUSED)|A_BOLD)
                                       : (COLOR_PAIR(CP_BORDER)|A_BOLD));
        std::string bar = hdr; bar.resize((size_t)cols, ' ');
        mvwaddnstr(win_playlist_, 0, 0, bar.c_str(), cols);
        wattroff(win_playlist_, A_BOLD|COLOR_PAIR(CP_FOCUSED)|COLOR_PAIR(CP_BORDER));
    }
    int visible = paneVisibleRows(win_playlist_);
    // Guard against a stale scroll offset left past the end of the list (e.g.
    // a load where every track was a duplicate) — otherwise the bounds check
    // below fires on the first row and the pane draws blank.
    if (pl_scroll_ >= (int)playlist_.size())
        pl_scroll_ = std::max(0, (int)playlist_.size() - 1);
    if (pl_scroll_ < 0) pl_scroll_ = 0;
    ensurePlaylistCursorVisible();   // the invariant: every offscreen-cursor path self-heals here
    // Lit row = nowPlayingRow(); see the accessor for why current() can't be used
    // here (stale in stream mode; queue-launched stations light no row). Computed
    // once before the loop - the accessor scans the playlist, so a per-row call
    // would be O(n^2) on the draw path.
    const std::optional<std::size_t> now_row = nowPlayingRow();
    for (int i = 0; i < visible; ++i) {
        size_t idx = (size_t)(pl_scroll_ + i);
        if (idx >= playlist_.size()) break;
        const auto& e  = playlist_.at(idx);
        bool cursor  = ((int)idx == pl_cursor_);
        bool playing = now_row && *now_row == idx;
        short rpair = CP_DIM; attr_t rattr = A_BOLD;
        if      (cursor && focused) { rpair = CP_SELECTED;  rattr = A_BOLD; }
        else if (cursor)            { rpair = CP_SELECTED_UNFOCUSED; rattr = A_BOLD; }
        else if (playing)           { rpair = CP_STATUS_OK; rattr = A_BOLD; }
        else                        { rpair = CP_DIM;       rattr = A_BOLD; }
        wattron(win_playlist_, COLOR_PAIR(rpair) | rattr);
        const bool ico = config_.nerd_icons;
        std::string mark = (playing && !ico) ? "> " : "  ";
        // Optional filetype column (F11): a fixed 4-char field + 1 space between
        // title and duration. Blank tag (CD/stream/unknown) = zero width for
        // that row, title reclaims the space.
        std::string ftype;
        int ftw = 0;
        if (config_.show_filetype) {
            ftype = fileTypeTag(e.path);
            ftw = ftype.empty() ? 0 : 5;
        }
        std::string dur  = formatTime(e.duration_sec);
        // THE marquee-width constraint: nw is computed ONCE, with ftw already
        // subtracted, and this single value feeds BOTH the scroll decision and
        // the render (scrollToWidth marquees iff dispWidth > nw). A second
        // width calc anywhere makes long titles scroll against the wrong width.
        int nw = cw - (int)mark.size() - (int)dur.size() - 3 - ftw;
        std::string name = (nw > 0) ? scrollToWidth(e.display_title, nw, text_scroll_offset_) : "";
        std::string ftcol = ftw ? (padToWidth(ftype, 4) + " ") : "";
        std::string line = " " + mark + name + " " + ftcol + dur + " ";
        std::wstring wline = utf8_to_wide(padToWidth(line, cw));
        mvwaddnwstr(win_playlist_, i+1, cx, wline.c_str(), (int)wline.size());
        if (ico && playing) {   // play glyph on the reserved mark cell
            cchar_t cc;
            wchar_t s[2] = { L'\uf04b', 0 };  // play
            setcchar(&cc, s, rattr, rpair, nullptr);
            mvwadd_wch(win_playlist_, i+1, cx + 1, &cc);
        }
        wattroff(win_playlist_, A_BOLD|A_REVERSE|COLOR_PAIR(CP_SELECTED)
                              |COLOR_PAIR(CP_SELECTED_UNFOCUSED)
                              |COLOR_PAIR(CP_STATUS_OK)|COLOR_PAIR(CP_DIM));
    }
    if (aw) {
        panelFrame(win_playlist_, hdr, focused, L'\uf03a');
    } else if (!focused) {
        wattron(win_playlist_, COLOR_PAIR(CP_BORDER));
        box(win_playlist_, 0, 0);
        wattroff(win_playlist_, COLOR_PAIR(CP_BORDER));
    }
}

// ── Visualizer ────────────────────────────────────────────────────────────────
void UIManager::drawVisualizer() {
    werase(win_viz_);
    int rows, cols;
    getmaxyx(win_viz_, rows, cols);

    const bool aw = config_.awesome_mode;
    // Header (Classic: full-width bar on row 0; Awesome: title lives in the frame)
    if (!aw) {
        wattron(win_viz_, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        std::string hdr = " Visualizer ";
        hdr.resize((size_t)cols, ' ');
        mvwaddnstr(win_viz_, 0, 0, hdr.c_str(), cols);
        wattroff(win_viz_, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }

    if (audio_.state() == PlaybackState::Stopped) {
        wattron(win_viz_, COLOR_PAIR(CP_DIM) | A_DIM);
        const char* msg = "  no signal  ";
        int mx = (cols - (int)strlen(msg)) / 2;
        int my = rows / 2;
        if (mx > 0) mvwaddstr(win_viz_, my, mx, msg);
        wattroff(win_viz_, COLOR_PAIR(CP_DIM) | A_DIM);
        if (aw) panelFrame(win_viz_, " Spectrum ", false, L'\uf001');
        return;
    }

    // Draw bars
    const int bar_area_rows = rows - 2;  // -1 header, -1 bottom border row
    const int bar_area_cols = cols - 2;  // -1 each side border
    if (bar_area_rows < 2 || bar_area_cols < 4) return;

    // Thin, uniform bars that fill the width. Each bar is a SINGLE column with a
    // SINGLE-column gap (slot 2); the count grows to fill the strip rather than the
    // bars getting wider. A 1-col bar can't show a seam down its middle (a 2-col one
    // does, on many fonts) and 1-on/1-off never clumps. Since that's usually MORE
    // than the 64 DSP bins, each bar's level is linearly INTERPOLATED across the bins,
    // so every bar is distinct; the sub-slot remainder is a tiny centred margin.
    const int bar_w = 1, gap = 1, slot = bar_w + gap;
    const int n_bars  = std::clamp(bar_area_cols / slot, 1, 4 * VIZ_BINS);
    const int x_start = 1 + std::max(0, (bar_area_cols - n_bars * slot) / 2);

    for (int b = 0; b < n_bars; ++b) {
        const int x0 = x_start + b * slot;

        float val;
        if (n_bars <= 1) {
            val = viz_smoothed_[0];
        } else {
            const double fb  = (double)b * (VIZ_BINS - 1) / (n_bars - 1);  // bin position
            const int    i0  = (int)fb;
            const int    i1  = std::min(i0 + 1, VIZ_BINS - 1);
            const float  frac = (float)(fb - i0);
            val = viz_smoothed_[i0] * (1.0f - frac) + viz_smoothed_[i1] * frac;
        }
        // Short Awesome strip crushes dynamics -> lift with a gamma + gain and a
        // taller floor so bars fill the strip; Classic's tall pane keeps the raw
        // response.
        float floor = kVizFloorCells;
        if (aw) { val = std::pow(val, 0.65f) * 1.10f; floor = kVizStripFloorCells; }
        // Lift the resting floor per spectrum style: 80s LED +20%, classic bars +10%.
        floor *= config_.viz_led ? 1.20f : 1.10f;
        float exact = std::clamp(val, 0.0f, 1.0f) * bar_area_rows;
        if (exact < floor) exact = floor;

        if (config_.viz_led) {
            // 80s graphic-EQ (F2 on): stacked LED segments, colour by HEIGHT in FIXED
            // zones - base band at the bottom rising to the peak colour at the top
            // (idle bars sit green; only loud ones reach red). Each lit cell is a lower
            // half-block on the base bg, so a dark gap sits above it => discrete LEDs.
            // >=1 keeps a lit baseline LED while playing (EQ resting row).
            const int full = std::clamp((int)(exact + 0.5f), 1, bar_area_rows);
            for (int r = 0; r < full; ++r) {
                const int   screen_row = rows - 1 - r;
                const float fr = (bar_area_rows > 1) ? (float)r / (bar_area_rows - 1) : 1.0f;
                const short pair = (fr < 0.50f) ? CP_VIZ_LOW_B
                                 : (fr < 0.78f) ? CP_VIZ_MID_B
                                 : (fr < 0.92f) ? CP_VIZ_HIGH_B : CP_VIZ_TIP;
                cchar_t cc; wchar_t s[2] = { (wchar_t)0x2584, 0 };   // ▄ LOWER HALF BLOCK
                setcchar(&cc, s, A_NORMAL, pair, nullptr);
                for (int c = 0; c < bar_w && x0 + c < cols - 1; ++c)
                    mvwadd_wch(win_viz_, screen_row, x0 + c, &cc);
            }
        } else {
            // Classic solid bars (default): colour by FREQUENCY position, peak-tipped,
            // with an 8x sub-cell fractional top (▁..▇) for smooth motion.
            int full    = (int)exact;
            int eighths = (int)((exact - full) * 8.0f + 0.5f);
            if (eighths >= 8) { ++full; eighths = 0; }
            full = std::clamp(full, 0, bar_area_rows);
            const float pos  = (float)b / n_bars;
            const short pair = (pos < 0.33f) ? CP_VIZ_LOW
                             : (pos < 0.66f) ? CP_VIZ_MID : CP_VIZ_HIGH;
            for (int r = 0; r < bar_area_rows; ++r) {
                const int screen_row = rows - 1 - r;
                if (r < full) {
                    const short dp = (r == full - 1 && eighths == 0) ? CP_VIZ_PEAK : pair;
                    wattron(win_viz_, COLOR_PAIR(dp));
                    for (int c = 0; c < bar_w && x0 + c < cols - 1; ++c)
                        mvwaddch(win_viz_, screen_row, x0 + c, ' ');
                    wattroff(win_viz_, COLOR_PAIR(dp));
                } else if (r == full && eighths > 0) {
                    cchar_t cc; wchar_t s[2] = { (wchar_t)(0x2580 + eighths), 0 };
                    setcchar(&cc, s, A_NORMAL, CP_VIZ_TIP, nullptr);
                    for (int c = 0; c < bar_w && x0 + c < cols - 1; ++c)
                        mvwadd_wch(win_viz_, screen_row, x0 + c, &cc);
                }
            }
        }
    }

    if (aw) {
        panelFrame(win_viz_, " Spectrum ", false, L'\uf001');
    } else {
        // Border
        wattron(win_viz_, COLOR_PAIR(CP_BORDER));
        box(win_viz_, 0, 0);
        wattroff(win_viz_, COLOR_PAIR(CP_BORDER));

        // Redraw header over border top
        wattron(win_viz_, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        std::string hdr2 = " Visualizer [v:back] ";
        mvwaddnstr(win_viz_, 0, 0, hdr2.c_str(), cols);
        wattroff(win_viz_, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

// ── Help screen ───────────────────────────────────────────────────────────────
void UIManager::drawHelp() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    struct Entry { const char* key; const char* desc; bool is_section = false; };
    static const Entry entries[] = {
        { "Playback",       "",                             true  },
        { "Enter",          "Play selected / enter dir"           },
        { "Space",          "Pause / Resume"                      },
        { "s",              "Stop"                                },
        { "n",              "Next track"                          },
        { "p",              "Previous track"                      },
        { "[  /  ]",        "Seek -5 / +5 seconds"                },
        { "-  /  +",        "Volume down / up  (5%)"              },
        { "m",              "Mute / unmute"                       },
        { "<  /  >",        "Balance left / right  (`=center)"   },
        { "{  /  }",        "Speed down / up  (2% steps, 50-200%)"  },
        { "f",              "Toggle ReplayGain normalization"      },
        { "*",              "Star/favourite highlighted track"      },
        { "e",              "10-band EQ  (1-5:presets  0:flat)"  },
        { "t",              "Toggle elapsed / remaining time"     },
        { "w",              "Toggle clock display in title bar"   },
        { "!",              "Toggle Windows toast notifications"  },
        { "Modes",          "",                             true  },
        { "r",              "Cycle repeat:  off / one / all"      },
        { "Z  (Shift+Z)",   "Sleep timer: cycle 15/30/45/60/90m / off" },
        { "Navigation",     "",                             true  },
        { "Tab",            "Switch focus: browser / playlist"    },
        { "j / k",          "Navigate down / up"                  },
        { "Arrow up/down",  "Navigate down / up"                  },
        { "PgDn / PgUp",    "Navigate down / up one page"         },
        { "Home / End",     "Jump to first / last row"            },
        { "Left arrow",     "Go to parent directory"              },
        { "g",              "Goto directory  (Tab = complete)"    },
        { "Playlist",       "",                             true  },
        { "a",              "Add selection to playlist (recursive)"},
        { "d",              "Delete selected track"               },
        { "K  /  J",        "Move track up / down"                },
        { "c",              "Clear entire playlist"               },
        { "S  (Shift+S)",   "Save playlist as M3U file"           },
        { "l",              "Load / append M3U playlist file"     },
        { "Convert",        "",                             true  },
        { "x",              "Convert file / folder / marked set to another format" },
        { "u",              "Mark / unmark file (for convert)"    },
        { "U  (Shift+U)",   "Clear all marks"                     },
        { "Up/Dn L/R M",    "Rip/rec/convert modal: pick per-format quality" },
        { "Views",          "",                             true  },
        { "o",              "Cycle playlist sort: none/title/artist/dur/file" },
        { "O  (Shift+O)",   "Cycle browser sort: name / modified / size"  },
        { "H  (Shift+H)",   "Toggle hidden files in browser"              },
        { "b",              "Bookmark current directory"          },
        { "B",              "Show bookmarks  (Enter:jump x:del)"  },
        { "v",              "Toggle visualizer"                   },
        { "L  (Shift+L)",   "Toggle lyrics  (needs .lrc file)"   },
        { "A  (Shift+A)",   "About RE-MOCT"                      },
        { "X  (Shift+X)",   "Output device picker"               },
        { "i",              "Track info popup"                    },
        { "Ctrl+L",         "Force redraw / fix layout after resize" },
        { "?",              "Toggle this help  (j/k, PgUp/PgDn, Home/End scroll)"  },
        { "q",              "Add track to play queue"             },
        { "Q  (Shift+Q)",   "Show / hide queue pane"              },
        { "Ctrl+R",         "Fetch CD metadata from MusicBrainz" },
        { "Ctrl+Y",         "Rip CD  (A=AccurateRip  C=CUETools  Y=Local  B=Local 2-pass)" },
        { "Ctrl+D",         "Toggle Discord Rich Presence" },
        { "Ctrl+T",         "Toggle Classic / Awesome theme" },
        { "F2",             "Spectrum style: classic / 80s LED"   },
        { "F3",             "Follow the playing track (cursor tracks the song)" },
        { "F7  /  F8",      "Awesome theme: previous / next" },
        { "F  (Shift+F)",   "Toggle file-type column (FLAC/MP3/...) in the playlist" },
        { "F12",            "Refresh the [Drives] list (pick up hot-plugged drives)" },
        { "E  (Shift+E)",   "Eject highlighted CD drive (in [Drives])" },
        { "\\",             "Search playlist (jump to a track)" },
#ifdef PDCURSES
        { "Alt+Enter",      "Toggle fullscreen (borderless)"      },
#endif
        { "~",              "Reload theme.conf colours (live)" },
        { "Ctrl+N",         "Toggle Nerd Font icons (needs Nerd Font)" },
        { "Ctrl+A",         "Toggle deep-analysis iHeart log (diagnostic)" },
        { "Ctrl+K",         "Stream mode: Web Player (fewer ads) / Raw broadcast" },
        { "Ctrl+U",         "Play an internet-radio stream URL" },
        { "Ctrl+E",         "Record the playing stream ([Rec] panel)" },
        { "Ctrl+O",         "Normalize folder (batch ReplayGain scan)" },
        { "Ctrl+G",         "Last.fm login (scrobbling)" },
        { "Ctrl+B",         "ListenBrainz login (paste user token)" },
    };

    const int n    = (int)(sizeof(entries) / sizeof(entries[0]));
    const int key_w = 16;
    const int desc_w = cols - key_w - 6;

    // Build flat list of rendered lines with type tags so we can scroll
    struct Line { bool is_section; std::string key; std::string desc; };
    std::vector<Line> lines;
    for (int i = 0; i < n; ++i) {
        if (entries[i].is_section) {
            lines.push_back({ true, entries[i].key, "" });
        } else {
            lines.push_back({ false, entries[i].key, entries[i].desc });
        }
    }

    int total_lines  = (int)lines.size();
    int visible_rows = rows - 2;  // -1 header -1 border bottom

    // Clamp scroll
    int max_scroll = std::max(0, total_lines - visible_rows);
    help_scroll_ = std::clamp(help_scroll_, 0, max_scroll);

    // Header
    std::string hdr = " RE-MOCT keybindings  [? close  j/k scroll] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    // Render visible lines
    for (int r = 0; r < visible_rows; ++r) {
        int li = help_scroll_ + r;
        if (li >= total_lines) break;
        int screen_row = r + 1;

        if (lines[(size_t)li].is_section) {
            // Section header — cyan bold, full width
            wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
            std::string sh = "  " + lines[(size_t)li].key;
            sh.resize((size_t)cols, ' ');
            mvwaddnstr(w, screen_row, 0, sh.c_str(), cols);
            wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
        } else {
            // Key
            wattron(w, COLOR_PAIR(CP_STATUS_OK));
            std::string k = "    " + lines[(size_t)li].key;
            k.resize((size_t)(key_w + 4), ' ');
            mvwaddnstr(w, screen_row, 0, k.c_str(), key_w + 4);
            wattroff(w, COLOR_PAIR(CP_STATUS_OK));
            // Description
            wattron(w, COLOR_PAIR(CP_DIM));
            std::string d = lines[(size_t)li].desc;
            if ((int)d.size() > desc_w) d = d.substr(0, (size_t)desc_w);
            mvwaddnstr(w, screen_row, key_w + 4, d.c_str(), desc_w);
            wattroff(w, COLOR_PAIR(CP_DIM));
        }
    }

    // Scroll indicator on right border
    if (total_lines > visible_rows) {
        wattron(w, COLOR_PAIR(CP_DIM));
        if (help_scroll_ > 0)
            mvwaddstr(w, 1, cols - 2, "^");
        if (help_scroll_ < max_scroll)
            mvwaddstr(w, rows - 2, cols - 2, "v");
        wattroff(w, COLOR_PAIR(CP_DIM));
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf059');
    } else {
        // Border
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        // Redraw header over border top
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

// ── Bookmarks popup ───────────────────────────────────────────────────────────
void UIManager::drawBookmarks() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    std::string hdr = " Bookmarks  [Enter:jump  x:del  B:close] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    const auto& bm = config_.bookmarks;
    if (bm.empty()) {
        wattron(w, COLOR_PAIR(CP_DIM));
        mvwaddstr(w, rows/2, 2, "No bookmarks yet.  Press b to add current dir.");
        wattroff(w, COLOR_PAIR(CP_DIM));
    } else {
        // Clamp cursor
        bookmark_cursor_ = std::clamp(bookmark_cursor_, 0, (int)bm.size()-1);
        int visible = rows - 2;
        int scroll  = std::max(0, bookmark_cursor_ - visible/2);
        scroll      = std::min(scroll, std::max(0, (int)bm.size() - visible));

        for (int i = 0; i < visible; ++i) {
            int bi = scroll + i;
            if (bi >= (int)bm.size()) break;
            bool cursor = (bi == bookmark_cursor_);
            if (cursor) wattron(w, COLOR_PAIR(CP_SELECTED)|A_BOLD);
            else        wattron(w, COLOR_PAIR(CP_DIM)|A_BOLD);

            // Column-aware UTF-8: marquee + pad in DISPLAY columns (not bytes), then
            // draw via the wide API so multibyte titles render and align correctly.
            std::string body = " " + scrollToWidth(bm[(size_t)bi], cols - 2, text_scroll_offset_);
            std::wstring wline = utf8_to_wide(padToWidth(body, cols));
            mvwaddnwstr(w, i+1, 0, wline.c_str(), (int)wline.size());

            if (cursor) wattroff(w, COLOR_PAIR(CP_SELECTED)|A_BOLD);
            else        wattroff(w, COLOR_PAIR(CP_DIM)|A_BOLD);
        }
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf02e');
    } else {
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

// ── Playlist-search results pane ─────────────────────────────────────────────
// Pick-to-jump results for \ (mirrors drawBookmarks). Lists matches by their
// display_title; Enter jumps pl_cursor_ to the match's REAL playlist index.
// The playlist itself is never filtered - see the UIManager.h contract.
void UIManager::drawSearchResults() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    std::string hdr = " Search: " + search_query_ + "  ["
                    + std::to_string(search_results_.size())
                    + " matches  Enter:jump  Esc:close] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    if (search_results_.empty()) {
        wattron(w, COLOR_PAIR(CP_DIM));
        mvwaddstr(w, rows/2, 2, "No matches.");
        wattroff(w, COLOR_PAIR(CP_DIM));
    } else {
        search_cursor_ = std::clamp(search_cursor_, 0, (int)search_results_.size()-1);
        int visible = rows - 2;
        int scroll  = std::max(0, search_cursor_ - visible/2);
        scroll      = std::min(scroll, std::max(0, (int)search_results_.size() - visible));

        for (int i = 0; i < visible; ++i) {
            int ri = scroll + i;
            if (ri >= (int)search_results_.size()) break;
            std::size_t pi = search_results_[(size_t)ri];
            if (pi >= playlist_.size()) continue;   // list shrank since search
            bool cursor = (ri == search_cursor_);
            if (cursor) wattron(w, COLOR_PAIR(CP_SELECTED)|A_BOLD);
            else        wattron(w, COLOR_PAIR(CP_DIM)|A_BOLD);

            // Column-aware UTF-8 (the drawBookmarks shape): marquee + pad in
            // display columns, draw via the wide API.
            std::string body = " " + scrollToWidth(playlist_.at(pi).display_title,
                                                   cols - 2, text_scroll_offset_);
            std::wstring wline = utf8_to_wide(padToWidth(body, cols));
            mvwaddnwstr(w, i+1, 0, wline.c_str(), (int)wline.size());

            if (cursor) wattroff(w, COLOR_PAIR(CP_SELECTED)|A_BOLD);
            else        wattroff(w, COLOR_PAIR(CP_DIM)|A_BOLD);
        }
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf002');   // search glyph
    } else {
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

// Jump the playlist cursor to a real playlist index and return to the playlist
// view. One pl_cursor_ assignment is the whole jump: scroll-to-visible is the
// slice-5 draw-time invariant's job (never touch pl_scroll_ here), and the
// slice-6 follow-sync is unaffected (nothing about what's PLAYING changes).
void UIManager::jumpToPlaylistIndex(std::size_t idx) {
    if (idx >= playlist_.size()) return;
    pl_cursor_ = (int)idx;
    focus_ = Pane::Playlist;      // put the user's eyes where the cursor landed
    right_pane_ = RightPane::Playlist;
    redraw_needed_.store(true);
}

// ── Lyrics pane ───────────────────────────────────────────────────────────────
void UIManager::drawChapters() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    std::string hdr = " Chapters  [Enter:jump  ,/.:prev/next  ;:close] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    if (current_chapters_.empty()) {
        wattron(w, COLOR_PAIR(CP_DIM));
        mvwaddstr(w, rows/2, 2, "No chapters in this track.");
        wattroff(w, COLOR_PAIR(CP_DIM));
    } else {
        int playing = currentChapterIndex();
        chapter_cursor_ = std::clamp(chapter_cursor_, 0, (int)current_chapters_.size()-1);
        int visible = rows - 2;
        int scroll  = std::max(0, chapter_cursor_ - visible/2);
        scroll      = std::min(scroll, std::max(0, (int)current_chapters_.size() - visible));

        for (int i = 0; i < visible; ++i) {
            int ci = scroll + i;
            if (ci >= (int)current_chapters_.size()) break;
            bool cursor = (ci == chapter_cursor_);
            bool nowpl  = (ci == playing);
            if (cursor) wattron(w, COLOR_PAIR(CP_SELECTED)|A_BOLD);
            else        wattron(w, COLOR_PAIR(CP_DIM)|A_BOLD);

            double s = current_chapters_[(size_t)ci].start_sec;
            int hh = (int)(s/3600), mm = ((int)s%3600)/60, ss = (int)s%60;
            char ts[16];
            std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d", hh, mm, ss);
            std::string mark  = nowpl ? ">" : " ";
            std::string title = scrollToWidth(current_chapters_[(size_t)ci].title, cols - 14, text_scroll_offset_);
            std::string line  = " " + mark + " " + ts + "  " + title;
            // Column-aware pad + wide-API draw (ts/mark are ASCII; title may be UTF-8).
            std::wstring wline = utf8_to_wide(padToWidth(line, cols));
            mvwaddnwstr(w, i+1, 0, wline.c_str(), (int)wline.size());

            if (cursor) wattroff(w, COLOR_PAIR(CP_SELECTED)|A_BOLD);
            else        wattroff(w, COLOR_PAIR(CP_DIM)|A_BOLD);
        }
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf0ca');
    } else {
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

void UIManager::drawLyrics() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    // Auto-load LRC when track changes
    const auto& track = audio_.currentTrack();
    if (track.path != lrc_loaded_path_) {
        lrc_loaded_path_ = track.path;
        lrc_.clear();
        if (!track.path.empty()) {
            std::string lrc_path = LrcData::findLrc(track.path);
            if (!lrc_path.empty()) lrc_.loadFile(lrc_path);
        }
    }

    // Header
    std::string hdr = " Lyrics  [L:close] ";
    if (lrc_.loaded)
        hdr = " Lyrics: " + (track.title.empty() ? "unknown" : track.title) + "  [L:close] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    int visible = rows - 2;  // -1 header -1 border

    if (!lrc_.loaded || lrc_.lines.empty()) {
        wattron(w, COLOR_PAIR(CP_DIM));
        std::string msg = track.path.empty() ? "  No track playing"
                        : "  No .lrc file found for this track";
        mvwaddstr(w, rows/2, 0, msg.c_str());
        wattroff(w, COLOR_PAIR(CP_DIM));
    } else {
        double pos   = audio_.positionSec();
        int cur_line = lrc_.currentLine(pos);
        int n_lines  = (int)lrc_.lines.size();

        // Build display lines: word-wrap each LRC line to cols-2 chars
        // Each display entry knows which LRC index it came from
        struct DisplayLine { int lrc_idx; std::string text; };
        std::vector<DisplayLine> display_lines;
        const int wrap_w = cols - 2;

        for (int li = 0; li < n_lines; ++li) {
            const std::string& txt = lrc_.lines[(size_t)li].text;
            if (txt.empty()) {
                display_lines.push_back({li, ""});
                continue;
            }
            if ((int)txt.size() <= wrap_w) {
                display_lines.push_back({li, txt});
            } else {
                // Word-wrap
                int pos2 = 0;
                while (pos2 < (int)txt.size()) {
                    // Find break point
                    int end = std::min(pos2 + wrap_w, (int)txt.size());
                    if (end < (int)txt.size()) {
                        // Try to break at last space
                        int sp = (int)txt.rfind(' ', (size_t)end);
                        if (sp > pos2) end = sp;
                    }
                    std::string chunk = txt.substr((size_t)pos2, (size_t)(end - pos2));
                    // Trim leading space on continuation lines
                    if (pos2 > 0 && !chunk.empty() && chunk[0] == ' ')
                        chunk = chunk.substr(1);
                    display_lines.push_back({li, chunk});
                    pos2 = end;
                    if (pos2 < (int)txt.size() && txt[pos2] == ' ') ++pos2;
                }
            }
        }

        // Find display line index for current LRC line
        int cur_display = 0;
        for (int i = 0; i < (int)display_lines.size(); ++i) {
            if (display_lines[(size_t)i].lrc_idx == cur_line) {
                cur_display = i; break;
            }
        }

        int total_display = (int)display_lines.size();
        int half    = visible / 2;
        int scroll  = std::clamp(cur_display - half, 0, std::max(0, total_display - visible));

        for (int r = 0; r < visible; ++r) {
            int di = scroll + r;
            if (di >= total_display) break;
            int screen_row  = r + 1;
            int this_lrc    = display_lines[(size_t)di].lrc_idx;
            bool is_current = (this_lrc == cur_line);
            bool is_near    = (std::abs(this_lrc - cur_line) <= 1);

            if      (is_current) wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
            else if (is_near)    wattron(w, COLOR_PAIR(CP_DIM)   | A_BOLD);
            else                 wattron(w, COLOR_PAIR(CP_DIM));

            const std::string& text = display_lines[(size_t)di].text;
            int tw  = (int)text.size();
            int pad = std::max(0, (cols - tw) / 2);
            std::string line(cols, ' ');
            if (pad < cols && tw > 0) {
                int copy_len = std::min(tw, cols - pad);
                line.replace((size_t)pad, (size_t)copy_len,
                              text.substr(0, (size_t)copy_len));
            }
            mvwaddnstr(w, screen_row, 0, line.c_str(), cols);

            if      (is_current) wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
            else if (is_near)    wattroff(w, COLOR_PAIR(CP_DIM)   | A_BOLD);
            else                 wattroff(w, COLOR_PAIR(CP_DIM));
        }
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf001');
    } else {
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

// ── About pane ────────────────────────────────────────────────────────────────
UIManager::TagEditability UIManager::tagEditability(const std::string& path) const {
    if (path.empty())            return TagEditability::Empty;
    if (isCDTrackPath(path))     return TagEditability::NotAFile;   // CD track
    if (isStreamPath(path))      return TagEditability::NotAFile;   // radio URL
    // Locked iff this exact file is the one actively playing.
    if (!audio_.streamMode() && !audio_.cdMode() &&
        path == audio_.currentTrack().path &&
        audio_.state() != PlaybackState::Stopped)
        return TagEditability::PlayingLocked;
    return TagEditability::Editable;
}

// Write the edited tags to disk. Returns true ONLY if the file was actually written:
// TagLib::FileRef::save() returns bool and on Windows a locked (playing) file fails
// with false, no throw. On failure we touch NOTHING in the UI - the pane and playlist
// must reflect the unchanged disk, not the attempted edit.
bool UIManager::saveTagEdits() {
    if (tag_edit_path_.empty()) return false;
    bool ok = false;
    try {
#ifdef _WIN32
        auto wp = utf8_to_wide(tag_edit_path_);
        TagLib::FileRef ref(wp.c_str(), false, TagLib::AudioProperties::Fast);
#else
        TagLib::FileRef ref(tag_edit_path_.c_str(), false, TagLib::AudioProperties::Fast);
#endif
        if (ref.isNull() || !ref.tag()) return false;
        auto* tag = ref.tag();
        tag->setTitle (TagLib::String(tag_edit_values_[0], TagLib::String::UTF8));
        tag->setArtist(TagLib::String(tag_edit_values_[1], TagLib::String::UTF8));
        tag->setAlbum (TagLib::String(tag_edit_values_[2], TagLib::String::UTF8));
        tag->setGenre (TagLib::String(tag_edit_values_[3], TagLib::String::UTF8));
        if (!tag_edit_values_[4].empty()) {
            try { tag->setYear((unsigned int)std::stoi(tag_edit_values_[4])); }
            catch (...) {}
        }
        ok = ref.save();                       // capture it - false = write refused
    } catch (...) {
        return false;
    }
    if (!ok) return false;                      // disk unchanged: do NOT lie to the UI

    info_cached_path_.clear();  // invalidate so drawTrackInfo re-reads on next open

    // Update playlist display title so it reflects immediately (only on a real write)
    for (std::size_t i = 0; i < playlist_.size(); ++i) {
        if (playlist_.at(i).path == tag_edit_path_) {
            // Rebuild display title as "Artist - Title"
            std::string dt;
            if (!tag_edit_values_[1].empty())
                dt = sanitizeForDisplay(tag_edit_values_[1]) + " - ";
            dt += sanitizeForDisplay(tag_edit_values_[0]);
            playlist_.setDisplayTitle(i, dt);
            break;
        }
    }
    return true;
}

void UIManager::drawQueue() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);
    (void)rows;

    const bool aw = config_.awesome_mode;
    const int  cx = aw ? 1 : 0;
    const int  cw = aw ? cols - 2 : cols;
    int qsize = playlist_.queueSize();
    std::string hdr = " Queue [" + std::to_string(qsize) + "]  d:remove  Q:close  Ctrl+Q:quit ";
    if (!aw) {
        std::string bar = hdr; bar.resize((size_t)cols, ' ');
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, bar.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }

    if (qsize == 0) {
        wattron(w, COLOR_PAIR(CP_DIM) | A_BOLD);
        mvwaddnstr(w, 2, cx + 1, "Queue is empty.", cw - 1);
        mvwaddnstr(w, 3, cx + 1, "Press q on a track to add it.", cw - 1);
        wattroff(w, COLOR_PAIR(CP_DIM) | A_BOLD);
        if (aw) panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf03a');
        return;
    }

    // Clamp cursor
    if (q_cursor_ >= qsize) q_cursor_ = qsize - 1;
    if (q_cursor_ < 0)      q_cursor_ = 0;

    int visible = paneVisibleRows(w);
    int scroll  = 0;
    if (q_cursor_ >= scroll + visible) scroll = q_cursor_ - visible + 1;

    for (int i = 0; i < visible; ++i) {
        int qi = scroll + i;
        if (qi >= qsize) break;

        const auto& e = playlist_.queueAt(qi);
        bool is_cursor = (qi == q_cursor_);

        if (is_cursor) wattron(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
        else           wattron(w, COLOR_PAIR(CP_DIM) | A_BOLD);

        std::string time_str = e.duration_sec > 0 ? formatTime(e.duration_sec) : "--:--";
        const int time_w = (int)time_str.size();          // ASCII → bytes == columns
        int title_width = cw - time_w - 4;
        // Left part (" " + title) padded in COLUMNS, then time + trailing space.
        // padToWidth guards width<=0, so a very narrow pane no longer overflows the
        // old byte resize (which cast a negative length to a huge size_t).
        std::string left = " " + scrollToWidth(e.display_title, title_width, text_scroll_offset_);
        left = padToWidth(left, cw - time_w - 1);
        std::wstring wline = utf8_to_wide(padToWidth(left + time_str + " ", cw));
        mvwaddnwstr(w, i + 1, cx, wline.c_str(), (int)wline.size());

        if (is_cursor) wattroff(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
        else           wattroff(w, COLOR_PAIR(CP_DIM) | A_BOLD);
    }
    if (aw) panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf03a');
}

void UIManager::drawAbout() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    // Header
    std::string hdr = " About RE-MOCT  [A:close] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    // ASCII logo
    static const char* logo[] = {
        " ######  #######         ##   ##  #######  #######  ########",
        " ##  ##  ##              ### ###  ##   ##  ##          ##   ",
        " ######  #####   ####    ## # ##  ##   ##  ##          ##   ",
        " ## ##   ##              ##   ##  ##   ##  ##          ##   ",
        " ##  ##  #######         ##   ##  #######  #######     ##   ",
    };
    const int logo_w = 62;
    const int logo_rows = 5;
    int logo_x = std::max(1, (cols - logo_w) / 2);

    // Render the logo as solid shade-block letters with a soft drop-shadow for
    // depth. '#' in the art becomes █ (or ▒ for the offset shadow); spaces blank.
    auto blockRow = [](const char* s, wchar_t fill) {
        std::wstring o;
        for (const char* p = s; *p; ++p) o += (*p == '#') ? fill : L' ';
        return o;
    };
    // Shadow pass first (offset +1,+1, dim) so the solid letters overlay it.
    wattron(w, COLOR_PAIR(CP_DIM));
    for (int i = 0; i < logo_rows && i + 3 < rows; ++i) {
        std::wstring sh = blockRow(logo[i], L'\u2592');   // ▒ soft shadow
        mvwaddnwstr(w, i + 3, logo_x + 1, sh.c_str(), (int)sh.size());
    }
    wattroff(w, COLOR_PAIR(CP_DIM));
    // Solid block letters on top.
    wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
    for (int i = 0; i < logo_rows && i + 2 < rows; ++i) {
        std::wstring ln = blockRow(logo[i], L'\u2588');   // █ solid
        mvwaddnwstr(w, i + 2, logo_x, ln.c_str(), (int)ln.size());
    }
    wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);

    // Info lines
    struct Line { const char* text; bool bold; };
    static const Line info[] = {
        { "Music On Console Terminal",      true  },
        { "",                               false },
#ifdef _WIN32
        { "Version v" REMOCT_VERSION "-win  |  C++20  |  ncurses  |  miniaudio  |  TagLib", false },
#else
        { "Version v" REMOCT_VERSION "-linux  |  C++20  |  ncurses  |  miniaudio  |  TagLib", false },
#endif
        { "",                               false },
        { "A terminal music player inspired by MOC (Music On Console).", false },
        { "Plays MP3, FLAC, OGG, WAV and more.  Gapless playback,",     false },
        { "crossfade, visualizer, lyrics, BPM detection and bookmarks.", false },
        { "goto path navigation with tab complete", false },
        { "",                               false },
        { "Press ? for keybindings",        false },
    };

    int row = logo_rows + 3;
    for (const auto& line : info) {
        if (row >= rows - 1) break;
        int tx = std::max(1, (cols - (int)strlen(line.text)) / 2);
        if (line.bold) {
            wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
            mvwaddstr(w, row, tx, line.text);
            wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
        } else if (line.text[0]) {
            wattron(w, COLOR_PAIR(CP_DIM) | A_BOLD);
            mvwaddstr(w, row, tx, line.text);
            wattroff(w, COLOR_PAIR(CP_DIM) | A_BOLD);
        }
        ++row;
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf05a');
    } else {
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}
// ── EQ pane ───────────────────────────────────────────────────────────────────
void UIManager::drawEq() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    bool enabled = audio_.eqEnabled();
    std::string hdr = std::string(" EQ") +
                      (enabled ? " [ON]" : " [OFF]");
    if (!eq_preset_name_.empty()) hdr += " [" + eq_preset_name_ + "]";
    hdr += "  [h/l:band  k/j:gain  f:toggle  0:flat  1:Rock  2:Bass  3:Vocal  4:Ref  5:Night  e:close] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    static const char* band_labels[] = {
        "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
    };

    // Layout: 10 bands, each band gets equal width
    int bar_area_rows = rows - 4;  // header + top label + bottom label + border
    if (bar_area_rows < 4) return;

    int scale_w  = 5;  // space for "+12 |" on left edge
    int band_w   = std::max(3, (cols - 2 - scale_w) / AudioManager::EQ_BANDS);
    float max_db = 12.0f;
    int   half   = bar_area_rows / 2;

    // Draw dB scale labels and centre line
    wattron(w, COLOR_PAIR(CP_DIM));
    mvwaddstr(w, 1,          1, "+12");
    mvwaddstr(w, 1+half,     1, "  0");
    mvwaddstr(w, 1+half*2,   1, "-12");
    // Vertical scale bar
    for (int r = 1; r <= half*2; ++r)
        mvwaddch(w, r + 1, scale_w - 1, '|');
    // Centre line
    for (int x = scale_w; x < cols-1; ++x)
        mvwaddch(w, 1 + half, x, '-');
    wattroff(w, COLOR_PAIR(CP_DIM));

    for (int b = 0; b < AudioManager::EQ_BANDS; ++b) {
        float db  = audio_.getEqGain(b);
        int   col = scale_w + b * band_w;
        bool  sel = (b == eq_cursor_);

        // Draw bar
        int bar_h = (int)std::round(std::abs(db) / max_db * half);
        bar_h = std::clamp(bar_h, 0, half);
        bool positive = (db >= 0.0f);

        for (int r = 0; r < half; ++r) {
            int screen_row_up   = half - r;        // r=0 → row just above centre
            int screen_row_down = half + r + 2;    // r=0 → row just below centre (+1 hdr, +1 centre)

            if (positive && r < bar_h && screen_row_up > 1) {
                // Use solid block: sel=cyan-on-cyan, boost=green-on-green, disabled=white-on-white
                short pair = sel ? CP_VIZ_HIGH
                           : (enabled ? CP_VIZ_LOW : CP_VIZ_PEAK);
                wattron(w, COLOR_PAIR(pair));
                for (int x = 0; x < band_w-1 && col+x < cols-1; ++x)
                    mvwaddch(w, screen_row_up, col+x, ' ');
                wattroff(w, COLOR_PAIR(pair));
            } else if (!positive && r < bar_h && screen_row_down < rows-3) {
                // cut = yellow-on-yellow, sel = cyan-on-cyan
                short pair = sel ? CP_VIZ_HIGH
                           : (enabled ? CP_VIZ_MID : CP_VIZ_PEAK);
                wattron(w, COLOR_PAIR(pair));
                for (int x = 0; x < band_w-1 && col+x < cols-1; ++x)
                    mvwaddch(w, screen_row_down, col+x, ' ');
                wattroff(w, COLOR_PAIR(pair));
            }
        }

        // Gain value — show "0" for flat, "+N" or "-N" otherwise
        char dbstr[8];
        if (std::abs(db) < 0.5f)
            std::snprintf(dbstr, sizeof(dbstr), "0");
        else
            std::snprintf(dbstr, sizeof(dbstr), "%+.0f", db);

        wattron(w, sel ? (COLOR_PAIR(CP_TITLE)|A_BOLD) : COLOR_PAIR(CP_DIM));
        mvwaddstr(w, rows-3, col, dbstr);
        mvwaddstr(w, rows-2, col, band_labels[b]);
        wattroff(w, sel ? (COLOR_PAIR(CP_TITLE)|A_BOLD) : COLOR_PAIR(CP_DIM));

        if (sel) {
            wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
            mvwaddch(w, rows-4, col + (band_w-1)/2, '^');
            wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
        }
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf1de');
    } else {
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

// ── Device picker pane ────────────────────────────────────────────────────────
void UIManager::drawDevices() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    std::string hdr = " Output Device  [Enter:select  d:default  X:close] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    if (device_list_.empty()) {
        wattron(w, COLOR_PAIR(CP_DIM));
        mvwaddstr(w, rows/2, 2, "No audio devices found.");
        wattroff(w, COLOR_PAIR(CP_DIM));
    } else {
        int visible = rows - 3;
        int scroll  = std::max(0, device_cursor_ - visible/2);
        scroll      = std::min(scroll, std::max(0, (int)device_list_.size() - visible));
        int sel     = audio_.selectedDeviceIndex();

        for (int i = 0; i < visible; ++i) {
            int di = scroll + i;
            if (di >= (int)device_list_.size()) break;
            bool is_cursor  = (di == device_cursor_);
            bool is_current = (di == sel);

            if      (is_cursor)  wattron(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (is_current) wattron(w, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
            else                 wattron(w, COLOR_PAIR(CP_DIM) | A_BOLD);

            const bool ico = config_.nerd_icons;
            std::string mark = (is_current && !ico) ? "> " : "  ";
            std::string body = " " + mark + scrollToWidth(device_list_[(size_t)di].name, cols - 4, text_scroll_offset_);
            std::wstring wline = utf8_to_wide(padToWidth(body, cols));
            mvwaddnwstr(w, i + 1, 0, wline.c_str(), (int)wline.size());
            if (ico && is_current) {   // active-device glyph on the reserved mark cell
                cchar_t cc;
                wchar_t s[2] = { L'\uf028', 0 };  // speaker (volume-up)
                short pair = is_cursor ? CP_SELECTED : CP_STATUS_OK;
                setcchar(&cc, s, A_BOLD, pair, nullptr);
                mvwadd_wch(w, i + 1, 1, &cc);
            }

            if      (is_cursor)  wattroff(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (is_current) wattroff(w, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
            else                 wattroff(w, COLOR_PAIR(CP_DIM) | A_BOLD);
        }

        // Footer hint
        wattron(w, COLOR_PAIR(CP_DIM));
        std::string foot = " Default device: press d to reset ";
        foot.resize((size_t)cols, ' ');
        mvwaddnstr(w, rows-1, 0, foot.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_DIM));
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf025');
    } else {
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

// Pull the embedded front-cover (or first picture) bytes from a file's tags via
// TagLib's generic complexProperties("PICTURE"). Empty vector = no art / any
// failure. Local + synchronous (fast); no network. (Radio/streams have no file -
// they degrade to metadata-only, matching the isCDTrackPath skip.)
static std::vector<uint8_t> extractEmbeddedArt(const std::string& path) {
    try {
#ifdef _WIN32
        auto wp = utf8_to_wide(path);
        TagLib::FileRef ref(wp.c_str(), false);
#else
        TagLib::FileRef ref(path.c_str(), false);
#endif
        if (ref.isNull()) return {};
        const auto pics = ref.complexProperties("PICTURE");
        TagLib::ByteVector first, front;
        for (const auto& pic : pics) {
            auto d = pic.find("data");
            if (d == pic.end()) continue;
            TagLib::ByteVector bv = d->second.value<TagLib::ByteVector>();
            if (bv.isEmpty()) continue;
            if (first.isEmpty()) first = bv;
            auto t = pic.find("pictureType");
            if (t != pic.end() && t->second.value<TagLib::String>() == "Front Cover") {
                front = bv; break;
            }
        }
        const TagLib::ByteVector& use = front.isEmpty() ? first : front;
        if (!use.isEmpty())
            return std::vector<uint8_t>(use.data(), use.data() + use.size());
    } catch (...) {}
    return {};
}

// Allocate curses colours + pairs for a rendered cover, filling out_pairs (one
// pair index per cell). Uses a DEDICATED slot/pair range so it never disturbs the
// theme (colours 16..~30, pairs 1..14): art colours start at 64, pairs at 20,
// both capped at index 255 so plain init_color/init_pair (short) work on ncursesw
// (256) and PDCurses alike - no extended-colour API needed. Greedy-quantises to
// the budget (exact match -> reuse; else allocate; else nearest already-allocated).
// Truecolor tier when COLORS>=256 && can_change_color(); else nearest-of-16 (the
// same fidelity ladder the themes use). Returns false only if colour is unusable.
bool UIManager::allocArtColorPairs(const cover::Rendered& art,
                                   std::vector<short>& out_pairs) {
    out_pairs.assign(art.cells.size(), 0);
    if (!has_colors() || art.cells.empty()) return false;

    const bool truecolor = (COLORS >= 256 && can_change_color());
    const short C0 = kArtColourBase;
    const int   c_budget = truecolor ? std::min((int)COLORS, 256) - C0 : 0;   // art colour slots
    const short P0 = kArtPairBase;
    const int   p_budget = std::min(COLOR_PAIRS, 256) - P0;                    // art pair slots
    if (p_budget < 1) return false;
#ifdef REMOCT_PROBE
    // Slice-1 Probe A2 counters (instrumentation only; allocation logic unchanged).
    int probe_colour_fallback = 0;   // times the colour budget was spent (nearest-existing)
    int probe_pair_fallback   = 0;   // times the PAIR budget was spent (nearest-existing)
#endif

    static const int kBasic16[16][3] = {
        {0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},
        {128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255},
    };
    auto dist = [](int r,int g,int b,int R,int G,int B) -> long {
        long dr=r-R, dg=g-G, db=b-B; return 2*dr*dr + 4*dg*dg + 3*db*db;
    };

    std::vector<std::array<int,3>> pal;   // slot index i -> rgb (truecolor path)
    auto colorFor = [&](int r,int g,int b) -> short {
        if (!truecolor) {                 // nearest-of-16
            long best=-1; short bi=0;
            for (short i=0;i<16;++i){ long d=dist(r,g,b,kBasic16[i][0],kBasic16[i][1],kBasic16[i][2]);
                                      if(best<0||d<best){best=d;bi=i;} }
            return bi;
        }
        for (size_t i=0;i<pal.size();++i)
            if (pal[i][0]==r && pal[i][1]==g && pal[i][2]==b) return (short)(C0+i);
        if ((int)pal.size() < c_budget) {
            const short id = (short)(C0 + pal.size());
            auto sc=[](int c)->short{ return (short)((c*1000+127)/255); };  // 0..255 -> 0..1000
            init_color(id, sc(r), sc(g), sc(b));
            pal.push_back({r,g,b});
            return id;
        }
        long best=-1; short bi=C0;        // budget spent: nearest existing
#ifdef REMOCT_PROBE
        ++probe_colour_fallback;
#endif
        for (size_t i=0;i<pal.size();++i){ long d=dist(r,g,b,pal[i][0],pal[i][1],pal[i][2]);
                                           if(best<0||d<best){best=d;bi=(short)(C0+i);} }
        return bi;
    };

    std::vector<std::pair<short,short>> pairs;   // pair index j -> (fg,bg)
    auto pairFor = [&](short fg, short bg) -> short {
        for (size_t j=0;j<pairs.size();++j)
            if (pairs[j].first==fg && pairs[j].second==bg) return (short)(P0+j);
        if ((int)pairs.size() < p_budget) {
            const short id = (short)(P0 + pairs.size());
            init_pair(id, fg, bg);
            pairs.push_back({fg,bg});
            return id;
        }
        // Budget spent: reuse the existing pair nearest in colour to the wanted fg/bg
        // (the pair analogue of colorFor's nearest-existing fallback). The 22x11 art box
        // is 242 cells vs a 236-pair budget, so a busy cover reaches this on real art -
        // Probe A2 measured it on 6 of 116 covers. P0 is only the seed for the search.
        // fg/bg are slot IDs, not RGB: map back via pal (truecolor, slot = C0+i) or
        // kBasic16 (non-truecolor, slot = 0..15), then score by summed fg+bg distance.
        auto slotRGB = [&](short slot, int& r, int& g, int& b) {
            if (truecolor) {
                const int i = slot - C0;
                if (i >= 0 && i < (int)pal.size()) { r=pal[i][0]; g=pal[i][1]; b=pal[i][2]; return; }
            }
            if (slot >= 0 && slot < 16) { r=kBasic16[slot][0]; g=kBasic16[slot][1]; b=kBasic16[slot][2]; return; }
            r=g=b=0;
        };
        int wfr,wfg,wfb, wbr,wbg,wbb;
        slotRGB(fg,wfr,wfg,wfb); slotRGB(bg,wbr,wbg,wbb);
        long best=-1; short bi=(short)P0;
        for (size_t j=0;j<pairs.size();++j) {
            int pfr,pfg,pfb, pbr,pbg,pbb;
            slotRGB(pairs[j].first,  pfr,pfg,pfb);
            slotRGB(pairs[j].second, pbr,pbg,pbb);
            long d = dist(wfr,wfg,wfb, pfr,pfg,pfb) + dist(wbr,wbg,wbb, pbr,pbg,pbb);
            if (best<0 || d<best) { best=d; bi=(short)(P0+j); }
        }
#ifdef REMOCT_PROBE
        ++probe_pair_fallback;
#endif
        return bi;
    };

    for (size_t i=0;i<art.cells.size();++i) {
        const auto& c = art.cells[i];
        out_pairs[i] = pairFor(colorFor(c.r_top,c.g_top,c.b_top),
                               colorFor(c.r_bot,c.g_bot,c.b_bot));
    }
#ifdef REMOCT_PROBE
    Log::writef("probe",
        "A2 alloc grid=%dx%d cells=%zu truecolor=%d colours=%zu pairs=%zu "
        "c_budget=%d p_budget=%d colour_fallback=%d pair_fallback=%d ret=1",
        art.cols, art.rows, art.cells.size(), (int)truecolor, pal.size(), pairs.size(),
        c_budget, p_budget, probe_colour_fallback, probe_pair_fallback);
#endif
    return true;
}

// Small MRU cache of decoded covers, keyed on "path|box". Look-up never reorders
// (so the returned pointer stays valid until the next put); FIFO eviction at cap.
const cover::Rendered* UIManager::artCacheGet(const std::string& key) {
    for (auto& e : info_art_cache_) if (e.key == key) return &e.art;
    return nullptr;
}
void UIManager::artCachePut(const std::string& key, const cover::Rendered& r) {
    for (auto& e : info_art_cache_) if (e.key == key) { e.art = r; return; }
    info_art_cache_.push_back({ key, r });
    if (info_art_cache_.size() > kInfoArtCacheMax) info_art_cache_.erase(info_art_cache_.begin());
}

// Commit a decoded grid as the shown art: store the grid and mark its key. Colour
// pairs are allocated later, at the blit. r.ok==false is a valid "no art" verdict.
void UIManager::commitInfoArt(const std::string& key, const cover::Rendered& r) {
    info_art_key_ = key;
    info_art_ = r;
    // Colour-pair allocation happens at the blit (drawTrackInfo), when we know
    // which art source owns the shared global pair table. See art_pairs_key_.
}

// Spawn the one-shot decode worker (file read + stb decode, both curses-free).
// Single in-flight (guarded by info_art_active_); the result is picked up by
// refreshInfoArt on the UI thread, which does the colour allocation.
void UIManager::startInfoArtDecode(const std::string& path, const std::string& key,
                                   int box_cols, int box_rows) {
    if (info_art_active_.load()) return;
    info_art_active_.store(true);
    info_art_done_.store(false);
    { std::lock_guard<std::mutex> lk(info_art_mtx_);
      info_art_want_key_ = key; info_art_result_ = cover::Rendered{}; }
    if (info_art_thread_.joinable()) info_art_thread_.join();   // prior worker already done
    std::string p = path; int bc = box_cols, br = box_rows;
    info_art_thread_ = std::thread([this, p, bc, br]() {
        std::vector<uint8_t> bytes = extractEmbeddedArt(p);
        cover::Rendered r;
        if (!bytes.empty()) r = cover::render(bytes, bc, br);   // r.ok stays false if no art
        { std::lock_guard<std::mutex> lk(info_art_mtx_); info_art_result_ = std::move(r); }
        info_art_done_.store(true);
    });
}

// Ensure the shown cover matches (path, box). Same key -> already committed, no
// work. Cached decode -> commit instantly (colour-alloc only). Otherwise decode
// off the UI thread and fill it in when it lands, so the pane never blocks. Never
// throws. First open / track-change briefly shows no art, then the cover fills in.
void UIManager::refreshInfoArt(const std::string& path, int box_cols, int box_rows) {
    const std::string key = path + "|" + std::to_string(box_cols) + "x" + std::to_string(box_rows);

    // Pick up a finished background decode: cache it, and show it if still current.
    if (info_art_done_.exchange(false)) {
        cover::Rendered r; std::string forkey;
        { std::lock_guard<std::mutex> lk(info_art_mtx_);
          r = std::move(info_art_result_); forkey = info_art_want_key_; }
        if (info_art_thread_.joinable()) info_art_thread_.join();
        info_art_active_.store(false);
        artCachePut(forkey, r);
        if (forkey == key) { commitInfoArt(forkey, r); requestRedraw(); }
    }

    if (key == info_art_key_) return;                 // already committed & drawn

    if (const cover::Rendered* c = artCacheGet(key)) { commitInfoArt(key, *c); return; }

    // Not decoded yet: clear the stale cover (don't show the wrong one) and kick
    // off the decode. Leave info_art_key_ on the old key so we keep retrying the
    // spawn until this key's decode lands and commits it.
    if (!info_art_.cells.empty()) {
        info_art_ = cover::Rendered{};
    }
    if (path.empty() || isCDTrackPath(path) || box_cols < 4 || box_rows < 2) {
        artCachePut(key, cover::Rendered{});          // cache the "no art" verdict
        commitInfoArt(key, cover::Rendered{});
        return;
    }
    startInfoArtDecode(path, key, box_cols, box_rows);
}

// Lazy-load the bundled RE-MOCT logo (the radio-art floor). Tried once; if the
// asset isn't found the floor is simply blank (metadata reflows full width, same
// as a local file with no embedded cover). Copied beside the exe at build time
// (remoct_logo.jpg); a couple of source-tree relatives cover in-place dev runs.
const std::vector<uint8_t>& UIManager::logoBytes() {
    if (logo_load_tried_) return logo_bytes_;
    logo_load_tried_ = true;
    const fs::path exe = fs::path(port::exeDir());
    for (const fs::path& p : { exe / "remoct_logo.jpg",
                               exe / "icon" / "remoct_logo.jpg",
                               exe / ".." / "icon" / "remoct_logo.jpg",
                               exe / ".." / ".." / "icon" / "remoct_logo.jpg" }) {
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) continue;
        std::streamoff n = f.tellg();
        if (n <= 0) continue;
        logo_bytes_.resize((size_t)n);
        f.seekg(0);
        if (f.read((char*)logo_bytes_.data(), n)) break;
        logo_bytes_.clear();   // short read -> treat as absent
    }
    return logo_bytes_;
}

// Spawn the one-shot radio-art fetch worker: resolve the cover URL (station-
// supplied iHeart cover if given, else a song-entity urlBySong lookup) and GET
// the bytes, all off the UI thread. Single in-flight (guarded by radio_art_active_).
void UIManager::startRadioArtFetch(const std::string& artist, const std::string& title,
                                   const std::string& station_art) {
    if (radio_art_active_.load()) return;
    radio_art_active_.store(true);
    radio_art_done_.store(false);
    radio_fetch_station_ = !station_art.empty();
    { std::lock_guard<std::mutex> lk(radio_art_mtx_);
      radio_art_want_key_ = artist + "\t" + title; radio_art_result_.clear(); }
    if (radio_art_thread_.joinable()) radio_art_thread_.join();   // prior worker already done
    std::string a = artist, t = title, sa = station_art;
    radio_art_thread_ = std::thread([this, a, t, sa]() {
        std::string url = sa.empty() ? CoverArt::urlBySong(a, t) : sa;
        std::vector<uint8_t> bytes = CoverArt::bytesByUrl(url);   // {} when url empty or not an image
        { std::lock_guard<std::mutex> lk(radio_art_mtx_); radio_art_result_ = std::move(bytes); }
        radio_art_done_.store(true);
    });
}

// Resolve + decode the cover for the currently-committed radio song into the
// Info-pane art box. Mirrors the Discord committed-song decision but runs
// independently (so the pane's art works with Discord presence off). UI thread.
// rec-cover-art: the committed-song identity parse, one home for the pane and
// the recording wiring (the RAW pane parse — deliberately NOT deinverted, so
// the fetch key matches what the pane always used).
std::string UIManager::radioSongIdentity(const std::string& np,
                                         std::string& artist, std::string& title) {
    std::string song_key;
    auto dash = np.find(" - ");
    if (dash != std::string::npos) {
        artist = np.substr(0, dash);
        title  = np.substr(dash + 3);
        auto trim = [](std::string& x){
            while (!x.empty() && (x.front()==' '||x.front()=='\t')) x.erase(x.begin());
            while (!x.empty() && (x.back()==' '||x.back()=='\t')) x.pop_back();
        };
        trim(artist); trim(title);
        if (!artist.empty() && !title.empty()) song_key = artist + "\t" + title;
    }
    return song_key;
}

// rec-cover-art: the pickup half of the radio-art machinery, extracted
// VERBATIM from refreshRadioArt so the recording wiring can consume fetch
// results with the Info pane closed. Pick up a finished fetch for the
// still-current song. A confirmed miss on a urlBySong lookup is remembered in
// the shared negative cache so the same song returning in rotation keeps the
// logo without re-querying (station-art misses are NOT neg-cached: a
// late-landing station cover should still get a chance).
void UIManager::radioArtPickup(const std::string& song_key) {
    if (radio_art_done_.exchange(false)) {
        std::vector<uint8_t> bytes; std::string forkey; bool was_station = radio_fetch_station_;
        { std::lock_guard<std::mutex> lk(radio_art_mtx_);
          bytes = std::move(radio_art_result_); forkey = radio_art_want_key_; }
        if (radio_art_thread_.joinable()) radio_art_thread_.join();
        radio_art_active_.store(false);
        if (forkey == song_key) {
            radio_bytes_ = std::move(bytes);
            radio_bytes_key_ = song_key;
            radio_bytes_resolved_ = true;
            radio_render_key_.clear();                       // force a re-decode
            if (radio_bytes_.empty() && !was_station)
                discord_art_neg_.add(song_key, port::tickMs());
            requestRedraw();                                 // cover landed -> repaint
        }
        // else: the song moved on mid-fetch; the (re)trigger handles the new one.
    }
}

// rec-cover-art: the trigger half, extracted VERBATIM from refreshRadioArt.
// (Re)start resolution on a committed song-change, or when the station
// supplies a new non-empty cover URL for the same song (iHeart digital art
// can land a tick after the title commits). While a fetch is in flight we
// hold off so the in-flight result isn't discarded; once it clears we
// re-evaluate and catch up.
void UIManager::radioArtKick(const std::string& artist, const std::string& title,
                             const std::string& song_key) {
    if (song_key.empty()) return;
    const std::string station_art = audio_.streamArtUrl();
    const bool song_changed    = (song_key != radio_bytes_key_);
    const bool station_changed = (!station_art.empty() && station_art != radio_last_station_art_);
    if ((song_changed || station_changed) && !radio_art_active_.load()) {
        radio_last_station_art_ = station_art;
        radio_bytes_key_        = song_key;
        radio_bytes_resolved_   = false;                 // show the logo floor while we fetch
        radio_render_key_.clear();
        if (!station_art.empty())
            startRadioArtFetch(artist, title, station_art);
        else if (!discord_art_neg_.hit(song_key, port::tickMs()))
            startRadioArtFetch(artist, title, "");       // song-entity lookup
        else { radio_bytes_.clear(); radio_bytes_resolved_ = true; }   // fresh miss -> floor
    }
}

// radio-art-refresh-fix: the empty-key floor reset, hoisted from
// refreshRadioArt's inline else-branch so BOTH drivers (the pane and the
// recording tick) share it. The confirmed stale-until-bounce bug existed
// precisely because the tick driver was extracted with a SUBSET of the
// original's branches: a fetch completing during a metadata dip was
// discarded while radio_bytes_key_ still named the song, so the catch-up
// re-kick (song_changed) never fired when the song returned. With all four
// operations (identity/pickup/kick/floor) shared, the drivers cannot
// diverge again.
void UIManager::radioArtFloor(const std::string& song_key) {
    if (song_key != radio_bytes_key_) {
        // No parseable now-playing (ad break / LIVE / pre-title): float on the logo.
        radio_bytes_key_ = song_key;   // ""
        radio_bytes_.clear();
        radio_bytes_resolved_ = true;
        radio_render_key_.clear();
    }
}

void UIManager::refreshRadioArt(int box_cols, int box_rows) {
    // Committed-song identity — same parse as updateScrobbler's radio branch.
    std::string artist, title;
    std::string song_key = radioSongIdentity(audio_.streamNowPlaying(), artist, title);

    radioArtPickup(song_key);
    if (!song_key.empty())
        radioArtKick(artist, title, song_key);
    else
        radioArtFloor(song_key);   // same code, new home — behavior identical

    // Decode step: real cover if we have one, else the logo floor. Cached on
    // (source | box), so no per-frame re-decode and the logo doesn't thrash.
    const bool use_logo = !(radio_bytes_resolved_ && !radio_bytes_.empty());
    const std::vector<uint8_t>& src = use_logo ? logoBytes() : radio_bytes_;
    std::string rkey = (use_logo ? std::string("\x01logo") : song_key)
                     + "|" + std::to_string(box_cols) + "x" + std::to_string(box_rows);
    if (rkey == radio_render_key_) return;
    radio_render_key_ = rkey;
    radio_art_ = cover::Rendered{};
    if (src.empty() || box_cols < 4 || box_rows < 2) return;
    cover::Rendered r = cover::render(src, box_cols, box_rows);
    if (!r.ok) return;   // failed render -> radio_art_ stays {} (no art); colours alloc at the blit
    radio_art_ = std::move(r);
}

void UIManager::drawTrackInfo() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    // Which track to show: cursored row if browsing the playlist, else the playing
    // row (stream-aware), else the last-known index for the nothing-playing floor.
    std::size_t idx;
    if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
        idx = (std::size_t)pl_cursor_;
    } else if (auto r = nowPlayingRow()) {
        idx = *r;
    } else {
        idx = playlist_.current();   // nothing playing / queue-launched stream: last-known index
    }

    // Header
    std::string hdr = tag_edit_mode_
        ? " Track Info  [Enter:save  Esc:cancel  Up/Dn:field] "
        : " Track Info  [i:close  e:edit tags] ";
    hdr.resize((size_t)cols, ' ');
    wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
    wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);

    if (playlist_.empty()) {
        wattron(w, COLOR_PAIR(CP_DIM));
        mvwaddstr(w, rows/2, 2, "No track selected");
        wattroff(w, COLOR_PAIR(CP_DIM));
    } else {
        const auto& entry = playlist_.at(idx);
        const std::string& path = entry.path;

        // Use cached metadata from PlaylistManager — no file open needed
        // For currently playing track, use AudioManager's richer TrackInfo cache
        const TrackInfo* ct_ptr = nullptr;
        if (entry.path == audio_.currentTrack().path) {
            ct_ptr = &audio_.currentTrack();
            info_cached_path_.clear();  // invalidate cache when playing track shown
        } else {
            // For non-playing tracks: read TagLib once and cache by path
            if (info_cached_path_ != path) {
                info_cached_track_ = TrackInfo{};
                info_cached_track_.path         = path;
                info_cached_track_.title        = entry.display_title;
                info_cached_track_.duration_sec = entry.duration_sec;
                if (!isCDTrackPath(path)) {
                    try {
#ifdef _WIN32
                        auto wp = utf8_to_wide(path);
                        TagLib::FileRef ref(wp.c_str(), true, TagLib::AudioProperties::Fast);
#else
                        TagLib::FileRef ref(path.c_str(), true, TagLib::AudioProperties::Fast);
#endif
                        if (!ref.isNull()) {
                            if (auto* tag = ref.tag(); tag) {
                                info_cached_track_.title   = sanitizeForDisplay(tag->title().to8Bit(true));
                                info_cached_track_.artist  = sanitizeForDisplay(tag->artist().to8Bit(true));
                                info_cached_track_.album   = sanitizeForDisplay(tag->album().to8Bit(true));
                                info_cached_track_.genre   = sanitizeForDisplay(tag->genre().to8Bit(true));
                                if (!tag->comment().isEmpty()) {
                                    std::string c = sanitizeForDisplay(tag->comment().to8Bit(true));
                                    if (c.size() > 80) c = c.substr(0, 77) + "...";
                                    info_cached_track_.comment = c;
                                }
                                info_cached_track_.year      = (int)tag->year();
                                info_cached_track_.track_num = (int)tag->track();
                            }
                            if (auto* ap = ref.audioProperties(); ap) {
                                info_cached_track_.bitrate_kbps = ap->bitrate();
                                info_cached_track_.sample_rate  = ap->sampleRate();
                                info_cached_track_.channels     = ap->channels();
                            }
                            // TagLib reports the legacy stsd channelcount for AAC (often
                            // hardcoded to 2 even for mono); prefer the true ASC count.
                            if (int real = mp4AacChannelCount(path); real > 0)
                                info_cached_track_.channels = real;
                        }
                    } catch (...) {}
                }
                info_cached_path_ = path;  // mark cache valid
            }
            ct_ptr = &info_cached_track_;
        }
        const TrackInfo& ct = *ct_ptr;

        // Collect field/value pairs
        struct Field { std::string label; std::string value; };
        std::vector<Field> fields;

        // In edit mode always show all 5 editable fields (even if empty)
        // so user can type into them. Outside edit mode skip empty fields.
        auto add = [&](const char* label, const std::string& val) {
            // Check if this is an editable field
            static const char* editable_labels[] = {"Title","Artist","Album","Genre","Year"};
            bool is_editable = false;
            for (auto* el : editable_labels) if (std::string(label) == el) { is_editable = true; break; }
            if (!val.empty() || (tag_edit_mode_ && is_editable))
                fields.push_back({label, val});
        };
        auto addInt = [&](const char* label, int val, const std::string& suffix = "") {
            if (val > 0) fields.push_back({label, std::to_string(val) + suffix});
        };

        add("Title",   ct.title);
        add("Artist",  ct.artist);
        add("Album",   ct.album);
        add("Genre",   ct.genre);
        add("Comment", ct.comment);
        // Year: show as string so add() handles the empty-in-edit-mode logic
        if (tag_edit_mode_)
            add("Year", ct.year > 0 ? std::to_string(ct.year) : "");
        else
            addInt("Year", ct.year);
        addInt("Track",       ct.track_num);
        addInt("Duration",    ct.duration_sec,  "s");
        addInt("Bitrate",     ct.bitrate_kbps,  " kbps");
        addInt("Sample Rate", ct.sample_rate,    " Hz");
        addInt("Channels",    ct.channels);

        // BPM — from live detection if this is the current track
        {
            const auto& ct = audio_.currentTrack();
            int live_bpm = audio_.currentBpm();
            if (ct.path == path && live_bpm > 0)
                addInt("BPM", live_bpm);
            else if (ct.path == path && audio_.state() != PlaybackState::Stopped)
                add("BPM", "detecting...");
            // ReplayGain tag
            if (ct.path == path && ct.replaygain_db != 0.0f) {
                char rg[16];
                std::snprintf(rg, sizeof(rg), "%+.2f dB", ct.replaygain_db);
                add("ReplayGain", rg);
            }
        }

        // File info
        add("File", sanitizeForDisplay(fs::path(path).filename().string()));
        try {
            auto sz = fs::file_size(path);
            std::string szstr;
            if (sz >= 1024*1024)
                szstr = std::to_string(sz / (1024*1024)) + "." +
                        std::to_string((sz % (1024*1024)) / (1024*102)) + " MB";
            else
                szstr = std::to_string(sz / 1024) + " KB";
            add("Size", szstr);
        } catch (...) {}
        add("Path", sanitizeForDisplay(path));

        // Play statistics — not applicable for CD tracks (volatile, not saved)
        if (!isCDTrackPath(path)) {
            auto it = config_.track_stats.find(path);
            if (it != config_.track_stats.end()) {
                addInt("Times Played", it->second.play_count);
                if (it->second.last_played > 0) {
                    char tsbuf[32];
                    std::tm tmbuf{}; localtimeSafe(it->second.last_played, tmbuf); std::tm* tm = &tmbuf;
                    std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M", tm);
                    add("Last Played", tsbuf);
                }
            } else {
                add("Times Played", "never");
            }
        }

        // ── Cover art (top-left half-block box) + metadata reflow ──
        // Box ~22 cols wide (square-ish given the 2:1 cell aspect), clamped so the
        // metadata keeps room beside it; skipped on a too-small pane or in tag-edit
        // mode. Rendered + colour-allocated once per (track, box) - cached.
        const int art_x = 1, art_y = 2;
        const int box_cols = std::clamp(22, 4, cols - 26);
        const int box_rows = std::clamp(11, 2, rows - 4);
        // Live-stream item shown while it's the playing stream: resolve radio art
        // (station cover / song lookup / logo floor). Everything else: local-file
        // embedded art. Both feed the same half-block box below.
        const bool radio_item = audio_.streamMode() && path == audio_.streamUrl();
        if (radio_item) refreshRadioArt(box_cols, box_rows);
        else            refreshInfoArt(path, box_cols, box_rows);
        const cover::Rendered&    art_r = radio_item ? radio_art_       : info_art_;
        const std::string&        key   = radio_item ? radio_render_key_ : info_art_key_;

        // Allocate curses pairs only when the global table doesn't already hold this
        // image's colours. A pair index is valid only while the palette holds the
        // colours it was allocated for; art_pairs_key_ records who owns it now.
        if (art_r.ok && key != art_pairs_key_) {
            art_pairs_.clear();
            if (allocArtColorPairs(art_r, art_pairs_)) art_pairs_key_ = key;
            else                                       art_pairs_key_.clear();
        }
        const bool has_art    = art_r.ok && !tag_edit_mode_ && !art_pairs_.empty();
        const int  art_cols   = has_art ? art_r.cols : 0;
        const int  art_rows   = has_art ? art_r.rows : 0;
        const int  art_bottom = art_y + art_rows;      // first row below the art band
        if (has_art) {
            for (int cy = 0; cy < art_rows; ++cy)
                for (int cx = 0; cx < art_cols; ++cx) {
                    cchar_t cc; wchar_t s[2] = { (wchar_t)0x2580, 0 };   // ▀ UPPER HALF BLOCK
                    setcchar(&cc, s, A_NORMAL,
                             art_pairs_[(size_t)cy * art_cols + cx], nullptr);
                    mvwadd_wch(w, art_y + cy, art_x + cx, &cc);
                }
        }
        const int meta_right_x = has_art ? (art_x + art_cols + 2) : 0;

        // Render label: value pairs
        // Editable fields: Title(0) Artist(1) Album(2) Genre(3) Year(4)
        static const char* editable[] = {"Title","Artist","Album","Genre","Year"};
        const int label_w = 13;
        int row = 2;
        for (const auto& f : fields) {
            if (row >= rows - 1) break;

            // Position: right of the art within its vertical band, then full-width
            // below it (or from col 0 when there's no art).
            const int base_x = (has_art && row < art_bottom) ? meta_right_x : 0;

            // Check if this field is editable
            int edit_idx = -1;
            if (!path.empty() && !isCDTrackPath(path)) {
                for (int ei = 0; ei < 5; ++ei)
                    if (f.label == editable[ei]) { edit_idx = ei; break; }
            }
            bool is_selected = tag_edit_mode_ && edit_idx == tag_edit_field_;

            // Label
            wattron(w, is_selected ? (COLOR_PAIR(CP_SELECTED)|A_BOLD)
                                   : (COLOR_PAIR(CP_TITLE)|A_BOLD));
            std::string lbl = "  " + f.label + ": ";
            lbl.resize((size_t)(label_w + 2), ' ');
            mvwaddnstr(w, row, base_x, lbl.c_str(), label_w + 2);
            wattroff(w, is_selected ? (COLOR_PAIR(CP_SELECTED)|A_BOLD)
                                    : (COLOR_PAIR(CP_TITLE)|A_BOLD));

            // Value
            const int val_x = base_x + label_w + 2;
            int val_w = cols - val_x - 1;
            if (val_w > 0) {
                std::string val;
                if (tag_edit_mode_ && edit_idx >= 0)
                    val = tag_edit_values_[edit_idx];  // show live edit buffer
                else
                    val = f.value;

                if (is_selected) {
                    // Show edit cursor at end
                    wattron(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
                    // Tail + cursor measured in DISPLAY columns (reserve 1 col for the
                    // cursor) so a multibyte value scrolls/positions correctly.
                    std::string disp = val;
                    if (dispWidth(disp) > val_w - 1) disp = truncateToWidthRight(disp, val_w - 1);
                    std::wstring wdisp = utf8_to_wide(padToWidth(disp, val_w));
                    mvwaddnwstr(w, row, val_x, wdisp.c_str(), (int)wdisp.size());
                    // Blink cursor position
                    int cur_col = std::min(dispWidth(val), val_w - 1);
                    mvwaddch(w, row, val_x + cur_col, '_');
                    wattroff(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
                } else {
                    wattron(w, edit_idx >= 0 && !tag_edit_mode_
                              ? (COLOR_PAIR(CP_DIM)|A_BOLD) : COLOR_PAIR(CP_DIM));
                    std::wstring wval = utf8_to_wide(scrollToWidth(val, val_w, text_scroll_offset_));
                    mvwaddnwstr(w, row, val_x, wval.c_str(), (int)wval.size());
                    wattroff(w, edit_idx >= 0 && !tag_edit_mode_
                               ? (COLOR_PAIR(CP_DIM)|A_BOLD) : COLOR_PAIR(CP_DIM));
                }
            }
            ++row;
            if (has_art && row == art_bottom) ++row;   // 1-row gap below the art band
        }

        // Hint when not in edit mode and file is editable
        if (!tag_edit_mode_ && !path.empty() &&
            !isCDTrackPath(path) && row < rows - 1) {
            wattron(w, COLOR_PAIR(CP_STATUS_OK));
            mvwaddnstr(w, rows-2, 2, "e - edit tags", cols-3);
            wattroff(w, COLOR_PAIR(CP_STATUS_OK));
        }

        // Separator before file info
        // (already integrated above)
    }

    if (config_.awesome_mode) {
        panelFrame(w, hdr, focus_ == Pane::Playlist, L'\uf129');
    } else {
        // Border
        wattron(w, COLOR_PAIR(CP_BORDER));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(CP_BORDER));
        // Redraw header over border
        wattron(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
        mvwaddnstr(w, 0, 0, hdr.c_str(), cols);
        wattroff(w, COLOR_PAIR(CP_FOCUSED) | A_BOLD);
    }
}

void UIManager::drawProgress() {
    werase(win_progress_);
    int cols; { int _r; getmaxyx(win_progress_, _r, cols); (void)_r; }
    double pos = audio_.positionSec();
    double dur = audio_.durationSec();
    if (audio_.cdMode()) {
        pos = (double)audio_.cdPositionSec();
        dur = (double)audio_.cdDurationSec();
    }
    const auto& track = audio_.currentTrack();

    // Time string: elapsed / total  OR  -remaining / total
    std::string ts;
    if (show_remaining_ && dur > 0) {
        double rem = dur - pos;
        ts = "-" + formatTime((int)rem) + " / " + formatTime((int)dur);
    } else {
        ts = formatTime((int)pos) + " / " + formatTime((int)dur);
    }

    std::string meta;
    if (audio_.cdMode()) {
        meta = "1411 kbps  44.1 kHz  stereo  CD";
        // Live BPM is detected from the playback stream just like file mode; show
        // it here too (the CD meta string above is otherwise fixed Red Book values).
        if (audio_.currentBpm() > 0)
            meta += "  " + std::to_string(audio_.currentBpm()) + " bpm";
        else if (audio_.state() == PlaybackState::Playing)
            meta += "  bpm:...";
    } else {
    if (track.bitrate_kbps > 0 || audio_.bitrateKbps() > 0) {
        int br = audio_.bitrateKbps();
        if (br <= 0) br = track.bitrate_kbps;
        meta += std::to_string(br) + " kbps";
    }
    if (track.sample_rate  > 0) {
        if (!meta.empty()) meta += "  ";
        meta += std::to_string(track.sample_rate/1000)+"."+
                std::to_string((track.sample_rate%1000)/100)+" kHz";
    }
    if (track.channels > 0) {
        if (!meta.empty()) meta += "  ";
        meta += (track.channels==1?"mono":"stereo");
    }
    if (audio_.currentBpm() > 0) {
        if (!meta.empty()) meta += "  ";
        meta += std::to_string(audio_.currentBpm()) + " bpm";
    } else if (audio_.state() == PlaybackState::Playing) {
        if (!meta.empty()) meta += "  ";
        meta += "bpm:...";
    }
    }
    {
        int vp = (int)(audio_.volume() * 100.0f + 0.5f);
        if (!meta.empty()) meta += "  ";
        meta += "vol:" + std::to_string(vp) + "%";
    }
    int bw = cols - (int)ts.size() - (int)meta.size() - 6;
    if (bw < 4) bw = 4;
    int filled = (dur > 0) ? (int)((pos/dur)*bw) : 0;
    filled = std::clamp(filled, 0, bw);
    // Stream status readouts — portable since slice 2 (the whole radio path
    // runs on Linux); the _WIN32 gate here was slice-1 scaffolding and left
    // Linux drawing the file-mode bar (blank 0:00 + bpm) over a live stream.
    if (audio_.streamConnecting()) {
        // Backgrounded connect in progress — make the current operation obvious.
        std::string left  = "Negotiating Radio Stream...";
        std::string right = "vol:" + std::to_string((int)(audio_.volume()*100.0f+0.5f)) + "%";
        int avail = cols - (int)right.size() - 3;
        if (avail < 1) avail = 1;
        if ((int)left.size() > avail) left = left.substr(0, (size_t)avail);
        wattron(win_progress_, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwaddstr(win_progress_, 0, 1, left.c_str());
        wattroff(win_progress_, A_BOLD);
        mvwaddstr(win_progress_, 0, cols - (int)right.size() - 1, right.c_str());
        wattroff(win_progress_, COLOR_PAIR(CP_TITLE));
        return;
    }
    if (audio_.streamMode()) {
        // Live stream: no position/duration. Repurpose this row for the ICY
        // now-playing title, a [LIVE]/[BUFFERING] marker + volume, and a KITT
        // scanner sweeping the idle gap between them. It all draws in this single
        // stream-bar pass (one wnoutrefresh by the caller), so the scanner and the
        // title can never fight for the region -> no flicker.
        std::string title = audio_.streamNowPlaying();
        std::string right = audio_.streamBuffering() ? "[BUFFERING]" : "[LIVE]";
        right += "  vol:" + std::to_string((int)(audio_.volume()*100.0f+0.5f)) + "%";
        std::string left = title.empty() ? "(live stream)" : title;

        const int right_w = (int)right.size();          // ASCII -> byte width == display width
        const int right_x = cols - right_w - 1;         // first col of the right block
        int left_cap = right_x - 2;                     // keep >=1 blank col before the right block
        if (left_cap < 0) left_cap = 0;
        if (dispWidth(left) > left_cap) left = truncateToWidth(left, left_cap);   // keep the head
        const int left_w   = dispWidth(left);
        const int left_end = 1 + left_w;                // first free col past the title

        wattron(win_progress_, COLOR_PAIR(CP_TITLE));
        if (left_w > 0) {
            std::wstring wl = utf8_to_wide(left);
            mvwaddnwstr(win_progress_, 0, 1, wl.c_str(), (int)wl.size());
        }
        if (right_x >= left_end)
            mvwaddstr(win_progress_, 0, right_x, right.c_str());
        wattroff(win_progress_, COLOR_PAIR(CP_TITLE));

        // Scanner track = free cells between the title and the right block, 1-col
        // padded each side so the bright head never touches text/tag. If the title
        // fills the gap (long song, narrow window) there's no room -> draw nothing,
        // exactly the graceful-collapse the spectrum strip uses.
        const int track_x0 = left_end + 1;
        const int track_x1 = right_x - 2;               // inclusive
        const int track_w  = track_x1 - track_x0 + 1;
        if (track_w >= 4) {
            // Advance on a wall-clock step so the sweep speed is independent of the
            // draw cadence / keypress jitter (rides the ~80ms heartbeat; ~1 cell/tick).
            using namespace std::chrono;
            auto now = steady_clock::now();
            if (scanner_last_.time_since_epoch().count() == 0) scanner_last_ = now;
            long steps = (long)(duration_cast<milliseconds>(now - scanner_last_).count()
                                / kScannerStepMs);
            if (scanner_pos_ >= track_w) { scanner_pos_ = track_w - 1; scanner_dir_ = -1; }
            for (long s = 0; s < steps; ++s) {
                scanner_pos_ += scanner_dir_;
                if      (scanner_pos_ >= track_w - 1) { scanner_pos_ = track_w - 1; scanner_dir_ = -1; }
                else if (scanner_pos_ <= 0)           { scanner_pos_ = 0;           scanner_dir_ =  1; }
            }
            if (steps > 0) scanner_last_ = now;
            if (scanner_pos_ < 0) scanner_pos_ = 0;      // window shrank since last frame

            // Bright head (viz_peak) + a long gradient tail (viz_high -> mid -> low)
            // trailing kScannerTail cells in the travel direction. viz pairs paint
            // solid theme-coloured cells, so each palette gets its own scanner and it
            // rhymes with the spectrum.
            for (int i = 0; i < track_w; ++i) {
                const int behind = (scanner_dir_ > 0) ? (scanner_pos_ - i) : (i - scanner_pos_);
                if (behind < 0 || behind > kScannerTail) continue;   // ahead of head / past the tail
                wchar_t g; short pair;
                if (behind == 0) { g = 0x2588; pair = CP_VIZ_PEAK; }             // █ head
                else {
                    const float f = (float)behind / (kScannerTail + 1);          // 0..1 down the tail
                    if      (f < 0.34f) { g = 0x2593; pair = CP_VIZ_HIGH; }      // ▓ near
                    else if (f < 0.67f) { g = 0x2592; pair = CP_VIZ_MID;  }      // ▒ mid
                    else                { g = 0x2591; pair = CP_VIZ_LOW;  }      // ░ far
                }
                cchar_t cc; wchar_t s[2] = { g, 0 };
                setcchar(&cc, s, A_NORMAL, pair, nullptr);
                mvwadd_wch(win_progress_, 0, track_x0 + i, &cc);
            }
        }
        return;
    }
    wattron(win_progress_, COLOR_PAIR(CP_PROGRESS));
    if (config_.awesome_mode) {
        // Comet-style progress with a proportional gradient tail: a bright █ head at
        // the playhead, fading back through ▓▒░ over a span sized to how much has
        // PLAYED — so the gradient stretches across the whole track. The 2-cell head
        // stays visible even when a crossfade hands off before 100%; ahead is blank.
        // While playing it gently "breathes" 1↔2↔3 cells on a ~1.2s sine (the run loop
        // forces per-tick redraws only in Awesome mode while playing).
        int head_cells = 2;
        if (audio_.state() == PlaybackState::Playing) {
            using namespace std::chrono;
            double t = duration<double>(steady_clock::now().time_since_epoch()).count();
            double a = 0.5 * (std::sin(t * (2.0 * 3.14159265358979 / 1.2)) + 1.0);  // 0..1
            if      (a > 0.70) head_cells = 3;   // gentle breathe out
            else if (a < 0.30) head_cells = 1;   // gentle breathe in
        }
        // During a crossfade decoder_ is still the outgoing track but the swap to the
        // next track snaps position to ~0% a beat before the head visually completes.
        // Let the head ride to the end while crossfading; it releases the instant the
        // next track takes over (crossfading_ clears in the same step as the swap).
        int hfilled = audio_.isCrossfading() ? bw : filled;
        std::wstring wbar;
        wbar.reserve((size_t)bw + 2);
        wbar += L'[';
        const double played_len = (hfilled > 0) ? (double)hfilled : 1.0;
        for (int i = 0; i < bw; ++i) {
            wchar_t ch;
            if (i >= hfilled) {
                ch = L' ';                          // unplayed track (blank)
            } else {
                int    d = (hfilled - 1) - i;       // cells behind the head
                double f = (double)d / played_len;  // 0 at head → ~1 at the start
                if      (d < head_cells) ch = L'\u2588';  // █ bright (breathing) head
                else if (f < 0.18)       ch = L'\u2593';  // ▓ near trail
                else if (f < 0.48)       ch = L'\u2592';  // ▒ mid trail
                else                     ch = L'\u2591';  // ░ long faint trail
            }
            wbar += ch;
        }
        wbar += L']';
        mvwaddnwstr(win_progress_, 0, 0, wbar.c_str(), (int)wbar.size());
    } else {
        // Classic mode: the original plain ASCII bar.
        std::string bar = "[" + std::string((size_t)filled, '#')
                              + std::string((size_t)(bw - filled), '-') + "]";
        mvwaddstr(win_progress_, 0, 0, bar.c_str());
    }
    wattroff(win_progress_, COLOR_PAIR(CP_PROGRESS));
    wattron(win_progress_, COLOR_PAIR(CP_TITLE));
    waddstr(win_progress_, ("  "+ts+"   "+meta).c_str());
    wattroff(win_progress_, COLOR_PAIR(CP_TITLE));
}

void UIManager::drawCmdLine() {
    if (goto_active_) { drawGotoBar(); return; }
    werase(win_cmdline_);
    // Transient warning takes the bar for its ~5s lifetime (both platforms).
    if (!warn_msg_.empty()) {
        wattron(win_cmdline_, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        mvwaddnstr(win_cmdline_, 0, 1, warn_msg_.c_str(), screen_cols_ - 2);
        wattroff(win_cmdline_, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        wnoutrefresh(win_cmdline_);
        return;
    }
#ifndef _WIN32
    // Toast fallback (see UIManager.h) — drawn exactly like rip_status_.
    if (!status_msg_.empty()) {
        wattron(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
        mvwaddnstr(win_cmdline_, 0, 1, status_msg_.c_str(), screen_cols_ - 2);
        wattroff(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
        wnoutrefresh(win_cmdline_);
        return;
    }
#endif
    // slice 6: MB/rip status on the cmdline is common — CD lookup (^R) and rip
    // (^Y) progress now render on Linux too. On Linux this runs AFTER the toast-
    // fallback above; on Windows there is no toast-fallback block, so this is the
    // first cmdline path exactly as before (behavior unchanged).
    std::string mb_status_snap, mb_error_snap;
    {
        std::lock_guard<std::mutex> lk(mb_mutex_);   // mb_status_/mb_error_ are written by worker threads
        mb_status_snap = mb_status_;
        mb_error_snap  = mb_error_;
    }
    if (!mb_status_snap.empty()) {
        bool is_err = (mb_status_snap.find("No results") != std::string::npos
                    || mb_status_snap.find("error")      != std::string::npos
                    || mb_status_snap.find("failed")     != std::string::npos);
        wattron(win_cmdline_, COLOR_PAIR(is_err ? CP_STATUS_ERR : CP_STATUS_OK) | A_BOLD);
        mvwaddnstr(win_cmdline_, 0, 1, mb_status_snap.c_str(), screen_cols_ - 2);
        wattroff(win_cmdline_, COLOR_PAIR(is_err ? CP_STATUS_ERR : CP_STATUS_OK) | A_BOLD);
        wnoutrefresh(win_cmdline_);
        return;
    }
    if (!mb_error_snap.empty()) {
        wattron(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
        mvwaddnstr(win_cmdline_, 0, 1, ("MB: " + mb_error_snap).c_str(), screen_cols_ - 2);
        wattroff(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
        wnoutrefresh(win_cmdline_);
        return;
    }
    if (!rip_status_.empty()) {
        // Green for ripping progress, normal for errors/done
        bool is_done = (rip_status_.find("complete") != std::string::npos
                     || rip_status_.find("Output") != std::string::npos);
        bool is_err  = (rip_status_.find("error") != std::string::npos
                     || rip_status_.find("failed") != std::string::npos
                     || rip_status_.find("cancelled") != std::string::npos);
        (void)is_err;
        wattron(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | (is_done ? A_BOLD : 0));
        mvwaddnstr(win_cmdline_, 0, 1, rip_status_.c_str(), screen_cols_ - 2);
        wattroff(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | (is_done ? A_BOLD : 0));
        wnoutrefresh(win_cmdline_);
        return;
    }

    // Build a styled command bar: [key] desc pairs, cyan keys on dark bg
    // Only show the most-used commands; rest are in ? help
    struct Cmd { const char* key; const char* desc; };

    Cmd cmds_normal[] = {
        {"SPC","pause"}, {"n/p","next/prev"}, {"[/]","seek"},
        {"-/+","vol"}, {"Tab","focus"}, {"Enter","play"},
        {"?","help"}, {"^Q","quit"},
    };
    Cmd cmds_viz[] = {
        {"SPC","pause"}, {"n/p","next/prev"}, {"[/]","seek"},
        {"-/+","vol"}, {"v","back"}, {"?","help"}, {"^Q","quit"},
    };
    Cmd cmds_help[] = {
        {"j/k","scroll"}, {"?","close"}, {"^Q","quit"},
    };

    Cmd* cmds = cmds_normal;
    int  n    = (int)(sizeof(cmds_normal)/sizeof(cmds_normal[0]));
    if (right_pane_ == RightPane::Visualizer) { cmds = cmds_viz;  n = (int)(sizeof(cmds_viz)/sizeof(cmds_viz[0])); }
    if (right_pane_ == RightPane::Help)        { cmds = cmds_help; n = (int)(sizeof(cmds_help)/sizeof(cmds_help[0])); }

    int x = 1;
    for (int i = 0; i < n; ++i) {
        // Key in cyan bold
        wattron(win_cmdline_, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwaddstr(win_cmdline_, 0, x, cmds[i].key);
        x += (int)strlen(cmds[i].key);
        wattroff(win_cmdline_, COLOR_PAIR(CP_TITLE) | A_BOLD);

        // Colon + description in dim
        wattron(win_cmdline_, COLOR_PAIR(CP_DIM));
        std::string d = std::string(":") + cmds[i].desc;
        mvwaddstr(win_cmdline_, 0, x, d.c_str());
        x += (int)d.size();
        wattroff(win_cmdline_, COLOR_PAIR(CP_DIM));

        // Separator
        if (i < n-1) {
            wattron(win_cmdline_, COLOR_PAIR(CP_DIM));
            mvwaddstr(win_cmdline_, 0, x, "  ");
            x += 2;
            wattroff(win_cmdline_, COLOR_PAIR(CP_DIM));
        }
        if (x >= screen_cols_ - 2) break;
    }
}

void UIManager::drawGotoBar() {
    werase(win_cmdline_);
    std::string prompt;
    switch (input_mode_) {
        case InputMode::Goto:    prompt = " goto: ";      break;
        case InputMode::SaveM3U: prompt = " save playlist (.m3u/.m3u8/.pls/.xspf): "; break;
        case InputMode::LoadM3U: prompt = " load m3u: ";  break;
        case InputMode::StreamURL: prompt = " radio url: "; break;
        case InputMode::StreamName: prompt = " station name (optional, Enter to skip): "; break;
        case InputMode::RadioSearch: prompt = " search radio: "; break;
        case InputMode::LastfmKey:    prompt = " last.fm API key: "; break;
        case InputMode::LastfmSecret: prompt = " last.fm secret: ";  break;
        case InputMode::ListenBrainzToken: prompt = " listenbrainz token: "; break;
        case InputMode::PlaylistSearch: prompt = " search playlist: "; break;
        case InputMode::RecDir: prompt = " recordings dir: "; break;
        case InputMode::RecOffset: prompt = " split hold ms (0-5000): "; break;
    }
    wattron(win_cmdline_, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwaddstr(win_cmdline_, 0, 0, prompt.c_str());
    wattroff(win_cmdline_, COLOR_PAIR(CP_TITLE) | A_BOLD);

    int cols; { int _r; getmaxyx(win_cmdline_, _r, cols); (void)_r; }
    int input_x = (int)prompt.size();
    int input_w = cols - input_x - 1;
    std::string display = goto_input_;
    if ((int)display.size() > input_w)
        display = display.substr(display.size() - (size_t)input_w);

    wattron(win_cmdline_, COLOR_PAIR(CP_SELECTED));
    std::string field = display;
    field.resize((size_t)input_w, ' ');
    mvwaddnstr(win_cmdline_, 0, input_x, field.c_str(), input_w);
    wattroff(win_cmdline_, COLOR_PAIR(CP_SELECTED));

    int cursor_col = input_x + std::min((int)display.size(), input_w - 1);
    curs_set(1);
    wmove(win_cmdline_, 0, cursor_col);
    wnoutrefresh(win_cmdline_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Input

static std::string radioLabel(const std::string& url) {
    return PlaylistManager::streamLabel(url);   // single source of truth
}

std::string UIManager::stationLabel(const std::string& url) const {
    std::string nm = config_.radioStationName(url);
    if (!nm.empty()) return "RADIO: " + sanitizeForDisplay(nm);
    return radioLabel(url);
}

// Shared junk-metadata gate for scrobbling. Real radio tracks (e.g. "Sia -
// Unstoppable") pass; ad/station-break markers and empty/Unknown fields are
// dropped so neither Last.fm nor ListenBrainz gets polluted. Heuristic by
// nature — extend the marker list as new junk patterns turn up in the log.
// (deinvertArtist moved verbatim to StringUtils.h in stream-record R2 so the
// recorder's parseNowPlaying shares it; the :4663 caller is unchanged.)

// (looksLikeRealTrack moved verbatim to StringUtils.h in the ad-aware slice so
// the recorder's ad classification shares the scrobbler's vocabulary; the two
// callers below are unchanged.)

// Canonical track identity for scrobble dedup. iHeart sometimes relabels the SAME
// song mid-play with a different EXTINF string — moving the featured artist between
// the artist and title fields and/or changing spacing, e.g.:
//     "Pink Pantheress / Zara Larsson - Stateside"
//     "PinkPantheress - Stateside + Zara Larsson"
// Raw-string comparison reads those as two tracks and re-arms now-playing/scrobble.
// We collapse both to the same identity by: lowercasing the combined artist+title,
// splitting on featuring/separator markers into name-chunks, stripping every chunk
// to bare alphanumerics (so "pink pantheress" == "pinkpantheress"), then sorting the
// chunks (so field position doesn't matter) and joining. Identical sets -> same id.
static std::string normTrackId(const std::string& artist, const std::string& track) {
    std::string blob = artist + " - " + track;
    for (auto& c : blob) c = (char)std::tolower((unsigned char)c);

    // Replace separator/featuring markers with a sentinel newline. Longest first so
    // multi-char markers win over their substrings.
    static const char* seps[] = {
        " featuring ", " feat. ", " feat ", " ft. ", " ft ", " with ",
        "(featuring ", "(feat. ", "(feat ", "(ft. ", "(ft ",
        " vs. ", " vs ", " x ", " - ", " / ", " + ", " & ",
        "/", "+", "&", ",", "(", ")", "[", "]"
    };
    for (const char* s : seps) {
        std::string from = s;
        size_t pos = 0;
        while ((pos = blob.find(from, pos)) != std::string::npos) {
            blob.replace(pos, from.size(), "\n");
            pos += 1;
        }
    }

    // Split on the sentinel; reduce each chunk to bare [a-z0-9].
    std::vector<std::string> chunks;
    size_t i = 0;
    while (i <= blob.size()) {
        size_t e = blob.find('\n', i);
        if (e == std::string::npos) e = blob.size();
        std::string c;
        for (size_t k = i; k < e; ++k)
            if (std::isalnum((unsigned char)blob[k])) c += blob[k];
        if (!c.empty()) chunks.push_back(c);
        i = e + 1;
    }
    std::sort(chunks.begin(), chunks.end());

    std::string id;
    for (const auto& c : chunks) { id += c; id += '|'; }
    return id;
}

// Spawn a one-shot worker to resolve an album-cover URL (iTunes/Deezer) off the
// UI thread. Single in-flight at a time; the result is picked up by the deferred
// art-commit in updateScrobbler. No-op if a lookup is already running.
// Portable since slice 4 (CoverArt has been WinINet-free since group (c)).
void UIManager::startDiscordArtLookup(const std::string& artist,
                                      const std::string& album,
                                      const std::string& key,
                                      bool song) {
    if (discord_art_active_.load()) return;
    discord_art_active_.store(true);
    discord_art_done_.store(false);
    { std::lock_guard<std::mutex> lk(discord_art_mtx_);
      discord_art_key_ = key; discord_art_url_.clear(); }
    if (discord_art_thread_.joinable()) discord_art_thread_.join();  // prior worker already done
    std::string a = artist, al = album;
    discord_art_thread_ = std::thread([this, a, al, song]() {
        std::string u = song ? CoverArt::urlBySong(a, al)
                             : CoverArt::urlByText(a, al);
        { std::lock_guard<std::mutex> lk(discord_art_mtx_); discord_art_url_ = u; }
        discord_art_done_.store(true);
    });
}

void UIManager::updateScrobbler() {
    // Portable since slice 2 (the scrobble clients + MD5 signing run on both
    // platforms) — the whole-body _WIN32 gate came off with the ^B/^G key fix:
    // it was slice-1 scaffolding from the MD5-placeholder era, and it silently
    // no-op'd every scrobble/auth-commit on Linux while the login PROMPTS
    // worked (Dos-found). Discord RP's inner gates came off at slice 4
    // (Unix-socket IIpc) — the whole tick is common code now.
    static bool announced = false;
    if (!announced) {
        sclog("updateScrobbler active (session=%s)",
              config_.lastfm_session.empty() ? "EMPTY" : "set");
        announced = true;
    }

    // Commit a completed auto-poll authorization (worker filled the slots).
    if (lf_poll_done_.exchange(false)) {
        std::string sk, user;
        { std::lock_guard<std::mutex> lk(lf_poll_mtx_); sk = lf_poll_session_; user = lf_poll_user_; }
        config_.lastfm_session = sk;
        config_.lastfm_user    = user;
        config_.lastfm_pending.clear();
        config_.save();
        lf_poll_active_.store(false);
        if (lf_poll_thread_.joinable()) lf_poll_thread_.join();
        sclog("auto-poll: committed session for %s", user.c_str());
        showTrackToast("Last.fm: logged in as " + user, "", "");
    }

    // Commit a completed ListenBrainz token validation (worker filled the slots).
    // Placed before the Last.fm early-return so it runs even with no Last.fm login.
    if (lb_validate_done_.exchange(false)) {
        std::string tok, user; bool ok;
        { std::lock_guard<std::mutex> lk(lb_validate_mtx_);
          tok = lb_validate_token_; user = lb_validate_user_; ok = lb_validate_ok_; }
        if (lb_validate_thread_.joinable()) lb_validate_thread_.join();
        if (ok) {
            config_.listenbrainz_token = tok;
            config_.listenbrainz_user  = user;
            config_.save();
            sclog("listenbrainz: token validated for %s", user.c_str());
            showTrackToast("ListenBrainz: logged in as " + user, "", "");
        } else {
            sclog("listenbrainz: token validation failed");
            showTrackToast("ListenBrainz: token invalid - press Ctrl+B to retry", "", "");
        }
    }

    if (config_.lastfm_session.empty() && config_.listenbrainz_token.empty()
        && !config_.discord_presence
        )
        return;   // nothing to drive (no scrobbler, no Discord) -> no-op

    auto st = audio_.state();
    if (st != PlaybackState::Playing && st != PlaybackState::Paused) {
        scrob_artist_.clear(); scrob_track_.clear();   // stopped -> reset
        if (discord_active_) { discord_.clearActivity(); discord_active_ = false; }
        discord_artist_.clear(); discord_track_.clear();
        return;
    }

    std::string artist, track, album;
    bool is_radio = false;
    int  pos = 0, dur = 0;

    if (audio_.streamMode()) {
        is_radio = true;
        std::string np = audio_.streamNowPlaying();    // "Artist - Title"
        auto dash = np.find(" - ");
        if (dash == std::string::npos) return;         // no parseable now-playing yet
        artist = np.substr(0, dash);
        track  = np.substr(dash + 3);
    } else if (audio_.cdMode() && audio_.cdCurrentTrack() > 0) {
        // CD: pull raw UTF-8 artist/title from the cached MusicBrainz release.
        // (The playlist's display_title is combined "Artist - Title" AND
        //  ASCII-sanitized for the terminal, so it's unsuitable for scrobbling.)
        // No MB metadata for this track -> leave artist/track empty so the guard
        // below skips: CD scrobbling requires a MusicBrainz lookup (Ctrl+R).
        int tnum = audio_.cdCurrentTrack();
        MBRelease rel;
        { std::lock_guard<std::mutex> lk(mb_mutex_); rel = mb_release_; }
        int n_phys = 0;
        for (std::size_t k2 = 0; k2 < playlist_.size(); ++k2)
            if (isCDTrackPath(playlist_.at(k2).path)) ++n_phys;
        const int cur_disc = pickDiscForTrackCount(rel, n_phys);
        const MBTrack* mt = nullptr;
        for (const auto& m : rel.tracks)
            if (m.number == tnum && m.disc == cur_disc) { mt = &m; break; }
        if (mt && !mt->title.empty()) {
            artist = mt->artist.empty() ? rel.artist : mt->artist;
            track  = mt->title;
            album  = rel.title;
            pos = (int)audio_.cdPositionSec();
            dur = (int)audio_.cdDurationSec();
        }
    } else {
        const auto& t = audio_.currentTrack();
        artist = t.artist; track = t.title; album = t.album;
        pos = (int)audio_.positionSec();
        dur = (int)audio_.durationSec();
    }
    // Trim whitespace on radio-derived fields.
    auto trim = [](std::string& x){
        while (!x.empty() && (x.front()==' '||x.front()=='\t')) x.erase(x.begin());
        while (!x.empty() && (x.back()==' '||x.back()=='\t')) x.pop_back();
    };
    trim(artist); trim(track);
    if (artist.empty() || track.empty()) {
        static bool warned = false;
        if (!warned) { sclog("skip: empty artist/track (radio=%d) np=\"%s\"",
                             (int)is_radio, audio_.streamMode() ? audio_.streamNowPlaying().c_str() : "(file)"); warned = true; }
        return;
    }
    artist = deinvertArtist(artist);   // "Shins, The" -> "The Shins" for both services
    // OS media control (osmedia-seam): publish to SMTC / MPRIS on a real change,
    // reusing these resolved values (no second assembler) and independent of the
    // Discord toggle. Radio art is the stream cover URL; local-file cover via the
    // OS surface is a v1 follow-up (title/artist/status/scrubber are full).
    // Resolve the OS-card cover BYTES, reusing existing machinery (no new lookup):
    //  - radio: radio_bytes_ from the shared radio-art machinery (station cover or
    //    the iTunes/Deezer song lookup - the same the pane/recorder/Discord use),
    //    driven each tick above; resolved+non-empty means it belongs to this song.
    //  - local file: the embedded picture, extracted ONCE per file (path-cached).
    static const std::vector<uint8_t> kNoArt;
    const std::vector<uint8_t>* art_bytes = &kNoArt;
    std::string art_url;
    if (audio_.streamMode()) {
        if (radio_bytes_resolved_ && !radio_bytes_.empty()) art_bytes = &radio_bytes_;
    } else {
        const std::string& p = audio_.currentTrack().path;
        if (p != media_art_path_) {   // extract once per file, not per tick
            media_art_path_  = p;
            media_art_cache_ = p.empty() ? std::vector<uint8_t>{} : extractEmbeddedArt(p);
        }
        if (!media_art_cache_.empty()) art_bytes = &media_art_cache_;
    }
    publishMedia(artist, track, album, art_url, *art_bytes, pos, dur);
    // Discord Rich Presence — same resolved metadata, independent of scrobbling.
    // Portable since slice 4 (Unix-socket IIpc twin).
    if (config_.discord_presence) {
        // iHeart digital cover (empty in raw mode / on ad breaks -> logo). Tracked so a
        // cover that lands a tick after the track commits still refreshes the presence.
        std::string radio_art = audio_.streamMode() ? audio_.streamArtUrl() : std::string();
        if (artist != discord_artist_ || track != discord_track_ || discord_force_update_
            || radio_art != discord_radio_art_) {
            discord_radio_art_ = radio_art;
            discord_artist_ = artist; discord_track_ = track; discord_album_ = album;
            long nowt = (long)std::time(nullptr);
            discord_start_ = (pos > 0) ? (nowt - pos) : nowt;   // anchor the elapsed bar
            std::string det = track;
            std::string sta = album.empty() ? artist : (artist + " - " + album);

            // Cover image: file OR cd -> async iTunes/Deezer lookup by artist+album
            // (the cd scrobble branch already set album = rel.title). The Cover Art
            // Archive is unreliable here (often no front cover, esp. Discogs-sourced
            // releases) -> a bad CAA URL shows Discord's broken-image dice, so we use
            // the same proven lookup files use. radio / no album -> the logo.
            std::string image = "remoct_logo";
            const std::string key = artist + "\t" + track;
            if (!radio_art.empty()) {
                image = radio_art;                            // iHeart digital cover for the live song
            } else if (!audio_.streamMode() && !album.empty()) {
                if (key == discord_art_cache_key_ && !discord_art_cache_url_.empty())
                    image = discord_art_cache_url_;            // resolved earlier
                else
                    startDiscordArtLookup(artist, album, key); // logo now, cover when it lands
            } else if (audio_.streamMode()) {
                // Radio with no station-supplied cover (ICY always; iHeart raw mode;
                // iHeart digital during ad/boundary skew). Fall back to a song-entity
                // lookup keyed on artist+title. Logo stays until/unless one lands; a
                // confirmed miss is remembered so rotation repeats don't re-query.
                if (key == discord_art_cache_key_ && !discord_art_cache_url_.empty())
                    image = discord_art_cache_url_;            // resolved earlier this session
                else if (!discord_art_neg_.hit(key, port::tickMs()))
                    startDiscordArtLookup(artist, track, key, /*song=*/true);
            }

            discord_.setActivity(det, sta, discord_start_, image, "RE-MOCT");
            discord_active_       = true;
            discord_force_update_ = false;
        }

        // Deferred art commit: when an async file lookup finishes, swap the logo
        // for the real cover on the still-current track.
        if (discord_art_done_.exchange(false)) {
            std::string url, forkey;
            { std::lock_guard<std::mutex> lk(discord_art_mtx_);
              url = discord_art_url_; forkey = discord_art_key_; }
            if (discord_art_thread_.joinable()) discord_art_thread_.join();
            discord_art_active_.store(false);
            const std::string curkey = discord_artist_ + "\t" + discord_track_;
            if (forkey == curkey) {
                if (!url.empty()) {
                    discord_art_cache_key_ = forkey;
                    discord_art_cache_url_ = url;
                    std::string sta = discord_album_.empty() ? discord_artist_
                                    : (discord_artist_ + " - " + discord_album_);
                    discord_.setActivity(discord_track_, sta, discord_start_, url, "RE-MOCT");
                } else if (audio_.streamMode()) {
                    // Radio lookup came back empty for the still-current track: remember
                    // the miss (time-bounded - a transient failure self-heals after the
                    // TTL) so the same song returning soon keeps the logo without
                    // re-hitting iTunes/Deezer. Logo is already on screen.
                    discord_art_neg_.add(forkey, port::tickMs());
                }
            } else if (!audio_.streamMode() && !discord_album_.empty()) {
                // The finished lookup was for a since-skipped track — retry for the
                // one we actually landed on (unless already cached). Covers file+cd.
                if (!(curkey == discord_art_cache_key_ && !discord_art_cache_url_.empty()))
                    startDiscordArtLookup(discord_artist_, discord_album_, curkey);
            }
        }
    }

    const std::string& k  = config_.lastfm_key;
    const std::string& s  = config_.lastfm_secret;
    const std::string& sk = config_.lastfm_session;

    // New track? -> reset state + send now-playing in the background.
    // Compare on canonical identity, not the raw string: iHeart relabels the same
    // song mid-play (featured artist hops fields / spacing changes), which would
    // otherwise reset the timer, re-send now-playing, and risk a duplicate scrobble.
    // A relabel keeps the FIRST-seen (usually cleaner) strings and the original timer.
    std::string normid = normTrackId(artist, track);
    bool same_track = !scrob_normid_.empty() && normid == scrob_normid_;
    if (!same_track && (artist != scrob_artist_ || track != scrob_track_)) {
        scrob_artist_ = artist; scrob_track_ = track; scrob_album_ = album;
        scrob_normid_ = normid;
        scrob_start_  = (long)std::time(nullptr);
        scrob_done_   = false;
        if (!looksLikeRealTrack(artist, track)) {   // ad/station junk -> skip both services
            sclog("skip now-playing (junk): %s - %s", artist.c_str(), track.c_str());
            return;
        }
        sclog("now-playing: %s - %s (radio=%d dur=%d)", artist.c_str(), track.c_str(), (int)is_radio, dur);
        if (!sk.empty())
            std::thread([=]{ LastFm::updateNowPlaying(k, s, sk, artist, track, album); }).detach();
        if (!config_.listenbrainz_token.empty()) {
            std::string lbtok = config_.listenbrainz_token;
            std::thread([=]{ ListenBrainz::playingNow(lbtok, artist, track, album); }).detach();
        }
        return;
    }

    if (scrob_done_) return;

    // Scrobble threshold: >30s track, played >=50% or 4min (files);
    // radio has no known duration, so use a conservative 90s of listening.
    long elapsed = (long)std::time(nullptr) - scrob_start_;
    bool ready = false;
    if (is_radio) {
        ready = (elapsed >= 90);
    } else if (dur > 30) {
        int threshold = dur / 2;
        if (threshold > 240) threshold = 240;
        ready = (pos >= threshold);
    }
    if (ready) {
        scrob_done_ = true;
        if (!looksLikeRealTrack(artist, track)) {   // ad/station junk -> skip both services
            sclog("skip scrobble (junk): %s - %s", artist.c_str(), track.c_str());
            return;
        }
        sclog("scrobble: %s - %s (elapsed=%ld pos=%d dur=%d)", artist.c_str(), track.c_str(), elapsed, pos, dur);
        long ts = scrob_start_;
        bool chosen = !is_radio;          // radio: chosenByUser=0
        std::string a = artist, t = track, al = album;
        if (!sk.empty())
            std::thread([=]{ LastFm::scrobble(k, s, sk, a, t, ts, chosen, al); }).detach();
        if (!config_.listenbrainz_token.empty()) {
            std::string lbtok = config_.listenbrainz_token;
            std::thread([=]{ ListenBrainz::submitSingle(lbtok, a, t, ts, al); }).detach();
        }
    }
}

void UIManager::startLastfmPoll(const std::string& token) {
    // Portable since slice 2 (getSession = seam HTTP + vendored MD5).
    if (lf_poll_active_.exchange(true)) return;        // already polling
    lf_poll_done_.store(false);
    if (lf_poll_thread_.joinable()) lf_poll_thread_.join();
    std::string key = config_.lastfm_key, secret = config_.lastfm_secret, tok = token;
    lf_poll_thread_ = std::thread([this, key, secret, tok]() {
        // Retry getSession for ~60s; the worker NEVER touches config_ — it only
        // drops the result into guarded slots for the UI thread to commit.
        for (int attempt = 0; attempt < 20 && lf_poll_active_.load(); ++attempt) {
            for (int w = 0; w < 30 && lf_poll_active_.load(); ++w)   // ~3s, responsive
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!lf_poll_active_.load()) break;
            std::string sk, user;
            if (LastFm::getSession(key, secret, tok, sk, user) && !sk.empty()) {
                std::lock_guard<std::mutex> lk(lf_poll_mtx_);
                lf_poll_session_ = sk;
                lf_poll_user_    = user;
                lf_poll_done_.store(true);
                break;
            }
        }
        lf_poll_active_.store(false);
    });
}

void UIManager::startListenBrainzValidate(const std::string& token) {
    // Portable since slice 2 (validate-token = seam HTTP, no signing).
    if (lb_validate_active_.exchange(true)) return;     // one validation already in flight
    lb_validate_done_.store(false);
    if (lb_validate_thread_.joinable()) lb_validate_thread_.join();
    std::string tok = token;
    lb_validate_thread_ = std::thread([this, tok]() {
        // Network call lives entirely off the UI thread; the worker NEVER touches
        // config_ — it drops the result into guarded slots for the UI thread to commit.
        std::string user;
        bool ok = ListenBrainz::validateToken(tok, user);
        {
            std::lock_guard<std::mutex> lk(lb_validate_mtx_);
            lb_validate_token_ = tok;
            lb_validate_user_  = ok ? user : std::string();
            lb_validate_ok_    = ok;
        }
        lb_validate_done_.store(true);
        lb_validate_active_.store(false);
    });
}

void UIManager::lastfmBeginAuth() {
    std::string token, url;
    if (LastFm::requestToken(config_.lastfm_key, config_.lastfm_secret, token, url)) {
        config_.lastfm_pending = token;   // persisted so it survives an app restart
        config_.save();
        sclog("beginAuth: token acquired, browser opened, auto-poll started");
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        // xdg-open twin — same fire-and-forget contract as ShellExecuteA "open".
        // (Last.fm auth URLs are https + query params; no shell-hostile chars.)
        (void)std::system(("xdg-open '" + url + "' >/dev/null 2>&1 &").c_str());
#endif
        startLastfmPoll(token);           // auto-complete after you click allow
        showTrackToast("Last.fm: approve in browser - finishes automatically", "", "");
    } else {
        sclog("beginAuth: requestToken FAILED");
        showTrackToast("Last.fm: token request failed", "", "");
    }
}

void UIManager::handleInput(int ch) {
    // slice 6: overlay input is common. The RipConfirm modal (^Y) is live on Linux;
    // the MBSearch line is inert there (^F stays gated, so it never opens).
    if (ui_overlay_ == UIOverlay::MBSearch) { handleMBSearchInput(ch); return; }
    // ── [Rec] panel (stream-record R2) — intercepts all input when active ──
    // The rip modal's structural disambiguation, minus its empty-selection
    // guard (a single-select radio always has exactly one format chosen):
    // N/Esc first (close always works; recording continues), then the
    // recording-view keys, then settings keys, then start.
    if (ui_overlay_ == UIOverlay::RecPanel) {
        auto& rec = audio_.streamRecorder();
        if (ch == 'n' || ch == 'N' || ch == 27) {
            ui_overlay_ = UIOverlay::None;      // close; [REC] badge carries state
            redraw_needed_.store(true);
            return;
        }
        if (rec.recording()) {
            if (ch == 'x' || ch == 'X') {
                audio_.endRecording();          // finalizes the in-flight cut
                                                // + clears the drain signal
                // Session-end summary: the ads-skipped count rides the toast —
                // the Discard trust surface survives past the live panel.
                std::string sum = std::to_string(rec.cuts().size() -
                                                 (size_t)rec.adsSkipped()) + " cut(s) saved";
                if (int sk = rec.adsSkipped(); sk > 0)
                    sum += ", " + std::to_string(sk) + " ad(s) skipped";
                showTrackToast("Recording stopped", sum, "");
                redraw_needed_.store(true);     // panel flips to settings view
            }
            return;                             // settings keys inert while recording
        }
        if (ch == '1') { rec_fmt_ = RipFormat::Opus; rec_copy_ = false; redraw_needed_.store(true); return; }
        if (ch == '2') { rec_fmt_ = RipFormat::Mp3;  rec_copy_ = false; redraw_needed_.store(true); return; }
        if (ch == '3') { rec_fmt_ = RipFormat::M4a;  rec_copy_ = false; redraw_needed_.store(true); return; }
        if (ch == '4') {   // Copy (slice B): selectable only when the plugin has the tee
            if (audio_.recordingCopySupported()) rec_copy_ = true;
            redraw_needed_.store(true);
            return;
        }
        // encoder-bitrate-mode: per-row quality editor for the REC fields.
        // Up/Down focus over Opus(0)/MP3(1)/M4A(2)/Copy(3); Left/Right cycle the
        // focused row's axis; [M] flips its mode. The Copy row has no axis (inert).
        if (ch == KEY_UP || ch == KEY_DOWN) {
            rec_focus_ += (ch == KEY_DOWN) ? 1 : -1;
            if (rec_focus_ < 0) rec_focus_ = 3;
            if (rec_focus_ > 3) rec_focus_ = 0;
            redraw_needed_.store(true);
            return;
        }
        if (ch == KEY_LEFT || ch == KEY_RIGHT) {
            int dir = (ch == KEY_RIGHT) ? 1 : -1;
            if (rec_focus_ == 0) {                      // Opus: bitrate only
                config_.rec_opus_bitrate = cycleBitrate(config_.rec_opus_bitrate, dir);
            } else if (rec_focus_ == 1) {               // MP3: V-scale or CBR bitrate
                if (config_.rec_mp3_cbr)
                    config_.rec_mp3_cbr_bitrate = cycleBitrate(config_.rec_mp3_cbr_bitrate, dir);
                else
                    config_.rec_mp3 = cycleMp3Vbr(config_.rec_mp3, dir);
            } else if (rec_focus_ == 2) {               // M4A: VBR ladder or CBR bitrate
                if (config_.rec_aac_vbr)
                    config_.rec_aac_vbr_level = cycleAacVbr(config_.rec_aac_vbr_level, dir);
                else
                    config_.rec_aac_cbr_bitrate = cycleBitrate(config_.rec_aac_cbr_bitrate, dir);
            }
            redraw_needed_.store(true);
            return;
        }
        if (ch == 'm' || ch == 'M') {
            if (rec_focus_ == 0)      config_.rec_opus_vbr = !config_.rec_opus_vbr;
            else if (rec_focus_ == 1) config_.rec_mp3_cbr  = !config_.rec_mp3_cbr;
            else if (rec_focus_ == 2) config_.rec_aac_vbr  = !config_.rec_aac_vbr;
            redraw_needed_.store(true);
            return;
        }
        if (ch == 's' || ch == 'S') {
            rec_split_on_ = !rec_split_on_;
            redraw_needed_.store(true);
            return;
        }
        if (ch == 't' || ch == 'T') {   // split-trim: edit the hold (ms)
            ui_overlay_ = UIOverlay::None;
            openInputBar(InputMode::RecOffset, std::to_string(rec_offset_ms_));
            return;
        }
        if (ch == 'a' || ch == 'A') {   // ad-aware: Save <-> Discard
            rec_ads_discard_ = !rec_ads_discard_;
            redraw_needed_.store(true);
            return;
        }
        if (ch == 'd' || ch == 'D') {
            // The goto bar and an overlay don't compose (drawGotoBar returns
            // early) — close the panel, prompt, reopen on submit.
            ui_overlay_ = UIOverlay::None;
            openInputBar(InputMode::RecDir,
                         rec_dir_.empty() ? CDRipper::recordingsDir() : rec_dir_);
            return;
        }
        if (ch == 'r' || ch == 'R' || ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (!audio_.streamMode()) {         // stream died while the panel was open
                ui_overlay_ = UIOverlay::None;
                showTrackToast("Stream recording", "Start a stream first (Ctrl+U)", "");
                redraw_needed_.store(true);
                return;
            }
            RecOptions opt;
            opt.formats         = { rec_fmt_ };
            // encoder-bitrate-mode: recording reads its OWN quality set (rec_*),
            // independent of the rip knobs, so radio can be right-sized.
            opt.mp3_vbr_q       = parseMp3VbrQ(config_.rec_mp3);
            opt.mp3_cbr         = config_.rec_mp3_cbr;
            opt.mp3_cbr_bitrate = std::clamp(config_.rec_mp3_cbr_bitrate, 6000, 510000);
            opt.opus_bitrate    = std::clamp(config_.rec_opus_bitrate, 6000, 510000);
            opt.opus_vbr        = config_.rec_opus_vbr;
            opt.aac_vbr         = config_.rec_aac_vbr;
            opt.aac_vbr_level   = std::clamp(config_.rec_aac_vbr_level, 1, 5);
            opt.aac_cbr_bitrate = std::clamp(config_.rec_aac_cbr_bitrate, 6000, 510000);
            opt.split_on_meta   = rec_split_on_;
            opt.split_offset_ms = rec_offset_ms_;
            opt.ads_discard     = rec_ads_discard_;
            // Copy (slice B): the wrapper wires the pull + arms the tee; the
            // selection can only be true when the capability probe passed,
            // but a plugin swap mid-session falls back honestly here.
            opt.copy_mode       = rec_copy_ && audio_.recordingCopySupported();
            std::string st = stationLabel(audio_.streamUrl());
            if (st.rfind("RADIO: ", 0) == 0) st = st.substr(7);
            // abi-cluster: start through the wrapper so the keep-draining
            // signal travels with the recorder lifecycle.
            if (audio_.beginRecording(opt, st, rec_dir_.empty() ? CDRipper::recordingsDir()
                                                                : rec_dir_))
                rec.onTitle(audio_.streamNowPlaying());   // seed the first label
            else
                showTrackToast("Recording failed", rec.lastError(), "");
            redraw_needed_.store(true);         // panel flips to recording view
            return;
        }
        return;   // swallow everything else — the overlay is modal
    }
    // ── batch-r128: the ^O tag/force prompt intercepts until answered ────────
    if (rgscan_prompt_) {
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 'f' || ch == 'F') {
            const bool force = (ch == 'f' || ch == 'F');
            rgscan_prompt_ = false;
            if (gain_scan_.start(rgscan_dir_, force)) {
                rip_status_ = std::string("ReplayGain scan started") +
                              (force ? " (force re-tag)" : "") + "...";
            } else {
                rip_status_ = "ReplayGain scan: could not read the folder.";
            }
            rip_msg_ticks_ = 0;
            redraw_needed_.store(true);
            return;
        }
        if (ch == 27) {
            rgscan_prompt_ = false;
            rip_status_.clear();
            redraw_needed_.store(true);
            return;
        }
        rip_msg_ticks_ = 0;   // keep the prompt line alive while undecided
        return;               // modal until answered
    }
    // ── Rip mode selection modal — intercepts all input when active ─────────
    // Input disambiguation is STRUCTURAL (rip-format-select): N/Esc first
    // (cancel always works), digits toggle and return inside their own case
    // (can never commit), letters carry the empty-selection guard (can never
    // toggle, and commit is inert — not "commits with empty list" — while
    // nothing is checked; the dimmed rows + red hint explain why).
    if (ui_overlay_ == UIOverlay::RipConfirm) {
        // Cancel — always allowed, selection state irrelevant.
        if (ch == 'n' || ch == 'N' || ch == 27) {
            ui_overlay_ = UIOverlay::None;
            redraw_needed_.store(true);
            return;
        }
        // Digit toggles: '1'..'0'+kRipFormatCount flip a row and stay open.
        if (ch > '0' && ch <= '0' + kRipFormatCount) {
            RipFormat f = kRipFormats[ch - '1'].id;
            auto it = std::find(rip_sel_.begin(), rip_sel_.end(), f);
            if (it != rip_sel_.end()) rip_sel_.erase(it);
            else                      rip_sel_.push_back(f);
            redraw_needed_.store(true);
            return;
        }
        // encoder-bitrate-mode: per-row quality editor. Up/Down move the focus
        // cursor; Left/Right cycle the focused row's active axis; [M] flips its
        // CBR/VBR mode. Inert on FLAC/WAV/WavPack (no bitrate axis).
        if (ch == KEY_UP || ch == KEY_DOWN) {
            rip_focus_ += (ch == KEY_DOWN) ? 1 : -1;
            if (rip_focus_ < 0)                 rip_focus_ = kRipFormatCount - 1;
            if (rip_focus_ >= kRipFormatCount)  rip_focus_ = 0;
            redraw_needed_.store(true);
            return;
        }
        if (ch == KEY_LEFT || ch == KEY_RIGHT) {
            int dir = (ch == KEY_RIGHT) ? 1 : -1;
            switch (kRipFormats[rip_focus_].id) {
                case RipFormat::Mp3:
                    if (config_.mp3_cbr)
                        config_.mp3_cbr_bitrate = cycleBitrate(config_.mp3_cbr_bitrate, dir);
                    else
                        config_.mp3 = cycleMp3Vbr(config_.mp3, dir);
                    redraw_needed_.store(true);
                    break;
                case RipFormat::Opus:
                    config_.opus_bitrate = cycleBitrate(config_.opus_bitrate, dir);
                    redraw_needed_.store(true);
                    break;
                case RipFormat::M4a:
                    if (config_.aac_vbr)
                        config_.aac_vbr_level = cycleAacVbr(config_.aac_vbr_level, dir);
                    else
                        config_.aac_cbr_bitrate = cycleBitrate(config_.aac_cbr_bitrate, dir);
                    redraw_needed_.store(true);
                    break;
                default: break;   // no axis on the lossless rows
            }
            return;
        }
        if (ch == 'm' || ch == 'M') {
            switch (kRipFormats[rip_focus_].id) {
                case RipFormat::Mp3:  config_.mp3_cbr  = !config_.mp3_cbr;  redraw_needed_.store(true); break;
                case RipFormat::Opus: config_.opus_vbr = !config_.opus_vbr; redraw_needed_.store(true); break;
                case RipFormat::M4a:  config_.aac_vbr  = !config_.aac_vbr;  redraw_needed_.store(true); break;
                default: break;
            }
            return;
        }
        RipMode chosen = RipMode::None;
        switch (ch) {
            case 'a': case 'A': chosen = RipMode::AccurateRip; break;
            case 'c': case 'C': chosen = RipMode::CUETools;    break;
            case 'y': case 'Y': chosen = RipMode::Local;       break;
            case 'b': case 'B': chosen = RipMode::LocalVerify; break;
            default: return;  // ignore everything else
        }
        if (rip_sel_.empty()) {           // commit keys inert at zero selected
            redraw_needed_.store(true);
            return;
        }
        ui_overlay_ = UIOverlay::None;
        if (!audio_.cdMode()) return;
        const auto& cd = audio_.cdSource();
        auto tracks = cd.tracks();
        MBRelease rel;
        std::string out_dir;
        {
            std::lock_guard<std::mutex> lk(mb_mutex_);
            rel     = mb_release_;
            out_dir = CDRipper::buildOutputDir(rel);
        }
        rip_status_    = "Initializing rip...  Output: " + out_dir;
        rip_msg_ticks_ = 0;
        redraw_needed_.store(true);
        // Build the rip request: selection normalized to TABLE order (the
        // fan-out sequence must not depend on toggle history — default set
        // always rips FLAC-then-MP3, byte-identical to pre-selection), plus
        // the config-fed quality values (defaults == the old literals).
        RipOptions opt;
        opt.formats.clear();
        for (const auto& r : kRipFormats)
            if (std::find(rip_sel_.begin(), rip_sel_.end(), r.id) != rip_sel_.end())
                opt.formats.push_back(r.id);
        opt.flac_level   = std::clamp(config_.flac_level, 0, 8);
        opt.mp3_vbr_q    = parseMp3VbrQ(config_.mp3);
        opt.mp3_cbr      = config_.mp3_cbr;
        opt.mp3_cbr_bitrate = std::clamp(config_.mp3_cbr_bitrate, 6000, 510000);
        opt.opus_bitrate = std::clamp(config_.opus_bitrate, 6000, 510000);
        opt.opus_vbr     = config_.opus_vbr;
        opt.wavpack_mode = config_.wavpack_mode == "fast"      ? 0
                         : config_.wavpack_mode == "high"      ? 2
                         : config_.wavpack_mode == "very_high" ? 3 : 1;
        opt.aac_vbr      = config_.aac_vbr;
        opt.aac_vbr_level = std::clamp(config_.aac_vbr_level, 1, 5);
        opt.aac_cbr_bitrate = std::clamp(config_.aac_cbr_bitrate, 6000, 510000);
        cd_ripper_.start(audio_, tracks, out_dir, rel, chosen, std::move(opt),
            [this](const RipProgress& p) {
                rip_status_ = p.status_msg;
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            });
        return;
    }

    // ── Convert scope chooser (convert-core) ────────────────────────────────
    if (ui_overlay_ == UIOverlay::ConvertScope) {
        if (ch == 'n' || ch == 'N' || ch == 27) {
            ui_overlay_ = UIOverlay::None; redraw_needed_.store(true); return;
        }
        if (ch == '1' && !convert_single_.empty()) {
            convert_scope_ = 1; convert_focus_ = 0;
            ui_overlay_ = UIOverlay::ConvertConfirm; redraw_needed_.store(true); return;
        }
        if (ch == '2' && !convert_src_dir_.empty()) {
            convert_scope_ = 2; convert_focus_ = 0;
            ui_overlay_ = UIOverlay::ConvertConfirm; redraw_needed_.store(true); return;
        }
        if (ch == '3' && !marked_.empty()) {
            convert_scope_ = 3; convert_focus_ = 0;
            ui_overlay_ = UIOverlay::ConvertConfirm; redraw_needed_.store(true); return;
        }
        return;   // modal
    }

    // ── Convert target picker (convert-core): single-select output format +
    //    the per-row quality editor reusing EncoderQuality.h (the [Rec] shape).
    //    Quality edits the shared rip config fields (the brief's reuse). ────────
    if (ui_overlay_ == UIOverlay::ConvertConfirm) {
        if (ch == 'n' || ch == 'N' || ch == 27) {
            ui_overlay_ = UIOverlay::None; redraw_needed_.store(true); return;
        }
        if (ch > '0' && ch <= '0' + kRipFormatCount) {   // radio-select output format
            convert_fmt_ = kRipFormats[ch - '1'].id;
            convert_focus_ = ch - '1';
            redraw_needed_.store(true); return;
        }
        if (ch == KEY_UP || ch == KEY_DOWN) {
            convert_focus_ += (ch == KEY_DOWN) ? 1 : -1;
            if (convert_focus_ < 0)                convert_focus_ = kRipFormatCount - 1;
            if (convert_focus_ >= kRipFormatCount) convert_focus_ = 0;
            redraw_needed_.store(true); return;
        }
        if (ch == KEY_LEFT || ch == KEY_RIGHT) {
            int dir = (ch == KEY_RIGHT) ? 1 : -1;
            switch (kRipFormats[convert_focus_].id) {
                case RipFormat::Flac:
                    config_.flac_level = std::clamp(config_.flac_level + dir, 0, 8);
                    redraw_needed_.store(true); break;
                case RipFormat::Mp3:
                    if (config_.mp3_cbr)
                        config_.mp3_cbr_bitrate = cycleBitrate(config_.mp3_cbr_bitrate, dir);
                    else
                        config_.mp3 = cycleMp3Vbr(config_.mp3, dir);
                    redraw_needed_.store(true); break;
                case RipFormat::Opus:
                    config_.opus_bitrate = cycleBitrate(config_.opus_bitrate, dir);
                    redraw_needed_.store(true); break;
                case RipFormat::WavPack: {
                    static const char* modes[] = { "fast", "normal", "high", "very_high" };
                    int mi = config_.wavpack_mode == "fast" ? 0 : config_.wavpack_mode == "high" ? 2
                           : config_.wavpack_mode == "very_high" ? 3 : 1;
                    mi = ((mi + dir) % 4 + 4) % 4;
                    config_.wavpack_mode = modes[mi];
                    redraw_needed_.store(true); break;
                }
                case RipFormat::M4a:
                    if (config_.aac_vbr)
                        config_.aac_vbr_level = cycleAacVbr(config_.aac_vbr_level, dir);
                    else
                        config_.aac_cbr_bitrate = cycleBitrate(config_.aac_cbr_bitrate, dir);
                    redraw_needed_.store(true); break;
                default: break;   // WAV: no axis
            }
            return;
        }
        if (ch == 'm' || ch == 'M') {
            switch (kRipFormats[convert_focus_].id) {
                case RipFormat::Mp3:  config_.mp3_cbr  = !config_.mp3_cbr;  redraw_needed_.store(true); break;
                case RipFormat::Opus: config_.opus_vbr = !config_.opus_vbr; redraw_needed_.store(true); break;
                case RipFormat::M4a:  config_.aac_vbr  = !config_.aac_vbr;  redraw_needed_.store(true); break;
                default: break;
            }
            return;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            RipOptions opt;
            opt.formats         = { convert_fmt_ };
            opt.flac_level      = std::clamp(config_.flac_level, 0, 8);
            opt.mp3_vbr_q       = parseMp3VbrQ(config_.mp3);
            opt.mp3_cbr         = config_.mp3_cbr;
            opt.mp3_cbr_bitrate = std::clamp(config_.mp3_cbr_bitrate, 6000, 510000);
            opt.opus_bitrate    = std::clamp(config_.opus_bitrate, 6000, 510000);
            opt.opus_vbr        = config_.opus_vbr;
            opt.wavpack_mode    = config_.wavpack_mode == "fast" ? 0
                                : config_.wavpack_mode == "high" ? 2
                                : config_.wavpack_mode == "very_high" ? 3 : 1;
            opt.aac_vbr         = config_.aac_vbr;
            opt.aac_vbr_level   = std::clamp(config_.aac_vbr_level, 1, 5);
            opt.aac_cbr_bitrate = std::clamp(config_.aac_cbr_bitrate, 6000, 510000);
            bool started = false;
            if (convert_scope_ == 1 && !convert_single_.empty()) {
                std::string dst = convertDstPath(convert_single_, convert_fmt_);
                started = convert_job_.startFiles({{ convert_single_, dst }}, convert_fmt_, opt);
            } else if (convert_scope_ == 2 && !convert_src_dir_.empty()) {
                started = convert_job_.startFolder(convert_src_dir_, convert_fmt_, opt);
            } else if (convert_scope_ == 3) {
                std::vector<ConvertPair> pairs;
                for (const auto& s : marked_.list())
                    pairs.push_back({ s, convertDstPath(s, convert_fmt_) });
                started = convert_job_.startFiles(std::move(pairs), convert_fmt_, opt);
                if (started) marked_.clear();   // consumed: clear the marked set
            }
            ui_overlay_ = UIOverlay::None;
            if (started) { rip_status_ = "Converting..."; rip_msg_ticks_ = 0; }
            else showTrackToast("Convert", "Nothing to convert (or a job is running)", "");
            redraw_needed_.store(true);
            return;
        }
        return;   // modal
    }

    // ? toggles help from anywhere
    // Don't let pane-toggle hotkeys fire while editing tags
    bool in_tag_edit = (right_pane_ == RightPane::TrackInfo && tag_edit_mode_);

    if (ch == 'L' && !in_tag_edit) {
        right_pane_ = (right_pane_ == RightPane::Lyrics)
                    ? RightPane::Playlist : RightPane::Lyrics;
        return;
    }

    if (ch == '?' && !in_tag_edit) {
        if (right_pane_ == RightPane::Help) {
            right_pane_ = RightPane::Playlist;
        } else {
            right_pane_ = RightPane::Help;
            help_scroll_ = 0;
        }
        return;
    }

    if (ch == 'A' && !in_tag_edit) {
        right_pane_ = (right_pane_ == RightPane::About)
                    ? RightPane::Playlist : RightPane::About;
        return;
    }

    // EQ pane
    if (right_pane_ == RightPane::EQ) {
        switch (ch) {
            case 'e': case 27:   // E freed for eject (was a pure dupe of e)
                right_pane_ = RightPane::Playlist; return;
            case 17: audio_.stop(); running_ = false; return;
            case ' ': audio_.togglePause(); return;
            case KEY_LEFT:  case 'h':
                if (eq_cursor_ > 0) { --eq_cursor_; } return;
            case KEY_RIGHT: case 'l':
                if (eq_cursor_ < AudioManager::EQ_BANDS-1) { ++eq_cursor_; } return;
            case KEY_UP:    case 'k':
                audio_.setEqGain(eq_cursor_, audio_.getEqGain(eq_cursor_) + 1.0f);
                if (!audio_.eqEnabled()) audio_.setEqEnabled(true);
                eq_preset_name_ = "";
                return;
            case KEY_DOWN:  case 'j':
                audio_.setEqGain(eq_cursor_, audio_.getEqGain(eq_cursor_) - 1.0f);
                if (!audio_.eqEnabled()) audio_.setEqEnabled(true);
                eq_preset_name_ = "";
                return;
            case 'f':   // F freed for the filetype toggle (was a pure dupe of f)
                audio_.setEqEnabled(!audio_.eqEnabled()); return;
            case '0':
                audio_.resetEq(); eq_preset_name_ = "Flat"; return;
            case '1': {
                static const float p[] = {4,5,3,-1,-2,0,2,4,4,2};
                for (int b=0;b<10;++b) audio_.setEqGain(b,p[b]);
                audio_.setEqEnabled(true); eq_preset_name_ = "Rock"; return;
            }
            case '2': {
                static const float p[] = {6,6,5,3,1,0,0,1,2,2};
                for (int b=0;b<10;++b) audio_.setEqGain(b,p[b]);
                audio_.setEqEnabled(true); eq_preset_name_ = "Bass Boost"; return;
            }
            case '3': {
                static const float p[] = {-3,-2,-1,0,2,3,4,3,2,1};
                for (int b=0;b<10;++b) audio_.setEqGain(b,p[b]);
                audio_.setEqEnabled(true); eq_preset_name_ = "Vocal"; return;
            }
            case '4': {
                audio_.resetEq(); audio_.setEqEnabled(false);
                eq_preset_name_ = "Reference"; return;
            }
            case '5': {
                static const float p[] = {-4,-2,1,2,2,1,0,-2,-4,-5};
                for (int b=0;b<10;++b) audio_.setEqGain(b,p[b]);
                audio_.setEqEnabled(true); eq_preset_name_ = "Night Mode"; return;
            }
            default: return;
        }
    }

    // Queue pane
    if (right_pane_ == RightPane::Queue) {
        switch (ch) {
            case 'Q': case 27: right_pane_ = RightPane::Playlist; return;
            case 17: audio_.stop(); running_ = false; return;
            case ' ': audio_.togglePause(); return;
            case 'j': case 'J': case KEY_DOWN:
                if (q_cursor_ < playlist_.queueSize() - 1) ++q_cursor_;
                return;
            case 'k': case 'K': case KEY_UP:
                if (q_cursor_ > 0) --q_cursor_;
                return;
            case 'd':
                if (q_cursor_ < playlist_.queueSize()) {
                    playlist_.queueRemoveAt(q_cursor_);
                    if (q_cursor_ >= playlist_.queueSize() && q_cursor_ > 0)
                        --q_cursor_;
                }
                return;
            case 'c': case 'C':
                playlist_.queueClear();
                q_cursor_ = 0;
                return;
            default: return;
        }
    }

    // Device picker
    if (right_pane_ == RightPane::Devices) {
        switch (ch) {
            case 'X': case 27: right_pane_ = RightPane::Playlist; return;
            case 17: audio_.stop(); running_ = false; return;
            case ' ': audio_.togglePause(); return;
            case 'j': case 'J': case KEY_DOWN:
                if (device_cursor_ < (int)device_list_.size()-1) ++device_cursor_;
                return;
            case 'k': case 'K': case KEY_UP:
                if (device_cursor_ > 0) --device_cursor_;
                return;
            case 'd':
                audio_.setDevice(nullptr);
                right_pane_ = RightPane::Playlist;
                return;
            case '\n': case KEY_ENTER: case '\r':
                if (!device_list_.empty()) {
                    device_cursor_ = std::clamp(device_cursor_, 0,
                                     (int)device_list_.size()-1);
                    audio_.setDevice(&device_list_[(size_t)device_cursor_].id);
                    audio_.setSelectedDeviceIndex(device_cursor_);
                    right_pane_ = RightPane::Playlist;
                }
                return;
            default: return;
        }
    }

    if (ch == 'i' || ch == 'I') {
        if (right_pane_ == RightPane::TrackInfo) {
            right_pane_ = RightPane::Playlist;  // always return to playlist
        } else {
            right_pane_ = RightPane::TrackInfo;
        }
        return;
    }

    // Dismiss help with any non-? key too (just re-route to normal handling)
    // Actually let ? be the only toggle — other keys pass through below

    // v: Awesome -> show/hide the full-width Spectrum strip; Classic -> the
    // right-pane visualizer overlay (unchanged MOC-style toggle).
    if (ch == 'v' || ch == 'V') {
        if (config_.awesome_mode) {
            awesome_viz_strip_ = !awesome_viz_strip_;
            resizeWindows();              // rebuild panes with/without the strip
            redraw_needed_.store(true);
        } else {
            right_pane_ = (right_pane_ == RightPane::Visualizer)
                              ? RightPane::Playlist : RightPane::Visualizer;
        }
        return;
    }

    // When help is showing, j/k scroll it; other keys still work globally.
    // PgUp/PgDn/Home/End mirror the playlist's nav SEMANTICS (page = one
    // visible page minus a row of overlap, clamped at both ends) applied to
    // the help pane's scroll offset. The end clamp is drawHelp's draw-time
    // invariant (help_scroll_ = clamp(0, max_scroll)), the same pattern the
    // playlist uses for its cursor scroll, so setting help_scroll_ past a
    // bound here is pinned on the next render - short content (max_scroll==0)
    // clamps every key to 0, a harmless no-op.
    if (right_pane_ == RightPane::Help) {
        // Page size from the help viewport (win_playlist_ hosts the pane):
        // rows-2 visible rows (header + bottom border), minus one for overlap,
        // exactly as drawHelp computes visible_rows.
        int hr = 0, hc = 0;
        if (win_playlist_) getmaxyx(win_playlist_, hr, hc);
        (void)hc;
        int help_page = std::max(1, (hr - 2) - 1);
        switch (ch) {
            case 17: audio_.stop(); running_ = false; return;
            case 'j': case 'J': case KEY_DOWN: ++help_scroll_; redraw_needed_.store(true); return;
            case 'k': case 'K': case KEY_UP:   --help_scroll_; redraw_needed_.store(true); return;
            case KEY_NPAGE: help_scroll_ += help_page; redraw_needed_.store(true); return;  // PgDn
            case KEY_PPAGE: help_scroll_ -= help_page; redraw_needed_.store(true); return;  // PgUp
            case KEY_HOME:  help_scroll_ = 0;          redraw_needed_.store(true); return;
            case KEY_END:   help_scroll_ = INT_MAX;    redraw_needed_.store(true); return;  // draw clamps to max_scroll
            case ' ': audio_.togglePause(); return;
            case 's': audio_.stop(); return;
            default: return;
        }
    }

    // Bookmarks popup — j/k navigate, Enter jump, x delete, B close
    if (right_pane_ == RightPane::Chapters) {
        switch (ch) {
            case 17: audio_.stop(); running_ = false; return;
            case ';': case 27: right_pane_ = RightPane::Playlist; return;
            case 'j': case 'J': case KEY_DOWN:
                ++chapter_cursor_; redraw_needed_.store(true); return;
            case 'k': case 'K': case KEY_UP:
                --chapter_cursor_; redraw_needed_.store(true); return;
            case '\n': case KEY_ENTER: case '\r':
                if (!current_chapters_.empty()) {
                    chapter_cursor_ = std::clamp(chapter_cursor_, 0,
                                       (int)current_chapters_.size()-1);
                    // Browsed book (chapters belong to a file that isn't playing):
                    // start playing it at the chosen chapter. Select its playlist
                    // row first so current()/auto-advance stay coherent - the same
                    // bookkeeping as a normal playlist play. Seek-only (the original
                    // behaviour) would seek the WRONG file's timeline.
                    if (chapters_for_path_ != audio_.currentTrack().path) {
                        for (std::size_t i = 0; i < playlist_.size(); ++i)
                            if (playlist_.at(i).path == chapters_for_path_) {
                                playlist_.selectAt(i);
                                break;
                            }
                        if (!audio_.play(chapters_for_path_)) {
                            showTrackToast("Cannot play file", "", "");
                            return;
                        }
                        config_.addRecentTrack(chapters_for_path_);
                        maybePreloadNext();
                    }
                    audio_.seekTo(current_chapters_[(size_t)chapter_cursor_].start_sec);
                    redraw_needed_.store(true);
                }
                return;
            case ',': jumpChapter(-1); return;
            case '.': jumpChapter(+1); return;
            case ' ': audio_.togglePause(); return;
            case 's': audio_.stop(); return;
            default: return;
        }
    }

    // Playlist-search results — pick-to-jump (mirrors the Bookmarks popup)
    if (right_pane_ == RightPane::SearchResults) {
        switch (ch) {
            case 17: audio_.stop(); running_ = false; return;
            case 27: case '\\':   // Esc / \ close without jumping
                right_pane_ = RightPane::Playlist;
                redraw_needed_.store(true);
                return;
            case 'j': case 'J': case KEY_DOWN:
                ++search_cursor_; redraw_needed_.store(true); return;
            case 'k': case 'K': case KEY_UP:
                --search_cursor_; redraw_needed_.store(true); return;
            case '\n': case KEY_ENTER: case '\r':
                if (!search_results_.empty()) {
                    search_cursor_ = std::clamp(search_cursor_, 0,
                                       (int)search_results_.size()-1);
                    jumpToPlaylistIndex(search_results_[(size_t)search_cursor_]);
                }
                return;
            default: return;
        }
    }

    if (right_pane_ == RightPane::Bookmarks) {
        switch (ch) {
            case 17: audio_.stop(); running_ = false; return;
            case 'B': right_pane_ = RightPane::Playlist; return;
            case 27:  right_pane_ = RightPane::Playlist; return;
            case 'j': case 'J': case KEY_DOWN:
                ++bookmark_cursor_; redraw_needed_.store(true); return;
            case 'k': case 'K': case KEY_UP:
                --bookmark_cursor_; redraw_needed_.store(true); return;
            case '\n': case KEY_ENTER: case '\r':
                if (!config_.bookmarks.empty()) {
                    bookmark_cursor_ = std::clamp(bookmark_cursor_, 0,
                                       (int)config_.bookmarks.size()-1);
                    current_dir_   = config_.bookmarks[(size_t)bookmark_cursor_];
                    in_drive_list_ = false;
                    in_recent_     = false;
                    dir_cursor_    = 0;
                    dir_scroll_    = 0;
                    refreshDir();
                    right_pane_ = RightPane::Playlist;
                }
                return;
            case 'x': case 'X':
                if (!config_.bookmarks.empty()) {
                    bookmark_cursor_ = std::clamp(bookmark_cursor_, 0,
                                       (int)config_.bookmarks.size()-1);
                    config_.bookmarks.erase(
                        config_.bookmarks.begin() + bookmark_cursor_);
                    if (bookmark_cursor_ >= (int)config_.bookmarks.size() && bookmark_cursor_ > 0)
                        --bookmark_cursor_;
                    redraw_needed_.store(true);
                }
                return;
            case ' ': audio_.togglePause(); return;
            case 's': audio_.stop(); return;
            default: return;
        }
    }

    // Track info — global keys only
    if (right_pane_ == RightPane::TrackInfo) {
        // In edit mode — capture all keystrokes for text editing
        if (tag_edit_mode_) {
            switch (ch) {
                case 27:  // Esc — cancel
                    tag_edit_mode_ = false;
                    break;
                case '\n': case KEY_ENTER: case '\r':  // Enter — save
                    if (!saveTagEdits()) {
                        // Write refused (file locked/read-only). Exit edit mode anyway -
                        // staying would trap the user (s = stop isn't reachable from
                        // inside edit mode). Warn persistently; the display title was NOT
                        // updated, so the UI still shows the unchanged disk.
                        warn_msg_ = "Tag save failed - file locked or read-only";
                        warn_msg_ticks_ = 0;
                        redraw_needed_.store(true);
                    }
                    tag_edit_mode_ = false;
                    break;
                case KEY_UP: case 'k':
                    if (tag_edit_field_ > 0) --tag_edit_field_;
                    break;
                case KEY_DOWN: case 'j':
                    if (tag_edit_field_ < 4) ++tag_edit_field_;
                    break;
                case KEY_BACKSPACE: case 127: case 8: {
                    auto& v = tag_edit_values_[tag_edit_field_];
                    if (!v.empty()) v.pop_back();
                    break;
                }
                default:
                    if (ch >= 32 && ch < 127) {
                        // Printable ASCII
                        tag_edit_values_[tag_edit_field_] += (char)ch;
                    }
                    break;
            }
            redraw_needed_.store(true);
            return;
        }
        // Normal mode
        switch (ch) {
            case 17: audio_.stop(); running_ = false; return;
            case ' ': audio_.togglePause(); return;
            case 's': audio_.stop(); return;
            case 'i': case 'I': case 27:
                right_pane_ = RightPane::Playlist;
                info_cached_path_.clear();
                return;
            case 'e': {   // E freed for eject (was a pure dupe of e)
                // Enter edit mode — only for real files, not CD tracks, not currently playing
                std::size_t idx;
                if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                    idx = (std::size_t)pl_cursor_;
                } else if (auto r = nowPlayingRow()) {
                    idx = *r;
                } else {
                    idx = playlist_.current();   // nothing playing / queue-launched stream: last-known index
                }
                if (idx < playlist_.size()) {
                    const std::string& path = playlist_.at(idx).path;
                    switch (tagEditability(path)) {
                        case TagEditability::NotAFile:      // CD track / radio stream
                        case TagEditability::Empty:
                            return;                          // silently ignore, as today
                        case TagEditability::PlayingLocked:
                            // Currently playing — warn in the cmdline bar (persists ~5s)
                            warn_msg_ = "Stop playback first to edit tags  (s = stop)";
                            warn_msg_ticks_ = 0;
                            redraw_needed_.store(true);
                            return;
                        case TagEditability::Editable:
                            break;                           // fall through to enter edit mode
                    }
                    // All checks passed — enter edit mode
                    tag_edit_path_ = path;
                    tag_edit_field_ = 0;
                    const auto& ct = (path == audio_.currentTrack().path)
                                   ? audio_.currentTrack() : TrackInfo{};
                    const auto& e = playlist_.at(idx);
                    tag_edit_values_[0] = ct.title.empty() ? e.display_title : ct.title;
                    tag_edit_values_[1] = ct.artist;
                    tag_edit_values_[2] = ct.album;
                    tag_edit_values_[3] = ct.genre;
                    tag_edit_values_[4] = ct.year > 0 ? std::to_string(ct.year) : "";
                    if (ct.title.empty()) {
                        try {
#ifdef _WIN32
                            auto wp = utf8_to_wide(path);
                            TagLib::FileRef ref(wp.c_str(),true,TagLib::AudioProperties::Fast);
#else
                            TagLib::FileRef ref(path.c_str(),true,TagLib::AudioProperties::Fast);
#endif
                            if (!ref.isNull() && ref.tag()) {
                                auto* t = ref.tag();
                                if (!t->title().isEmpty())  tag_edit_values_[0] = t->title().to8Bit(true);
                                if (!t->artist().isEmpty()) tag_edit_values_[1] = t->artist().to8Bit(true);
                                if (!t->album().isEmpty())  tag_edit_values_[2] = t->album().to8Bit(true);
                                if (!t->genre().isEmpty())  tag_edit_values_[3] = t->genre().to8Bit(true);
                                if (t->year() > 0) tag_edit_values_[4] = std::to_string(t->year());
                            }
                        } catch (...) {}
                    }
                    tag_edit_mode_ = true;
                }
                return;
            }
            case 'n': case 'N':
                if (auto p = playlist_.next(); p.has_value()) {
                    audio_.play(p.value());
                    maybePreloadNext();
                } else audio_.stop();
                return;
            case 'p': case 'P':
                if (auto p = playlist_.previous(); p.has_value()) {
                    audio_.play(p.value());
                    maybePreloadNext();
                }
                return;
            default: return;
        }
    }

    // When visualizer is showing, only global keys work
    if (right_pane_ == RightPane::Visualizer) {
        switch (ch) {
            case 17: audio_.stop(); running_ = false; break;
            case ' ':           audio_.togglePause(); break;
            case 's': case 'S': audio_.stop(); break;
            case '-': case '_': audio_.adjustVolume(-0.05f); break;
            case '=': case '+': audio_.adjustVolume(+0.05f); break;
            case 'n': case 'N':
                if (auto p = playlist_.next(); p.has_value()) {
                    audio_.play(p.value());
                    maybePreloadNext();
                } else audio_.stop();
                break;
            case 'p': case 'P':
                if (auto p = playlist_.previous(); p.has_value()) {
                    audio_.play(p.value());
                    maybePreloadNext();
                }
                break;
            case '[': requestSeek(-5.0); break;
            case ']': requestSeek(+5.0); break;
            case ',': jumpChapter(-1); break;
            case '.': jumpChapter(+1); break;
            case 't': case 'T': show_remaining_ = !show_remaining_; break;
            case 'w': case 'W': show_clock_ = !show_clock_; break;
            default: break;
        }
        return;
    }

    switch (ch) {
        case 17:  // Ctrl+Q — quit
            audio_.stop(); running_ = false; break;
        // Ctrl+A / Ctrl+K un-gated for Linux (slice-5 follow-up, Dos-found: both
        // keys dead on Linux — the whole case was compiled out, not the tty).
        // Both are portable and depend on NO unported subsystem: IHeartDeepLog is
        // portable since slice 1; the digital(HLS)/raw(ICY) stream paths are both
        // ported (slices 2/3). Unlike ^F/^Y/^R (overlay/CD still Windows-gated),
        // these had no reason to stay gated — a missed un-gate, same class as
        // b2abb12.
        case 1:  // Ctrl+A — toggle deep-analysis iHeart capture log (diagnostic; not persisted)
        {
            // Deep log lives in the streaming plugin now (slice c): toggle it across
            // the ABI (audio_.setDeepLog -> set_config("deeplog",...)). The host
            // tracks the on/off state; the plugin-side capture path is no longer
            // surfaced in the toast (it was lazily created on the producer thread
            // anyway — path() right after enable was already racy/empty).
            deeplog_on_ = !deeplog_on_;
            audio_.setDeepLog(deeplog_on_);
            showTrackToast(deeplog_on_ ? "Deep log: ON" : "Deep log: OFF", "", "");
        }
            break;
        case 11:  // Ctrl+K — toggle iHeart stream mode: digital (web player) vs raw broadcast
            config_.prefer_digital_stream = !config_.prefer_digital_stream;
            config_.save();
            audio_.setPreferDigital(config_.prefer_digital_stream);
            showTrackToast(config_.prefer_digital_stream
                               ? "Stream: Web Player mode (fewer ads)"
                               : "Stream: Raw broadcast (direct)", "", "");
            if (audio_.streamMode())                 // reconnect now so it takes effect
                audio_.beginStream(audio_.streamUrl());
            break;
        case 16:  // Ctrl+P — PROBE: toggle iHeart digital-handshake identity arm
                  // (anon vs minted anonymous profileId). Hidden A/B control; only has
                  // an effect while the deep log (Ctrl+A) is ON — the mint is probe-gated
                  // in hlsConnect. Read at reconnect, so reconnect now to switch arms.
            config_.iheart_probe_minted = !config_.iheart_probe_minted;
            config_.save();
            audio_.setProbeMinted(config_.iheart_probe_minted);
            showTrackToast(config_.iheart_probe_minted
                               ? "Probe: minted profileId arm (deep log only)"
                               : "Probe: anon handshake arm", "", "");
            if (audio_.streamMode())                 // reconnect now so the arm takes effect
                audio_.beginStream(audio_.streamUrl());
            break;
        case 20:  // Ctrl+T — toggle Classic / Awesome theme
            config_.awesome_mode = !config_.awesome_mode;
            config_.save();
            // Awesome has no right-pane visualizer (the spectrum is the bottom
            // strip); if Classic left the right pane in viz mode, reset it so the
            // pane doesn't come up blank after the switch.
            if (config_.awesome_mode && right_pane_ == RightPane::Visualizer)
                right_pane_ = RightPane::Playlist;
            // Re-init colour pairs for the new mode BEFORE rebuilding windows: Awesome
            // gives CP_DIM a solid base bg, so createWindows()'s wbkgd(CP_DIM) then fills
            // every pane (and the title/cwd/cmdline strips) with the themed base; Classic
            // restores the transparent -1 defaults so it re-inherits the terminal bg.
            initColours();
            resizeWindows();        // rebuild panes with theme-aware geometry +
            redraw_needed_.store(true);   // full shrink-safe screen wipe
            theme_tag_ticks_ = 0;   // flash [THEME:<name>] on the cwd line for ~10s
            break;
        case 14:  // Ctrl+N — toggle Nerd Font title icons (needs a Nerd Font)
            config_.nerd_icons = !config_.nerd_icons;
            config_.save();
            redraw_needed_.store(true);
            showTrackToast(config_.nerd_icons ? "Nerd icons: ON (needs Nerd Font)"
                                              : "Nerd icons: OFF", "", "");
            break;
        case KEY_F(2):    // toggle spectrum style: classic solid bars <-> 80s LED
            config_.viz_led = !config_.viz_led;
            config_.save();
            redraw_needed_.store(true);
#ifndef _WIN32
            status_msg_ = config_.viz_led ? "Spectrum: 80s LED" : "Spectrum: classic bars";
            status_msg_ticks_ = 0;
#else
            showTrackToast(config_.viz_led ? "Spectrum: 80s LED" : "Spectrum: classic bars", "", "");
#endif
            break;
        case KEY_F(3):    // toggle follow-the-playing-row (persisted)
            config_.follow_playing = !config_.follow_playing;
            config_.save();
            if (config_.follow_playing) {   // snap to the playing row now, not next track change
                if (auto row = nowPlayingRow()) pl_cursor_ = (int)*row;
                last_playlist_current_for_sync_ = (int)playlist_.current();  // keep the sync marker coherent
            }
            redraw_needed_.store(true);
            showTrackToast(config_.follow_playing ? "Follow playing: ON"
                                                  : "Follow playing: OFF", "", "");
            break;
        case KEY_F(7):    // previous Awesome named palette (F7)
        case KEY_F(8): {  // next Awesome named palette (F8)
            if (!config_.awesome_mode) {
                // F7/F8 only cycle in Awesome mode; hint otherwise (Classic keeps its
                // theme.conf colours; named palettes are an Awesome feature).
#ifndef _WIN32
                status_msg_ = "Themes live in Awesome mode (Ctrl+T)";
                status_msg_ticks_ = 0;
                redraw_needed_.store(true);
#else
                showTrackToast("Themes live in Awesome mode (Ctrl+T)", "", "");
#endif
                break;
            }
            // F7 steps back, F8 steps forward - a smooth forward/back cycle.
            const int step = (ch == KEY_F(7)) ? (kNumAwesomeThemes - 1) : 1;
            config_.awesome_theme = (config_.awesome_theme + step) % kNumAwesomeThemes;
            config_.save();
            applyAwesomeTheme();          // recolour in place — pairs are global, no rebuild
            clearok(stdscr, TRUE);
            redraw_needed_.store(true);
            theme_tag_ticks_ = 0;   // flash [THEME:<name>] on the cwd line for ~10s
            break;
        }
        case 'F':   // Shift+F: toggle the per-row filetype column (persisted)
            // Not an F-key: F11 is grabbed by Linux terminals for fullscreen
            // before the app ever sees it. Shift+F is terminal-safe and
            // mnemonic (f=ReplayGain, F=Filetype), like e/E.
            // No toast: the column appearing/disappearing is its own feedback
            // (unlike F12, whose refresh may change nothing visible).
            config_.show_filetype = !config_.show_filetype;
            config_.save();
            redraw_needed_.store(true);
            break;
        case KEY_F(12):   // refresh the drive list (pick up hot-plugged drives)
            // Hot-plug isn't auto-detected ([Drives] only rebuilds on entry, and
            // the periodic dir re-scan skips the drive list); F12 is the manual
            // trigger, re-running the same enterDriveList() rebuild. No-op
            // outside [Drives].
            if (in_drive_list_) {
                // enterDriveList() resets the cursor to the top; restore it onto
                // the previously-selected entry if it still exists so a refresh
                // isn't disruptive. Gone entry (drive removed) -> top is correct.
                const std::string sel =
                    (dir_cursor_ >= 0 && dir_cursor_ < (int)dir_entries_.size())
                        ? dir_entries_[(size_t)dir_cursor_] : "";
                enterDriveList();
                if (!sel.empty()) {
                    for (std::size_t i = 0; i < dir_entries_.size(); ++i)
                        if (dir_entries_[i] == sel) { dir_cursor_ = (int)i; break; }
                }
                // The dir browser has no draw-time scroll invariant (j/k nudge
                // per-handler), so re-clamp scroll to keep the restored cursor
                // visible ourselves.
                {
                    int v = paneVisibleRows(win_dir_);
                    if (dir_cursor_ < dir_scroll_) dir_scroll_ = dir_cursor_;
                    else if (v > 0 && dir_cursor_ >= dir_scroll_ + v)
                        dir_scroll_ = dir_cursor_ - v + 1;
                }
                showTrackToast("Drives refreshed", "", "");
                redraw_needed_.store(true);
            }
            break;
        case 'E':   // Shift+E — eject the highlighted CD drive ([Drives] only)
            // Per-pane key meaning (like d / K / J): e stays EQ / tag-edit
            // everywhere; E acts only in [Drives], and only on a row the eject
            // hint is shown for (CD drive with media at last enumeration).
            if (in_drive_list_ && dir_cursor_ >= 0
                && dir_cursor_ < (int)dir_entries_.size()) {
                const std::string& drv = dir_entries_[(size_t)dir_cursor_];
                if (driveHasMedia(drv)) ejectDrive(drv);
                // else: no hint was shown - silent no-op
            }
            break;
#ifdef PDCURSES
        case ALT_ENTER:      // Alt+Enter — wingui borderless-fullscreen toggle
        case ALT_PADENTER:   // (numpad Enter variant)
            toggleWinguiFullscreen();
            clearok(stdscr, TRUE);
            redraw_needed_.store(true);
            break;
#endif
        // Per-case Windows gates below (slice-2 fix): a single #ifdef here
        // used to span ^D/^B/^F/^G/^U/^Y/^R, silently deleting the portable
        // scrobbler-login and radio-URL keys from the Linux build (Dos-found:
        // ^U/^G/^B dead while ^N/^T/^L/^Q worked — a key probe proved the tty
        // delivers all of them; the gap was here). Gate each key on WHY:
        // ^D = common since slice 4 (Unix-socket IIpc); ^F = the MBSearch overlay
        // ENTRY stays deferred (its draw/input handlers are portable, but the
        // feature is Windows-only for now); ^Y/^R = common since slice 6 (SG_IO CD).
        case 4:  // Ctrl+D — toggle Discord Rich Presence
            config_.discord_presence = !config_.discord_presence;
            config_.save();
            if (config_.discord_presence) {
                discord_force_update_ = true;   // push the current track on the next tick
                showTrackToast("Discord presence: ON", "", "");
            } else {
                discord_.clearActivity();
                discord_active_ = false;
                discord_artist_.clear(); discord_track_.clear();
                showTrackToast("Discord presence: OFF", "", "");
            }
            break;
        case 2:  // Ctrl+B — ListenBrainz login (paste user token; no browser/handshake)
            if (config_.listenbrainz_token.empty())
                openInputBar(InputMode::ListenBrainzToken, "");
            else
                showTrackToast("ListenBrainz: logged in as " + config_.listenbrainz_user, "", "");
            break;
#ifdef _WIN32
        case 6:  // Ctrl+F — MusicBrainz / Discogs manual search
            if (ui_overlay_ == UIOverlay::None && !mb_lookup_.isActive()) {
                mb_search_ = {};
                ui_overlay_ = UIOverlay::MBSearch;
                redraw_needed_.store(true);
            }
            break;
#endif
        case 7:  // Ctrl+G — Last.fm login (stateful: request token, then exchange)
            if (!config_.lastfm_session.empty()) {
                // Already logged in — report it, mirroring ^B. (Re-auth = clear
                // lastfm-session in the conf, same convention as ListenBrainz.)
                showTrackToast("Last.fm: logged in as " + config_.lastfm_user, "", "");
            } else if (config_.lastfm_key.empty() || config_.lastfm_secret.empty()) {
                openInputBar(InputMode::LastfmKey, "");   // first-time: prompt in-app
            } else if (config_.lastfm_pending.empty()) {
                sclog("ctrl+G: begin auth");
                lastfmBeginAuth();
            } else {
                std::string sk, user;
                int err = 0;
                bool ok = LastFm::getSession(config_.lastfm_key, config_.lastfm_secret,
                                             config_.lastfm_pending, sk, user, &err);
                sclog("ctrl+G: getSession ok=%d err=%d user=%s", (int)ok, err, user.c_str());
                if (ok) {
                    config_.lastfm_session = sk;
                    config_.lastfm_user    = user;
                    config_.lastfm_pending.clear();
                    config_.save();
                    lf_poll_active_.store(false);   // stop any background poll
                    showTrackToast("Last.fm: logged in as " + user, "", "");
                } else if (err == 14) {
                    // Token valid but not authorized yet — keep it; re-arm the
                    // auto-poll so clicking Allow still completes hands-free.
                    startLastfmPoll(config_.lastfm_pending);
                    showTrackToast("Last.fm: approve in browser - finishes automatically", "", "");
                } else if (err != 0) {
                    // Definitive API rejection (15 expired / 4 invalid / other):
                    // the pending token can never succeed — drop it and start a
                    // fresh auth right away instead of looping on a dead token.
                    config_.lastfm_pending.clear();
                    config_.save();
                    sclog("ctrl+G: pending token dead (err=%d), restarting auth", err);
                    lastfmBeginAuth();
                } else {
                    // Transport failure — token may still be good, keep it.
                    showTrackToast("Last.fm: network error - press Ctrl+G to retry", "", "");
                }
            }
            break;
        case 21:  // Ctrl+U — input an internet-radio stream URL
            if (ui_overlay_ == UIOverlay::None)
                openInputBar(InputMode::StreamURL, "");
            break;
        case 15:  // Ctrl+O — batch ReplayGain scan over the browsed folder (batch-r128)
            if (gain_scan_.running()) {
                gain_scan_.cancel();
                rip_status_ = "ReplayGain scan: cancelling (finishing nothing mid-file)...";
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            } else if (cd_ripper_.isActive()) {
                showTrackToast("ReplayGain scan", "Wait for the rip to finish", "");
            } else {
                // The prompt names the folder — no fat-fingered scans on the
                // wrong directory. Enter = tag untagged, F = force re-tag all.
                rgscan_prompt_ = true;
                rgscan_dir_    = current_dir_;
                rip_status_ = "ReplayGain scan '" + sanitizeForDisplay(rgscan_dir_) +
                              "': [Enter] tag untagged  [F] force re-tag  [Esc] cancel";
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            }
            break;
        case 5:  // Ctrl+E — stream recording: the [Rec] panel (stream-record R2)
            if (ui_overlay_ != UIOverlay::None) break;  // already showing a modal
            if (audio_.streamMode() || audio_.streamRecorder().recording()) {
                // Idle -> settings view; recording -> live-state view (the
                // draw fn branches on the recorder's own state).
                ui_overlay_ = UIOverlay::RecPanel;
                redraw_needed_.store(true);
            } else {
                // Precondition: recording taps the playing stream — nothing
                // to record from a CD/file/silence. Toast, no panel.
                showTrackToast("Stream recording", "Start a stream first (Ctrl+U)", "");
            }
            break;
        case 25:  // Ctrl+Y — CD rip  (slice 6: common — CD path live on Linux)
            if (ui_overlay_ != UIOverlay::None) break;  // already showing modal
            if (cd_ripper_.isActive()) {
                // Already ripping — Ctrl+Y cancels
                cd_ripper_.cancel();
                rip_status_    = "Rip cancelled by user.";
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            } else if (!cd_drive_letter_.empty()) {
                // Slice 3: rip works on a stopped-but-loaded disc. Gate on loaded
                // (cd_drive_letter_), not cdMode — stop() closed the handle, so
                // reopen it first. reopenCDForAction handles an empty tray (eject
                // while stopped): purge + toast + false, and we bail.
                if (!reopenCDForAction(cd_drive_letter_)) break;
                if (!audio_.cdSource().tracks().empty()) {
                    ui_overlay_ = UIOverlay::RipConfirm;
                    redraw_needed_.store(true);
                }
            } else {
                rip_status_    = "Ctrl+Y: Insert a CD and load it from [Drives] first.";
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            }
            break;
        case 18:  // Ctrl+R — MusicBrainz CD lookup
            if (!cd_drive_letter_.empty() && !mb_fetching_.load()) {
                if (mb_lookup_.isActive()) break;  // already in progress
                // Slice 3: look up a stopped-but-loaded disc. Gate on loaded, not
                // cdMode — reopen the handle first (stop() closed it); bail on an
                // empty tray (reopenCDForAction purges + toasts).
                if (!reopenCDForAction(cd_drive_letter_)) break;
                const auto& cd = audio_.cdSource();
                if (cd.tracks().empty()) break;
                mb_fetching_.store(true);
                redraw_needed_.store(true);
                int first = cd.tracks().front().number;
                int last  = cd.tracks().back().number;
                auto offsets = cd.tocOffsets();
                mb_lookup_.lookup(first, last, offsets,
                    [this](bool ok, const MBRelease& rel, const std::string& err) {
                        // All writes to UIManager state from worker thread
                        // must be under mb_mutex_ to prevent races with UI reads
                        std::lock_guard<std::mutex> lk(mb_mutex_);
                        mb_fetching_.store(false);
                        if (!ok) {
                            mb_error_ = err;
                            redraw_needed_.store(true);
                            return;
                        }
                        mb_error_.clear();
                        mb_release_ = rel;  // cache for ripping
                        // playlist_ is UI-thread-owned: defer the title apply to
                        // the run loop (mb_release_ is cached above).
                        mb_titles_pending_.store(true);
                        if (!rel.title.empty())
                            mb_album_ = sanitizeForDisplay(rel.title) + (rel.date.size() >= 4
                                       ? " (" + rel.date.substr(0,4) + ")" : "");
                        redraw_needed_.store(true);
                    });
            } else if (cd_drive_letter_.empty()) {
                // No disc loaded — silently ignore
            }
            break;
        case 'Q':
            // Toggle queue pane
            right_pane_ = (right_pane_ == RightPane::Queue)
                        ? RightPane::Playlist : RightPane::Queue;
            break;
        case 'q': {
            // Add highlighted track to play queue
            // From playlist pane: add the cursor track
            // From dir browser: add the highlighted file
            PlaylistEntry qe;
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                qe = playlist_.at((size_t)pl_cursor_);
            } else if (focus_ == Pane::DirBrowser && dir_cursor_ < (int)dir_entries_.size()) {
                const std::string& nm = dir_entries_[(size_t)dir_cursor_];
                // Build full path — dir_entries_ stores names, not full paths
                std::string p;
                if (in_recent_ || in_favs_ || in_radio_ || fs::path(nm).is_absolute()) {
                    p = nm;  // recent/fav entries are already full paths
                } else {
                    p = (fs::path(current_dir_) / nm).string();
                }
                if (!fs::is_directory(p) && PlaylistManager::isSupportedAudio(p)) {
                    qe.path          = p;
                    // Default to sanitized stem; TagLib will override if tags present
                    qe.display_title = sanitizeForDisplay(fs::path(p).stem().string());
                    qe.duration_sec  = 0;
                    try {
#ifdef _WIN32
                        auto wp = utf8_to_wide(p);
                        TagLib::FileRef ref(wp.c_str(), true, TagLib::AudioProperties::Average);
#else
                        TagLib::FileRef ref(p.c_str(), true, TagLib::AudioProperties::Average);
#endif
                        if (!ref.isNull()) {
                            if (auto* tag = ref.tag()) {
                                std::string t = sanitizeForDisplay(tag->title().to8Bit(true));
                                std::string a = sanitizeForDisplay(tag->artist().to8Bit(true));
                                if (!t.empty())
                                    qe.display_title = a.empty() ? t : a + " - " + t;
                            }
                            if (auto* ap = ref.audioProperties())
                                qe.duration_sec = ap->lengthInSeconds();
                        }
                    } catch (...) {}
                }
            }
            if (!qe.path.empty()) {
                playlist_.queueAdd(qe);
                // Discard any preloaded next-playlist track so crossfade
                // doesn't bypass the queue and play the wrong track
                audio_.clearNext();
                right_pane_ = RightPane::Queue;
            }
            break;
        }
        case '\t':
            toggleFocus(); break;
        case '~':
            // Live-reload theme.conf: re-init the colour pairs and force a full
            // repaint. Pairs are global state, so re-running init_pair() updates
            // every COLOR_PAIR() reference in place — no need to rebuild windows.
            initColours();
            clearok(stdscr, TRUE);
            showTrackToast("Theme reloaded", DigiConfig::themePath(), "");
            break;
        case ' ':
            audio_.togglePause(); break;
        case 't': case 'T':
            show_remaining_ = !show_remaining_; break;
        case 'Z': {
            // Cycle through sleep presets: 0→15→30→45→60→90→0
            static const int presets[] = {15,30,45,60,90,0};
            static const int npresets  = 6;
            int cur = sleep_minutes_;
            int next = 0;
            for (int i = 0; i < npresets; ++i) {
                if (presets[i] > cur) { next = presets[i]; break; }
            }
            sleep_minutes_ = next;
            if (next > 0) sleep_start_ = std::chrono::steady_clock::now();
            break;
        }
        case 'w': case 'W':
            show_clock_ = !show_clock_; break;
        case 'H':
            show_hidden_ = !show_hidden_;
            refreshDir();
            break;
        case 'm': case 'M':
            audio_.toggleMute(); break;
        case 'X':
            if (right_pane_ == RightPane::Devices) {
                right_pane_ = RightPane::Playlist;
            } else {
                device_list_.clear();
                for (auto& d : audio_.enumerateDevices())
                    device_list_.push_back({d.name, d.id});
                device_cursor_ = std::max(0, audio_.selectedDeviceIndex());
                right_pane_ = RightPane::Devices;
            }
            break;
        case 'e':   // E freed for eject in [Drives] (was a pure dupe of e)
            if (right_pane_ == RightPane::EQ)
                right_pane_ = RightPane::Playlist;
            else
                right_pane_ = RightPane::EQ;
            break;
        case '{':
            if (!audio_.cdMode()) { audio_.adjustSpeed(-0.02f); } break;
        case '}':
            if (!audio_.cdMode()) { audio_.adjustSpeed(+0.02f); } break;
        case 'f':   // F freed for the filetype toggle (was a pure dupe of f)
            if (!audio_.cdMode())
                audio_.setReplayGain(!audio_.replayGain());
            break;
        case '*': {
            // Star/favourite the highlighted track — playlist pane or browser
            std::string fav_path;
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                fav_path = playlist_.at((size_t)pl_cursor_).path;
            } else if (focus_ == Pane::DirBrowser && in_favs_
                       && dir_cursor_ < (int)dir_entries_.size()) {
                fav_path = dir_entries_[(size_t)dir_cursor_];
            } else if (focus_ == Pane::DirBrowser
                       && dir_cursor_ < (int)dir_entries_.size()
                       && !in_drive_list_ && !in_recent_ && !in_favs_ && !in_radio_ && !in_books_) {
                const std::string& nm = dir_entries_[(size_t)dir_cursor_];
                std::string full = (fs::path(nm).is_absolute()) ? nm
                                 : (fs::path(current_dir_) / nm).string();
                if (PlaylistManager::isSupportedAudio(full)) fav_path = full;
            }
            if (!fav_path.empty() && !isCDTrackPath(fav_path)
                && PlaylistManager::isSupportedAudio(fav_path)) {
                if (config_.isFav(fav_path)) {
                    config_.removeFavTrack(fav_path);
                    showTrackToast("Removed from FAVs", "", "");
                } else {
                    config_.addFavTrack(fav_path);
                    showTrackToast("Added to FAVs", fs::path(fav_path).filename().string(), "");
                }
            }
            break;
        }
        case KEY_DC:  // Del key — remove from FAVs pane
            if (focus_ == Pane::DirBrowser && in_favs_
                && dir_cursor_ < (int)dir_entries_.size()) {
                const std::string& nm = dir_entries_[(size_t)dir_cursor_];
                if (nm != "[Back]" && !nm.empty()) {
                    config_.removeFavTrack(nm);
                    // Rebuild the fav list in dir_entries_
                    dir_entries_.clear(); dir_display_.clear();
                    dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                    for (const auto& fp : config_.fav_tracks) {
                        dir_entries_.push_back(fp);
                        std::string disp = fs::path(fp).filename().string();
                        dir_display_.push_back(sanitizeForDisplay(disp.empty() ? fp : disp));
                    }
                    if (dir_cursor_ >= (int)dir_entries_.size())
                        dir_cursor_ = std::max(0, (int)dir_entries_.size() - 1);
                }
            }
            if (focus_ == Pane::DirBrowser && in_radio_
                && dir_cursor_ < (int)dir_entries_.size()) {
                const std::string& nm = dir_entries_[(size_t)dir_cursor_];
                if (nm != "[Back]" && !nm.empty()) {
                    config_.removeRadioStation(nm);
                    dir_entries_.clear(); dir_display_.clear();
                    dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                    for (const auto& st : config_.radio_stations) {
                        dir_entries_.push_back(st);
                        dir_display_.push_back(sanitizeForDisplay(stationLabel(st)));
                    }
                    if (dir_cursor_ >= (int)dir_entries_.size())
                        dir_cursor_ = std::max(0, (int)dir_entries_.size() - 1);
                }
            }
            if (focus_ == Pane::DirBrowser && in_books_
                && dir_cursor_ < (int)dir_entries_.size()) {
                const std::string& nm = dir_entries_[(size_t)dir_cursor_];
                if (nm != "[Back]" && !nm.empty()) {
                    config_.removeAudiobook(nm);
                    config_.save();
                    dir_entries_.clear(); dir_display_.clear();
                    dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                    for (const auto& bk : config_.audiobooks) {
                        dir_entries_.push_back(bk);
                        std::string disp = fs::path(bk).filename().string();
                        dir_display_.push_back(sanitizeForDisplay(disp.empty() ? bk : disp));
                    }
                    if (dir_cursor_ >= (int)dir_entries_.size())
                        dir_cursor_ = std::max(0, (int)dir_entries_.size() - 1);
                }
            }
            break;
        case 'o':
            // Cycle playlist sort mode
            playlist_.cycleSort();
            last_playlist_current_for_sync_ = (int)playlist_.current();
            pl_cursor_ = (int)playlist_.current();
            break;
        case 'O':
            // Cycle browser sort mode
            browser_sort_ = static_cast<BrowserSort>(((int)browser_sort_ + 1) % 3);
            refreshDir();
            break;
        case 'b':
            // Cursor on an audiobook file in the normal browser -> toggle in [Books].
            if (focus_ == Pane::DirBrowser
                && !in_drive_list_ && !in_recent_ && !in_favs_ && !in_radio_ && !in_books_
                && dir_cursor_ < (int)dir_entries_.size()) {
                const std::string& nm = dir_entries_[(size_t)dir_cursor_];
                std::string full = fs::path(nm).is_absolute() ? nm
                                 : (fs::path(current_dir_) / nm).string();
                if (PlaylistManager::isAudiobook(full) && fs::exists(full)) {
                    if (config_.isSavedBook(full)) {
                        config_.removeAudiobook(full);
                        showTrackToast("Removed from Books", "", "");
                    } else {
                        config_.addAudiobook(full);
                        showTrackToast("Added to Books", fs::path(full).filename().string(), "");
                    }
                    config_.save();
                    break;                       // handled; don't also bookmark the dir
                }
            }
            // Add current directory to bookmarks (not available from [Drives] list)
            if (!in_drive_list_ && !in_recent_ && !in_radio_ && !current_dir_.empty()) {
                // Don't bookmark CD drive roots — physical drives are volatile
#ifdef _WIN32
                std::string dp = current_dir_;
                if (dp.back() != '\\') dp += '\\';
                std::wstring wdp(dp.begin(), dp.end());
                if (GetDriveTypeW(wdp.c_str()) == DRIVE_CDROM) break;
#endif
                auto& bm = config_.bookmarks;
                if (std::find(bm.begin(), bm.end(), current_dir_) == bm.end())
                    bm.push_back(current_dir_);
            }
            break;
        case 'B':
            // Show bookmarks popup
            if (right_pane_ == RightPane::Bookmarks) {
                right_pane_ = RightPane::Playlist;
            } else {
                right_pane_ = RightPane::Bookmarks;
                bookmark_cursor_ = 0;
            }
            break;
        case 'g': case 'G':
            gotoOpen(); break;
        case '\\':   // playlist search - pick-to-jump (never a filter)
            // Same modal guard as the other input-bar keys; the goto machinery
            // supplies the input line, cursor, backspace, and Esc for free.
            if (ui_overlay_ == UIOverlay::None)
                openInputBar(InputMode::PlaylistSearch, "");
            break;
        case 's':
            audio_.stop(); break;
        case 'S':
            // Shift+S: save playlist as M3U
            if (!playlist_.empty())
                openInputBar(InputMode::SaveM3U, current_dir_ + "\\playlist.m3u");
            break;
        case 'l': case 'L':
            openInputBar(InputMode::LoadM3U, current_dir_); break;
        case '!':
            config_.toast_enabled = !config_.toast_enabled;
            break;
        case 'c': case 'C':
            if (focus_ == Pane::Playlist) {
                bool was_playing = (audio_.state() != PlaybackState::Stopped);
                playlist_.clear();
                if (was_playing) audio_.stop();
                pl_cursor_ = 0; pl_scroll_ = 0;
            }
            break;
        case 'r': case 'R':
            playlist_.cycleRepeat();
            audio_.setRepeatOne(playlist_.repeatMode() == RepeatMode::One);
            // Clear preloaded next track when switching to repeat-one
            if (playlist_.repeatMode() == RepeatMode::One)
                audio_.clearNext();
            break;
        case 'z':
            playlist_.toggleShuffle(); break;
        case 'u':   // convert-core: mark/unmark the focused browser file
            if (focus_ == Pane::DirBrowser) {
                std::string p = browserEntryPath(dir_cursor_);
                if (!p.empty() && convertSupportedInput(p)) {
                    bool now = marked_.toggle(p);
                    showTrackToast(now ? "Marked" : "Unmarked",
                                   fs::path(p).filename().string(), "");
                    redraw_needed_.store(true);
                }
            }
            break;
        case 'U':   // convert-core: clear all marks
            if (!marked_.empty()) {
                marked_.clear();
                showTrackToast("Marks cleared", "", "");
                redraw_needed_.store(true);
            }
            break;
        case 'x': {  // convert-core: open the convert scope chooser (or cancel a run)
            if (ui_overlay_ != UIOverlay::None) break;
            if (convert_job_.running()) {
                convert_job_.cancel();
                rip_status_ = "Convert: cancelling (finishing nothing mid-file)...";
                rip_msg_ticks_ = 0; redraw_needed_.store(true);
                break;
            }
            convert_single_.clear(); convert_src_dir_.clear();
            if (focus_ == Pane::DirBrowser) {
                std::string p = browserEntryPath(dir_cursor_);
                if (!p.empty() && convertSupportedInput(p)) convert_single_ = p;
                if (!in_drive_list_ && !in_radio_ && !in_favs_ && !in_recent_ && !in_books_)
                    convert_src_dir_ = current_dir_;
            }
            if (convert_single_.empty() && convert_src_dir_.empty() && marked_.empty()) {
                showTrackToast("Convert", "No file, folder, or marks here", "");
                break;
            }
            convert_scope_ = 0; convert_focus_ = 0;
            ui_overlay_ = UIOverlay::ConvertScope;
            redraw_needed_.store(true);
            break;
        }
        // Manual next/prev - one home (manualNext/manualPrevious); an OS
        // Next/Previous command routes through the same methods (osmedia-seam).
        case 'n': case 'N': manualNext();     break;
        case 'p': case 'P': manualPrevious(); break;
        case '[': requestSeek(-5.0); break;
        case ']': requestSeek(+5.0); break;
        case ',': jumpChapter(-1); break;
        case '.': jumpChapter(+1); break;
        case ';':
            // Toggle the chapter list. Loads chapters for the HIGHLIGHTED playlist
            // row (not just the playing track), so a book's chapters can be browsed
            // before playing it. (Browser-pane highlight is out of scope - playlist
            // rows only; with nothing relevant highlighted the playing track's list,
            // kept fresh by the title-bar draw, is what opens.)
            if (right_pane_ == RightPane::Chapters) {
                right_pane_ = RightPane::Playlist;
            } else {
                if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size())
                    refreshChaptersIfNeeded(playlist_.at((size_t)pl_cursor_).path);
                if (!current_chapters_.empty()) {
                    // Playing book: open on the chapter under the playhead. Browsed
                    // (not playing) book: the playhead is another file's position -
                    // meaningless here - so open on chapter 1.
                    chapter_cursor_ = (chapters_for_path_ == audio_.currentTrack().path)
                                          ? std::max(0, currentChapterIndex()) : 0;
                    right_pane_ = RightPane::Chapters;
                } else {
                    showTrackToast("No chapters in this track", "", "");
                }
            }
            break;
        case '-': case '_':
            audio_.adjustVolume(-0.05f); break;
        case '=': case '+':
            audio_.adjustVolume(+0.05f); break;
        case '<':
            audio_.setBalance(std::clamp(audio_.balance() - 0.1f, -1.0f, 1.0f)); break;
        case '>':
            audio_.setBalance(std::clamp(audio_.balance() + 0.1f, -1.0f, 1.0f)); break;
        case '`':
            audio_.setBalance(0.0f); break;
        case 'K':   // move track up (was u/U)
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                playlist_.moveUp((std::size_t)pl_cursor_);
                if (pl_cursor_ > 0) --pl_cursor_;   // scroll follows via the draw-time invariant
                // Prevent cursor sync from snapping us away
                last_playlist_current_for_sync_ = (int)playlist_.current();
                // Keep the follow-sync from stealing the cursor onto the playing
                // track when the move shifts its row index - we are deliberately
                // moving pl_cursor_ ourselves. Refreshing the marker means the
                // follow-sync sees the new position as already accounted for; if
                // the moved track IS the playing one, cursor and marker move
                // together, so it still rides.
                if (auto npr = nowPlayingRow()) last_now_playing_row_ = (int)*npr;
            }
            break;
        case 'J':   // move track down (was D)
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                playlist_.moveDown((std::size_t)pl_cursor_);
                if (pl_cursor_ + 1 < (int)playlist_.size()) ++pl_cursor_;   // scroll follows via the invariant
                last_playlist_current_for_sync_ = (int)playlist_.current();
                // Same follow-sync marker refresh as K above.
                if (auto npr = nowPlayingRow()) last_now_playing_row_ = (int)*npr;
            }
            break;
        case 'd':
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                // Stream-aware "is the cursor on the playing row" - nowPlayingRow()
                // folds in the state()!=Stopped check and is not stale in stream mode,
                // so deleting an unrelated row under a stream no longer stops it.
                auto now = nowPlayingRow();
                bool was_playing = now && (size_t)pl_cursor_ == *now;
                playlist_.removeAt((size_t)pl_cursor_);
                if (was_playing) audio_.stop();
                // Keep cursor in bounds; scroll re-clamps via the draw-time invariant.
                if (pl_cursor_ >= (int)playlist_.size() && pl_cursor_ > 0)
                    --pl_cursor_;
                // Deleting an entry above the playing track shifts current()'s
                // index; resync the marker so the follow logic doesn't read it as
                // a track change and snap the cursor to the playing row.
                last_playlist_current_for_sync_ = (int)playlist_.current();
                // Same for the follow-sync marker (same family as the K/J fix):
                // the deletion shifted the playing row's index, not the track.
                if (auto npr = nowPlayingRow()) last_now_playing_row_ = (int)*npr;
            }
            else if (focus_ == Pane::DirBrowser && in_radio_
                     && dir_cursor_ < (int)dir_entries_.size()) {
                // 'd' also removes a saved station from the [Radio] list (like Del)
                const std::string& nm = dir_entries_[(size_t)dir_cursor_];
                if (nm != "[Back]" && !nm.empty()) {
                    config_.removeRadioStation(nm);
                    dir_entries_.clear(); dir_display_.clear();
                    dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                    for (const auto& st : config_.radio_stations) {
                        dir_entries_.push_back(st);
                        dir_display_.push_back(sanitizeForDisplay(stationLabel(st)));
                    }
                    if (dir_cursor_ >= (int)dir_entries_.size())
                        dir_cursor_ = std::max(0, (int)dir_entries_.size() - 1);
                }
            }
            break;
        case '/':
            if (focus_ == Pane::DirBrowser && in_radio_)
                openInputBar(InputMode::RadioSearch, "");
            break;
        case 'a': case 'A':
            if (focus_ == Pane::DirBrowser && dir_cursor_ < (int)dir_entries_.size()) {
                fs::path full = fs::path(current_dir_) / dir_entries_[(size_t)dir_cursor_];
                if (PlaylistManager::isSupportedAudio(full.string()))
                    playlist_.addTrack(full.string());
                else if (fs::is_directory(full))
                    playlist_.addDirectoryAsync(full.string());
            }
            break;
        case KEY_DOWN: case 'j': navigateDown(); break;   // J freed for move-down
        case KEY_UP:   case 'k': navigateUp();   break;   // K freed for move-up
        // Page nav for the focused list. The KEY_HOME/KEY_END in handleGotoInput
        // are the goto-bar TEXT-cursor handlers - separate context, no collision
        // (goto_active_ intercepts before this switch).
        case KEY_NPAGE: navigatePage(+1);        break;   // PgDn
        case KEY_PPAGE: navigatePage(-1);        break;   // PgUp
        case KEY_HOME:  navigateHomeEnd(false);  break;
        case KEY_END:   navigateHomeEnd(true);   break;
        case KEY_LEFT:
            if (focus_ == Pane::DirBrowser) {
                if (in_drive_list_) break;
                fs::path p(current_dir_);
                if (p.parent_path() == p) {
                    enterDriveList();
                } else {
                    fs::path parent = p.parent_path();
                    if (!parent.empty() && parent.string() != current_dir_) {
                        current_dir_ = parent.string();
                        dir_cursor_ = 0; dir_scroll_ = 0; refreshDir();
                    }
                }
            }
            break;
        case '\n': case KEY_ENTER: case '\r':
            activateSelection(); break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Goto-directory input bar
// ─────────────────────────────────────────────────────────────────────────────
void UIManager::gotoOpen() {
    openInputBar(InputMode::Goto, current_dir_);
}

void UIManager::openInputBar(InputMode mode, const std::string& initial) {
    goto_active_ = true;
    input_mode_  = mode;
    goto_input_  = initial;
    goto_cursor_ = (int)goto_input_.size();
    tab_matches_.clear();
    tab_idx_    = -1;
    tab_prefix_ = "";
    curs_set(1);
    redraw_needed_.store(true);
}

void UIManager::gotoClose(bool commit) {
    curs_set(0);
    goto_active_ = false;
    if (commit) {
        std::string target = goto_input_;
        // Strip trailing separator — but preserve drive roots (e.g. "G:\")
        // A bare "G:" without separator is not a valid Windows path
        auto is_drive_root = [](const std::string& s) {
            return s.size() == 3 && std::isalpha((unsigned char)s[0])
                   && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
        };
        while (target.size() > 1 && !is_drive_root(target)
               && (target.back() == '/' || target.back() == '\\'))
            target.pop_back();

        switch (input_mode_) {
            case InputMode::Goto: {
                // Check if target is a playlist file
                std::string ext = fs::path(target).extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                bool is_playlist = (ext == ".m3u" || ext == ".m3u8" ||
                                    ext == ".pls" || ext == ".xspf");

                if (is_playlist && fs::exists(target)) {
                    std::size_t before = playlist_.size();
                    playlist_.loadPlaylist(target);
                    std::size_t after  = playlist_.size();
                    if (int miss = playlist_.lastLoadMissing(); miss > 0)
                        showTrackToast("Playlist loaded", std::to_string(miss) + " file(s) not found", "");
                    if (after > before) {
                        // Jump to the first genuinely-new entry (trust the size
                        // delta, not the line count — dedup may drop duplicates).
                        pl_cursor_ = (int)before;   // scroll placed by the draw-time invariant
                        if (audio_.state() == PlaybackState::Stopped) {
                            playlist_.selectAt(before);
                            if (auto path = playlist_.currentPath(); path.has_value()) {
                                audio_.play(path.value());
                                maybePreloadNext();
                            }
                        }
                    } else {
                        // All duplicates / none on disk — keep the view valid.
                        if (pl_cursor_ >= (int)after) pl_cursor_ = std::max(0, (int)after - 1);
                    }
                } else if (fs::exists(target) && fs::is_directory(target)) {
                    current_dir_   = target;
                    dir_cursor_    = 0;
                    dir_scroll_    = 0;
                    in_drive_list_ = false;
                    refreshDir();
                }
                break;
            }
            case InputMode::RecDir: {
                // Stream-record R2: [D] from the [Rec] panel. Session override
                // only (never written back to config); empty submit keeps the
                // derived default. Reopen the panel with the new value.
                rec_dir_ = target;
                ui_overlay_ = UIOverlay::RecPanel;
                redraw_needed_.store(true);
                break;
            }
            case InputMode::RecOffset: {
                // split-trim: [T] from the [Rec] panel. Clamp 0-5000 (lead is
                // reserved - negative folds to 0); junk keeps the prior value.
                try { rec_offset_ms_ = std::clamp(std::stoi(target), 0, 5000); }
                catch (...) {}
                ui_overlay_ = UIOverlay::RecPanel;
                redraw_needed_.store(true);
                break;
            }
            case InputMode::SaveM3U: {
                // Default to .m3u if no recognised playlist extension given
                std::string ext = fs::path(target).extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext != ".m3u" && ext != ".m3u8" && ext != ".pls" && ext != ".xspf")
                    target += ".m3u";
                playlist_.savePlaylist(target);
                break;
            }
            case InputMode::LoadM3U: {
                std::size_t before = playlist_.size();
                playlist_.loadPlaylist(target);
                std::size_t after  = playlist_.size();
                if (int miss = playlist_.lastLoadMissing(); miss > 0)
                    showTrackToast("Playlist loaded", std::to_string(miss) + " file(s) not found", "");
                if (after > before) {
                    // Jump to the first genuinely-new entry. Dedup may have
                    // dropped some lines, so trust the size delta rather than
                    // the raw line count returned by loadPlaylist().
                    pl_cursor_ = (int)before;   // scroll placed by the draw-time invariant
                    if (audio_.state() == PlaybackState::Stopped) {
                        playlist_.selectAt(before);
                        if (auto path = playlist_.currentPath(); path.has_value()) {
                            audio_.play(path.value());
                            maybePreloadNext();
                        }
                    }
                } else {
                    // Nothing new was appended (every track was already present,
                    // or none existed on disk). Keep the view on-screen instead
                    // of scrolling past the end and blanking the pane.
                    if (pl_cursor_ >= (int)after) pl_cursor_ = std::max(0, (int)after - 1);
                }
                break;
            }
            case InputMode::StreamURL: {
                std::string url = goto_input_;
                // Trim whitespace (paste artifacts)
                while (!url.empty() && (url.front() == ' ' || url.front() == '\t'))
                    url.erase(url.begin());
                while (!url.empty() && (url.back() == ' ' || url.back() == '\t' ||
                                        url.back() == '\r' || url.back() == '\n'))
                    url.pop_back();
                // Collapse an accidental doubled scheme (e.g. prefilled + pasted)
                if (url.rfind("https://https://", 0) == 0)     url.erase(0, 8);
                else if (url.rfind("http://http://", 0) == 0)  url.erase(0, 7);
                if (!url.empty()) {
                    // Stash the cleaned URL and chain to an optional name prompt.
                    // The actual add/persist/play happens on StreamName submit so a
                    // user-supplied name can label the station (blank = derived label).
                    pending_stream_url_ = url;
                    openInputBar(InputMode::StreamName, "");
                }
                break;
            }
            case InputMode::StreamName: {
                std::string name = goto_input_;
                while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
                    name.erase(name.begin());
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t' ||
                                         name.back() == '\r' || name.back() == '\n'))
                    name.pop_back();
                std::string url = pending_stream_url_;
                pending_stream_url_.clear();
                if (!url.empty()) {
                    // Supplied name -> "RADIO: <name>" (keeps the station-signalling
                    // prefix); blank -> today's URL-derived label. Name is in-session
                    // only for now — persistence needs the Config schema change.
                    std::string label = name.empty() ? radioLabel(url)
                                                      : ("RADIO: " + sanitizeForDisplay(name));
                    config_.addRadioStation(url, name);   // persist URL + name, show in [Radio]
                    config_.save();                       // flush now so the name survives restart

                    // Append as a playlist entry (visible, removable, re-selectable),
                    // jump to it, reveal the playlist pane, then play. Coexists with
                    // existing file/CD entries; addStream dedups by URL.
                    std::size_t idx = playlist_.addStream(url, label);
                    pl_cursor_  = (int)idx;   // scroll placed by the draw-time invariant
                    right_pane_ = RightPane::Playlist;
                    playlist_.selectAt(idx);
                    audio_.beginStream(url);   // non-blocking; connects on a worker thread
                    showTrackToast("Negotiating Radio Stream...", label, "");
                }
                break;
            }
            case InputMode::RadioSearch: {
                std::string q = goto_input_;
                while (!q.empty() && (q.front() == ' ' || q.front() == '\t')) q.erase(q.begin());
                while (!q.empty() && (q.back() == ' ' || q.back() == '\t' ||
                                      q.back() == '\r' || q.back() == '\n')) q.pop_back();
                if (!q.empty()) {
                    radio_results_   = RadioBrowser::search(q, 30);   // synchronous, bounded
                    in_radio_        = true;
                    in_radio_search_ = true;
                    dir_entries_.clear(); dir_display_.clear();
                    dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                    for (const auto& r : radio_results_) {
                        dir_entries_.push_back(r.url);
                        std::string meta;
                        if (r.bitrate > 0)          meta  = std::to_string(r.bitrate) + "k";
                        if (!r.codec.empty())       meta += (meta.empty() ? "" : " ") + r.codec;
                        if (!r.country.empty())     meta += (meta.empty() ? "" : ", ") + r.country;
                        std::string info = r.name + (meta.empty() ? "" : "  (" + meta + ")");
                        dir_display_.push_back(sanitizeForDisplay(info));
                    }
                    dir_cursor_ = 0; dir_scroll_ = 0;
                    if (radio_results_.empty())
                        showTrackToast("No stations found", q, "");
                }
                break;
            }
            case InputMode::LastfmKey: {
                std::string k = goto_input_;
                while (!k.empty() && (k.front()==' '||k.front()=='\t')) k.erase(k.begin());
                while (!k.empty() && (k.back()==' '||k.back()=='\t'||k.back()=='\r'||k.back()=='\n')) k.pop_back();
                if (!k.empty()) {
                    config_.lastfm_key = k;
                    openInputBar(InputMode::LastfmSecret, "");   // chain to secret prompt
                }
                break;
            }
            case InputMode::LastfmSecret: {
                std::string sec = goto_input_;
                while (!sec.empty() && (sec.front()==' '||sec.front()=='\t')) sec.erase(sec.begin());
                while (!sec.empty() && (sec.back()==' '||sec.back()=='\t'||sec.back()=='\r'||sec.back()=='\n')) sec.pop_back();
                if (!sec.empty()) {
                    config_.lastfm_secret = sec;
                    config_.save();           // creds persisted
                    lastfmBeginAuth();        // start browser authorization
                }
                break;
            }
            case InputMode::ListenBrainzToken: {
                std::string tok = goto_input_;
                while (!tok.empty() && (tok.front()==' '||tok.front()=='\t')) tok.erase(tok.begin());
                while (!tok.empty() && (tok.back()==' '||tok.back()=='\t'||tok.back()=='\r'||tok.back()=='\n')) tok.pop_back();
                if (!tok.empty()) {
                    showTrackToast("ListenBrainz: validating token...", "", "");
                    startListenBrainzValidate(tok);   // async; result applied on next tick (never blocks UI)
                }
                break;
            }
            case InputMode::PlaylistSearch: {
                // Use goto_input_ raw, NOT the separator-stripped `target` - the
                // stripping above is path-mode behaviour and would mangle a
                // query ending in '/' or '\'. Match case-insensitively against
                // display_title: exactly the text each playlist row shows.
                std::string q = goto_input_;
                for (char& c : q) c = (char)std::tolower((unsigned char)c);
                search_results_.clear();
                search_query_ = goto_input_;
                if (!q.empty()) {
                    for (std::size_t i = 0; i < playlist_.size(); ++i) {
                        std::string disp = playlist_.at(i).display_title;
                        for (char& c : disp) c = (char)std::tolower((unsigned char)c);
                        if (disp.find(q) != std::string::npos)
                            search_results_.push_back(i);
                    }
                }
                if (search_results_.empty()) {
                    showTrackToast("No matches for: " + goto_input_, "", "");
                } else if (search_results_.size() == 1) {
                    // Single hit: jump straight, skip the overlay.
                    jumpToPlaylistIndex(search_results_[0]);
                } else {
                    search_cursor_ = 0;
                    right_pane_ = RightPane::SearchResults;
                }
                break;
            }
        }
    }
    tab_matches_.clear();
    tab_idx_    = -1;
    redraw_needed_.store(true);
}

std::vector<std::string> UIManager::gotoGetMatches(const std::string& partial) const {
    std::vector<std::string> matches;
    fs::path p(partial);
    // Determine directory to search and prefix to match
    fs::path search_dir;
    std::string stem;
    if (partial.empty() || partial == "/" || partial == "\\") {
        search_dir = partial.empty() ? "." : partial;
        stem = "";
    } else if (fs::is_directory(p)) {
        // Exact dir — list its children
        search_dir = p;
        stem = "";
    } else {
        search_dir = p.parent_path().empty() ? fs::path(".") : p.parent_path();
        stem = p.filename().string();
    }

    std::string stem_lower = stem;
    std::transform(stem_lower.begin(), stem_lower.end(), stem_lower.begin(), ::tolower);

    try {
        for (const auto& de : fs::directory_iterator(search_dir)) {
            bool is_dir = de.is_directory();
            // In LoadM3U mode also match .m3u files; otherwise dirs only
            bool is_m3u = false;
            if (!is_dir && de.is_regular_file()) {
                std::string ext = de.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                is_m3u = (ext == ".m3u" || ext == ".m3u8" || ext == ".pls" || ext == ".xspf");
            }
            if (!is_dir && !is_m3u) continue;
            std::string name = de.path().filename().string();
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            if (stem.empty() || name_lower.substr(0, stem_lower.size()) == stem_lower) {
                std::string full = de.path().string();
                // Normalise separators to backslash on Windows
#ifdef _WIN32
                for (auto& c : full) if (c == '/') c = '\\';
#endif
                matches.push_back(full);
            }
        }
    } catch (...) {}

    std::sort(matches.begin(), matches.end());
    return matches;
}

void UIManager::gotoTabComplete() {
    // First Tab press — compute matches
    if (tab_idx_ == -1) {
        tab_prefix_  = goto_input_;
        tab_matches_ = gotoGetMatches(goto_input_);
        if (tab_matches_.empty()) return;
        tab_idx_ = 0;
    } else {
        // Subsequent Tab — cycle forward
        tab_idx_ = (tab_idx_ + 1) % (int)tab_matches_.size();
    }

    goto_input_  = tab_matches_[(size_t)tab_idx_];
    // Append separator if it's a directory so user can keep drilling down
    if (fs::is_directory(goto_input_) && goto_input_.back() != '\\' && goto_input_.back() != '/')
        goto_input_ += '\\';
    goto_cursor_ = (int)goto_input_.size();
}

void UIManager::handleGotoInput(int ch) {
    // Any non-Tab keystroke resets tab-completion cycle
    auto resetTab = [&]() {
        if (tab_idx_ != -1) {
            tab_matches_.clear();
            tab_idx_ = -1;
        }
    };

    switch (ch) {
        case 27:  // Escape
            resetTab();
            gotoClose(false);
            break;
        case '\n': case KEY_ENTER: case '\r':
            resetTab();
            gotoClose(true);
            break;
        case '\t':
            gotoTabComplete();
            break;
        case KEY_BACKSPACE: case 127: case 8:
            resetTab();
            if (!goto_input_.empty()) {
                goto_input_.pop_back();
                goto_cursor_ = (int)goto_input_.size();
            }
            break;
        case KEY_LEFT:
            resetTab();
            if (goto_cursor_ > 0) --goto_cursor_;
            break;
        case KEY_RIGHT:
            resetTab();
            if (goto_cursor_ < (int)goto_input_.size()) ++goto_cursor_;
            break;
        case KEY_HOME:
            resetTab();
            goto_cursor_ = 0;
            break;
        case KEY_END:
            resetTab();
            goto_cursor_ = (int)goto_input_.size();
            break;
        default:
            // Printable ASCII
            if (ch >= 32 && ch < 256) {
                resetTab();
                goto_input_.insert(goto_input_.begin() + goto_cursor_, (char)ch);
                ++goto_cursor_;
            }
            break;
    }
    redraw_needed_.store(true);
}

void UIManager::toggleFocus() {
    focus_ = (focus_ == Pane::DirBrowser) ? Pane::Playlist : Pane::DirBrowser;
}

void UIManager::navigateDown() {
    if (focus_ == Pane::DirBrowser) {
        int v = paneVisibleRows(win_dir_);
        if (dir_cursor_+1 < (int)dir_entries_.size()) {
            ++dir_cursor_;
            if (dir_cursor_ >= dir_scroll_+v) ++dir_scroll_;
        }
    } else {
        if (pl_cursor_+1 < (int)playlist_.size()) ++pl_cursor_;   // scroll follows via the invariant
    }
}

void UIManager::navigateUp() {
    if (focus_ == Pane::DirBrowser) {
        if (dir_cursor_ > 0) { --dir_cursor_; if (dir_cursor_ < dir_scroll_) dir_scroll_ = dir_cursor_; }
    } else {
        if (pl_cursor_ > 0) --pl_cursor_;   // scroll follows via the invariant
    }
}

// PgUp/PgDn: move the focused pane's cursor by one visible page (visible-1, the
// standard pager overlap: the old view's last row becomes the new view's first).
// Page size is the REAL pane height via paneVisibleRows, not a constant.
void UIManager::navigatePage(int dir) {
    if (focus_ == Pane::DirBrowser) {
        int n = (int)dir_entries_.size();
        if (n == 0) return;
        int v = std::max(1, paneVisibleRows(win_dir_) - 1);
        dir_cursor_ = std::clamp(dir_cursor_ + dir * v, 0, n - 1);
        // No draw-time scroll invariant in the browser (j/k nudge per-handler):
        // clamp scroll to keep the paged cursor visible ourselves.
        int vis = paneVisibleRows(win_dir_);
        if (dir_cursor_ < dir_scroll_) dir_scroll_ = dir_cursor_;
        else if (vis > 0 && dir_cursor_ >= dir_scroll_ + vis)
            dir_scroll_ = dir_cursor_ - vis + 1;
    } else {
        int n = (int)playlist_.size();
        if (n == 0) return;
        int v = std::max(1, paneVisibleRows(win_playlist_) - 1);
        pl_cursor_ = std::clamp(pl_cursor_ + dir * v, 0, n - 1);
        // scroll follows via the slice-5 draw-time invariant
    }
    redraw_needed_.store(true);
}

// Home/End: cursor to the first / last row of the focused list.
void UIManager::navigateHomeEnd(bool to_end) {
    if (focus_ == Pane::DirBrowser) {
        int n = (int)dir_entries_.size();
        if (n == 0) return;
        dir_cursor_ = to_end ? n - 1 : 0;
        int vis = paneVisibleRows(win_dir_);
        if (dir_cursor_ < dir_scroll_) dir_scroll_ = dir_cursor_;
        else if (vis > 0 && dir_cursor_ >= dir_scroll_ + vis)
            dir_scroll_ = dir_cursor_ - vis + 1;
    } else {
        int n = (int)playlist_.size();
        if (n == 0) return;
        pl_cursor_ = to_end ? n - 1 : 0;   // scroll via the invariant
    }
    redraw_needed_.store(true);
}

void UIManager::activateSelection() {
    if (focus_ == Pane::DirBrowser) {
        if (dir_cursor_ >= (int)dir_entries_.size()) return;
        const auto& name = dir_entries_[(size_t)dir_cursor_];
        if (in_drive_list_) {
            if (name == "[Recent]") {
                // Show recently played as virtual dir
                in_drive_list_ = false;
                in_recent_     = true;
                dir_entries_.clear();
                dir_display_.clear();
                dir_entries_.push_back("[Drives]");
                dir_display_.push_back("[Drives]");
                for (const auto& p : config_.recent_tracks) {
                    dir_entries_.push_back(p);
                    std::string disp = fs::path(p).filename().string();
                    dir_display_.push_back(sanitizeForDisplay(disp.empty() ? p : disp));
                }
                dir_cursor_ = 0; dir_scroll_ = 0;
                return;
            }
            if (name == "[FAVs]") {
                in_drive_list_ = false;
                in_favs_       = true;
                dir_entries_.clear(); dir_display_.clear();
                dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                for (const auto& fp : config_.fav_tracks) {
                    dir_entries_.push_back(fp);
                    std::string disp = fs::path(fp).filename().string();
                    dir_display_.push_back(sanitizeForDisplay(disp.empty() ? fp : disp));
                }
                dir_cursor_ = 0; dir_scroll_ = 0;
                return;
            }
            if (name == "[Radio]") {
                in_drive_list_ = false;
                in_radio_      = true;
                dir_entries_.clear(); dir_display_.clear();
                dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                for (const auto& st : config_.radio_stations) {
                    dir_entries_.push_back(st);
                    dir_display_.push_back(sanitizeForDisplay(stationLabel(st)));
                }
                dir_cursor_ = 0; dir_scroll_ = 0;
                return;
            }
            if (name == "[Books]") {
                in_drive_list_ = false;
                in_books_      = true;
                dir_entries_.clear(); dir_display_.clear();
                dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                for (const auto& bk : config_.audiobooks) {
                    dir_entries_.push_back(bk);
                    std::string disp = fs::path(bk).filename().string();
                    dir_display_.push_back(sanitizeForDisplay(disp.empty() ? bk : disp));
                }
                dir_cursor_ = 0; dir_scroll_ = 0;
                return;
            }
            if (name == "[Bookmarks]") {
                right_pane_      = RightPane::Bookmarks;
                bookmark_cursor_ = 0;
                return;
            }
            activateDrive(name);
            return;
        }

        if (in_recent_) {
            if (name == "[Drives]" || name == "[Back]") {
                in_recent_ = false;
                refreshDir();
                return;
            }
            // Play the selected recent track
            if (fs::exists(name) && PlaylistManager::isSupportedAudio(name)) {
                size_t idx = playlist_.addTrack(name);
                playlist_.selectAt(idx);
                if (auto p = playlist_.currentPath(); p.has_value()) {
                    audio_.play(p.value());
                    maybePreloadNext();
                }
            }
            return;
        }
        if (in_radio_) {
            if (name == "[Back]") {
                if (in_radio_search_) {
                    // Return from search results to the saved-station list.
                    in_radio_search_ = false;
                    dir_entries_.clear(); dir_display_.clear();
                    dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
                    for (const auto& st : config_.radio_stations) {
                        dir_entries_.push_back(st);
                        dir_display_.push_back(sanitizeForDisplay(stationLabel(st)));
                    }
                    dir_cursor_ = 0; dir_scroll_ = 0;
                    return;
                }
                in_radio_ = false;
                refreshDir();
                return;
            }
            // Resolve the station. Search results carry the real station name +
            // uuid (persist the name); saved stations keep their stored name.
            std::string url, uuid;
            if (in_radio_search_) {
                int ri = dir_cursor_ - 1;            // [Back] occupies index 0
                if (ri < 0 || ri >= (int)radio_results_.size()) return;
                url   = radio_results_[(size_t)ri].url;
                uuid  = radio_results_[(size_t)ri].uuid;
                config_.addRadioStation(url, radio_results_[(size_t)ri].name);  // persist real name
            } else {
                url   = name;
                config_.addRadioStation(url);        // preserve any stored name
            }
            config_.save();                          // flush so persisted names survive restart
            // Unified label so search-played and saved stations read the same,
            // now and after restart: "RADIO: <name>" if known, else derived.
            std::string label = stationLabel(url);
            if (!uuid.empty()) RadioBrowser::countClick(uuid);  // popularity courtesy
            // Append the station and jump to it — coexists with existing file/CD
            // entries (delete from the playlist to remove). addStream dedups by
            // URL, so re-selecting a station just jumps to its existing row.
            std::size_t idx = playlist_.addStream(url, label);
            pl_cursor_ = (int)idx;   // reported bug fix: scroll placed by the draw-time invariant
            playlist_.selectAt(idx);
            right_pane_ = RightPane::Playlist;
            in_radio_search_ = false;
            audio_.beginStream(url);   // non-blocking; connects on a worker thread
            showTrackToast("Negotiating Radio Stream...", label, "");
            return;
        }
        if (in_favs_) {
            if (name == "[Back]") {
                in_favs_ = false;
                refreshDir();
                return;
            }
            if (!fs::exists(name)) return;  // dead path — ignore
            if (PlaylistManager::isSupportedAudio(name)) {
                // Add to playlist and play
                size_t idx = playlist_.addTrack(name);
                playlist_.selectAt(idx);
                if (auto p = playlist_.currentPath(); p.has_value()) {
                    audio_.play(p.value());
                    config_.addRecentTrack(p.value());
                    maybePreloadNext();
                }
                // Also navigate browser to that file's parent dir
                fs::path parent = fs::path(name).parent_path();
                if (fs::exists(parent)) {
                    current_dir_ = parent.string();
                    in_favs_ = false;
                    refreshDir();
                }
            }
            return;
        }
        if (in_books_) {
            if (name == "[Back]") {
                in_books_ = false;
                refreshDir();
                return;
            }
            if (!fs::exists(name)) return;            // dead path — ignore
            if (PlaylistManager::isSupportedAudio(name)) {
                size_t idx = playlist_.addTrack(name);
                playlist_.selectAt(idx);
                if (auto p = playlist_.currentPath(); p.has_value()) {
                    audio_.play(p.value());
                    config_.addAudiobook(p.value());   // keep/refresh in [Books]
                    config_.addRecentTrack(p.value());
                    maybePreloadNext();
                }
            }
            return;
        }
        if (name == "[Drives]") { enterDriveList(); return; }
        if (name == "[Radio]") {
            in_radio_  = true;
            in_recent_ = false;
            in_favs_   = false;
            in_drive_list_ = false;
            dir_entries_.clear(); dir_display_.clear();
            dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
            for (const auto& st : config_.radio_stations) {
                dir_entries_.push_back(st);
                dir_display_.push_back(sanitizeForDisplay(stationLabel(st)));
            }
            dir_cursor_ = 0; dir_scroll_ = 0;
            return;
        }
        if (name == "[Recent]") {
            in_recent_ = true;
            in_favs_   = false;
            in_drive_list_ = false;
            dir_entries_.clear(); dir_display_.clear();
            dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
            for (const auto& rp : config_.recent_tracks) {
                dir_entries_.push_back(rp);
                std::string disp = fs::path(rp).filename().string();
                dir_display_.push_back(sanitizeForDisplay(disp.empty() ? rp : disp));
            }
            dir_cursor_ = 0; dir_scroll_ = 0;
            return;
        }
        if (name == "[FAVs]") {
            in_favs_   = true;
            in_recent_ = false;
            in_drive_list_ = false;
            dir_entries_.clear(); dir_display_.clear();
            dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
            for (const auto& fp : config_.fav_tracks) {
                dir_entries_.push_back(fp);
                std::string disp = fs::path(fp).filename().string();
                dir_display_.push_back(sanitizeForDisplay(disp.empty() ? fp : disp));
            }
            dir_cursor_ = 0; dir_scroll_ = 0;
            return;
        }
        if (name == "[Books]") {
            in_books_  = true;
            in_favs_   = false;
            in_recent_ = false;
            in_drive_list_ = false;
            dir_entries_.clear(); dir_display_.clear();
            dir_entries_.push_back("[Back]"); dir_display_.push_back("[Back]");
            for (const auto& bk : config_.audiobooks) {
                dir_entries_.push_back(bk);
                std::string disp = fs::path(bk).filename().string();
                dir_display_.push_back(sanitizeForDisplay(disp.empty() ? bk : disp));
            }
            dir_cursor_ = 0; dir_scroll_ = 0;
            return;
        }
        if (name == "..") {
            fs::path parent = fs::path(current_dir_).parent_path();
            if (!parent.empty() && parent.string() != current_dir_) {
                current_dir_ = parent.string();
                dir_cursor_ = 0; dir_scroll_ = 0; refreshDir();
            } else {
                enterDriveList();
            }
            return;
        }
        fs::path full = fs::path(current_dir_) / name;
        if (fs::is_directory(full)) {
            current_dir_ = full.string();
            dir_cursor_ = 0; dir_scroll_ = 0; refreshDir();
        } else if (PlaylistManager::isSupportedAudio(full.string())) {
            size_t idx = playlist_.addTrack(full.string());
            playlist_.selectAt(idx);
            if (auto p = playlist_.currentPath(); p.has_value()) {
                audio_.play(p.value());
                config_.addRecentTrack(p.value());
                maybePreloadNext();
            }
        } else {
            // M3U file — load it
            std::string ext = full.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".m3u" || ext == ".m3u8" || ext == ".pls" || ext == ".xspf") {
                playlist_.loadPlaylist(full.string());
                if (int miss = playlist_.lastLoadMissing(); miss > 0)
                    showTrackToast("Playlist loaded", std::to_string(miss) + " file(s) not found", "");
                pl_cursor_ = 0; pl_scroll_ = 0;
            }
        }
    } else {
        if (pl_cursor_ < (int)playlist_.size()) {
            playlist_.selectAt((size_t)pl_cursor_);
            if (auto p = playlist_.currentPath(); p.has_value()) {
                const std::string& path = p.value();
                std::string drive; int track_num;
                if (parseCDPath(path, drive, track_num)) {
                    if (!reopenCDForAction(drive)) return;   // ejected while stopped
                    audio_.playCDTrack(track_num);
                    return;
                }
                // Regular file — exits CD mode automatically inside play()
                audio_.play(path);
                config_.addRecentTrack(path);
                maybePreloadNext();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Drive helpers
// ─────────────────────────────────────────────────────────────────────────────
#ifndef _WIN32
// /proc/mounts encodes space/tab/newline/backslash in mount paths as octal \NNN
// escapes; decode them so e.g. "/media/dave/My\040USB" browses as "My USB".
static std::string decodeMountOctal(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '7' &&
            s[i+2] >= '0' && s[i+2] <= '7' &&
            s[i+3] >= '0' && s[i+3] <= '7') {
            out += (char)((s[i+1]-'0')*64 + (s[i+2]-'0')*8 + (s[i+3]-'0'));
            i += 3;
        } else out += s[i];
    }
    return out;
}
#endif

std::vector<std::string> UIManager::listDrives() {
    std::vector<std::string> drives;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) {
            std::string d; d += (char)('A'+i); d += ":\\";
            drives.push_back(d);
        }
#else
    // Linux (slice 6): CD devices first (/dev/sr*), then browsable roots + the
    // user's mount points. CD entries route to CD mode in activateDrive; every
    // other entry is a directory the browser descends into. This is also the fix
    // for the empty [Drive] menu on Linux — Windows enumerated drive letters here.
    try {
        std::vector<std::string> cds;
        for (const auto& de : fs::directory_iterator("/dev")) {
            std::string nm = de.path().filename().string();
            if (nm.size() > 2 && nm.compare(0, 2, "sr") == 0 &&
                std::all_of(nm.begin() + 2, nm.end(),
                            [](unsigned char c){ return c >= '0' && c <= '9'; }))
                cds.push_back(de.path().string());              // "/dev/sr0"
        }
        std::sort(cds.begin(), cds.end());
        drives.insert(drives.end(), cds.begin(), cds.end());
    } catch (...) {}
    // Filesystem root + home directory as browse starting points.
    drives.push_back("/");
    if (const char* h = std::getenv("HOME"); h && *h) drives.push_back(h);
    // User / removable mount points from /proc/mounts (under /media, /run/media,
    // /mnt) — where udisks / desktop environments mount USB sticks, discs, phones.
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        // fields: device  mountpoint  fstype ...  (mountpoint is octal-escaped)
        auto s1 = line.find(' ');
        if (s1 == std::string::npos) continue;
        auto s2 = line.find(' ', s1 + 1);
        if (s2 == std::string::npos) continue;
        std::string mp = decodeMountOctal(line.substr(s1 + 1, s2 - s1 - 1));
        if (mp.rfind("/media/", 0) == 0 || mp.rfind("/run/media/", 0) == 0 ||
            mp.rfind("/mnt/", 0) == 0)
            if (std::find(drives.begin(), drives.end(), mp) == drives.end())
                drives.push_back(mp);
    }
#endif
    return drives;
}

void UIManager::enterDriveList() {
    in_drive_list_ = true;
    in_recent_     = false;
    in_favs_       = false;
    in_radio_      = false;
    in_books_      = false;
    dir_entries_   = listDrives();
    dir_display_   = dir_entries_;
    // Prepend virtual entries at top
    dir_entries_.insert(dir_entries_.begin(), "[Bookmarks]");
    dir_display_.insert(dir_display_.begin(), "[Bookmarks]");
    dir_entries_.insert(dir_entries_.begin(), "[Books]");
    dir_display_.insert(dir_display_.begin(), "[Books]");
    dir_entries_.insert(dir_entries_.begin(), "[Radio]");
    dir_display_.insert(dir_display_.begin(), "[Radio]");
    dir_entries_.insert(dir_entries_.begin(), "[FAVs]");
    dir_display_.insert(dir_display_.begin(), "[FAVs]");
    dir_entries_.insert(dir_entries_.begin(), "[Recent]");
    dir_display_.insert(dir_display_.begin(), "[Recent]");
    dir_cursor_    = 0;
    dir_scroll_    = 0;

    // Media detection for the Shift+E eject hint - ONCE, here, at enumeration
    // time (entering [Drives] or F12), NEVER polled: a CHECK_VERIFY-class probe
    // repeated on an idle drive spins it up audibly (probe B2 / slice 1). Each
    // CD drive gets one seam open + one mediaPresent, both user-triggered.
    // Concurrent opens of a drive CDSource holds are contract (see CdIoWin).
    cd_drives_with_media_.clear();
    for (const auto& e : dir_entries_) {
        std::string spec;
#ifdef _WIN32
        if (e.size() >= 2 && e[1] == ':') {
            std::wstring w(e.begin(), e.end());
            if (GetDriveTypeW(w.c_str()) == DRIVE_CDROM) spec = e.substr(0, 1);
        }
#else
        if (e.rfind("/dev/sr", 0) == 0) spec = e.substr(5);   // "sr0"
#endif
        if (spec.empty()) continue;
        if (auto dev = core::cdio().open(spec); dev && dev->mediaPresent())
            cd_drives_with_media_.push_back(e);
    }
}

void UIManager::activateDrive(const std::string& drive_entry) {
#ifdef _WIN32
    // Check if this is a CD-ROM drive
    std::string drive_path = drive_entry;
    if (drive_path.back() != '\\' && drive_path.back() != '/')
        drive_path += "\\";
    std::wstring wdrive(drive_path.begin(), drive_path.end());
    if (GetDriveTypeW(wdrive.c_str()) == DRIVE_CDROM) {
        // Extract single drive letter (e.g. "D" from "D:\")
        std::string letter = drive_entry.substr(0, 1);
        // Stop file playback and switch to CD mode
        if (audio_.cdMode()) audio_.closeCD();
        if (audio_.openCD(letter)) {
            // Purge any existing CD track entries for this drive letter
            // (handles re-inserting same drive or switching discs)
            std::string prefix = letter + ":CD Track ";
            playlist_.removeIf([&](const PlaylistEntry& e) {
                return e.path.substr(0, prefix.size()) == prefix;
            });
            cd_drive_letter_ = letter;
            cd_poll_ticks_   = 0;
            cd_fail_count_   = 0;
            // Populate playlist with CD tracks
            playlist_.clear();
            for (const auto& t : audio_.cdTracks()) {
                std::string title = std::string("CD Track ") +
                                    (t.number < 10 ? "0" : "") +
                                    std::to_string(t.number);
                // Add a fake path so PlaylistManager can hold it
                playlist_.addCDTrack(letter + ":" + title, title, t.duration_sec);
            }
            pl_cursor_ = 0; pl_scroll_ = 0;
            last_playlist_current_for_sync_ = 0;
            right_pane_ = RightPane::Playlist;
            return;
        }
        // Fall through to normal browse if CD open failed
    }
#else
    // Linux (slice 6): a /dev/sr* entry is a CD drive → enter CD mode; anything
    // else (root, mount point) is a directory → the fs::exists browse below.
    if (drive_entry.rfind("/dev/sr", 0) == 0) {
        // spec = device basename ("sr0"); CdIoSgIo maps it to /dev/sr0, and the
        // same basename is the CD-path key ("sr0:CD Track NN") parseCDPath reads.
        std::string spec = drive_entry.substr(5);           // strip "/dev/"
        if (audio_.cdMode()) audio_.closeCD();
        if (audio_.openCD(spec)) {
            std::string prefix = spec + ":CD Track ";
            playlist_.removeIf([&](const PlaylistEntry& e) {
                return e.path.substr(0, prefix.size()) == prefix;
            });
            cd_drive_letter_ = spec;
            cd_poll_ticks_   = 0;
            cd_fail_count_   = 0;
            playlist_.clear();
            for (const auto& t : audio_.cdTracks()) {
                std::string title = std::string("CD Track ") +
                                    (t.number < 10 ? "0" : "") +
                                    std::to_string(t.number);
                playlist_.addCDTrack(spec + ":" + title, title, t.duration_sec);
            }
            pl_cursor_ = 0; pl_scroll_ = 0;
            last_playlist_current_for_sync_ = 0;
            right_pane_ = RightPane::Playlist;
            return;
        }
        return;   // CD open failed — don't try to browse a device node as a dir
    }
#endif
    if (fs::exists(drive_entry)) {
        current_dir_   = drive_entry;
        in_drive_list_ = false;
        dir_cursor_    = 0;
        dir_scroll_    = 0;
        refreshDir();
    }
}

void UIManager::refreshDir() {
    in_drive_list_ = false;
    in_recent_     = false;
    in_favs_       = false;
    in_radio_      = false;
    in_books_      = false;
    dir_entries_.clear();
    dir_display_.clear();
    dir_poll_ticks_ = 0;
    try { dir_mtime_ = fs::last_write_time(current_dir_); } catch (...) {}
    try {
        fs::path p(current_dir_);
        bool at_root = (p.parent_path() == p);
        std::string nav = at_root ? "[Drives]" : "..";

        // Always pin [Drives], [Recent], [FAVs] at top
        dir_entries_.push_back("[Drives]");
        dir_display_.push_back("[Drives]");
        dir_entries_.push_back("[Recent]");
        dir_display_.push_back("[Recent]");
        dir_entries_.push_back("[FAVs]");
        dir_display_.push_back("[FAVs]");
        dir_entries_.push_back("[Radio]");
        dir_display_.push_back("[Radio]");
        dir_entries_.push_back("[Books]");
        dir_display_.push_back("[Books]");
        if (!at_root) {
            dir_entries_.push_back("..");
            dir_display_.push_back("..");
        }

        for (const auto& de : fs::directory_iterator(current_dir_)) {
            std::string nm = de.path().filename().string();
            if (nm.empty()) continue;
            // Skip dot-files unless show_hidden_
            if (nm[0] == '.' && !show_hidden_) continue;
            // Skip Windows hidden-attribute files unless show_hidden_
#ifdef _WIN32
            if (!show_hidden_) {
                DWORD attr = GetFileAttributesW(de.path().wstring().c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_HIDDEN))
                    continue;
            }
#endif
            std::string ext = fs::path(nm).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            bool is_m3u = (ext == ".m3u" || ext == ".m3u8" || ext == ".pls" || ext == ".xspf");
            if (de.is_directory() || PlaylistManager::isSupportedAudio(de.path().string()) || is_m3u) {
                dir_entries_.push_back(nm);
                dir_display_.push_back(sanitizeForDisplay(nm));
            }
        }

        std::vector<std::size_t> idx(dir_entries_.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::stable_sort(idx.begin(), idx.end(),
            [&](std::size_t a, std::size_t b) {
                const auto& ea = dir_entries_[a];
                const auto& eb = dir_entries_[b];
                // Virtual entries always first
                if (ea == "[Drives]")               return true;
                if (eb == "[Drives]")               return false;
                if (ea == "[Recent]")               return true;
                if (eb == "[Recent]")               return false;
                if (ea == "[FAVs]")                 return true;
                if (eb == "[FAVs]")                 return false;
                if (ea == "[Radio]")                return true;
                if (eb == "[Radio]")                return false;
                if (ea == "[Books]")                return true;
                if (eb == "[Books]")                return false;
                if (ea == "..")                     return true;
                if (eb == "..")                     return false;

                bool ad = fs::is_directory(fs::path(current_dir_)/ea);
                bool bd = fs::is_directory(fs::path(current_dir_)/eb);

                switch (browser_sort_) {
                    case BrowserSort::Name:
                        if (ad != bd) return ad > bd;
                        return ea < eb;
                    case BrowserSort::Modified: {
                        if (ad != bd) return ad > bd;
                        try {
                            auto ta = fs::last_write_time(fs::path(current_dir_)/ea);
                            auto tb = fs::last_write_time(fs::path(current_dir_)/eb);
                            return ta > tb;  // newer first
                        } catch (...) { return ea < eb; }
                    }
                    case BrowserSort::Size: {
                        if (ad != bd) return ad > bd;
                        try {
                            auto sa = ad ? 0 : (std::intmax_t)fs::file_size(fs::path(current_dir_)/ea);
                            auto sb = bd ? 0 : (std::intmax_t)fs::file_size(fs::path(current_dir_)/eb);
                            return sa > sb;  // larger first
                        } catch (...) { return ea < eb; }
                    }
                }
                return ea < eb;
            });

        std::vector<std::string> sorted_e, sorted_d;
        sorted_e.reserve(dir_entries_.size());
        sorted_d.reserve(dir_display_.size());
        for (auto i : idx) {
            sorted_e.push_back(std::move(dir_entries_[i]));
            sorted_d.push_back(std::move(dir_display_[i]));
        }
        dir_entries_ = std::move(sorted_e);
        dir_display_ = std::move(sorted_d);
    } catch (...) {}
}

std::string UIManager::formatTime(double s) const {
    if (s < 0.0) s = 0.0;   // guard against negative (e.g. position overrun) -> "-1:-30"
    int si=(int)s, m=si/60; si%=60; int h=m/60; m%=60;
    std::ostringstream ss;
    if (h > 0) ss << h << ':' << std::setw(2) << std::setfill('0');
    ss << m << ':' << std::setw(2) << std::setfill('0') << si;
    return ss.str();
}

// ── Scrolled text helper ──────────────────────────────────────────────────────
// Returns a substring of `text` of length `width` using the shared scroll
// offset. If text fits within width, returns it padded. If not, scrolls.
// (Removed: all consumers now use the column-aware scrollToWidth in StringUtils.h.)

// convert-core: the absolute path of browser entry idx, respecting the mode.
// Favs/Recent/Books entries are already absolute; normal-dir entries join
// current_dir_. Pseudo-entries and drive/radio modes have no file path.
std::string UIManager::browserEntryPath(int idx) const {
    if (idx < 0 || idx >= (int)dir_entries_.size()) return {};
    const std::string& nm = dir_entries_[(size_t)idx];
    if (nm.empty() || nm == ".." || nm == "[Back]" || nm.front() == '[') return {};
    if (in_drive_list_ || in_radio_) return {};
    namespace fs = std::filesystem;
    return fs::path(nm).is_absolute() ? nm : (fs::path(current_dir_) / nm).string();
}

// convert-core: pick the convert scope (this file / this folder / marked set).
void UIManager::drawConvertScope() {
    const int BOX_W = 60, BOX_H = 11;
    int y0 = (screen_rows_ - BOX_H) / 2, x0 = (screen_cols_ - BOX_W) / 2;
    if (y0 < 0) y0 = 0; if (x0 < 0) x0 = 0;
    WINDOW* w = newwin(BOX_H, BOX_W, y0, x0);
    if (!w) return;
    wbkgd(w, config_.awesome_mode ? COLOR_PAIR(CP_DIM) : COLOR_PAIR(0));
    werase(w);
    const char* title = " CONVERT ";
    panelFrame(w, title, true);
    if (!config_.awesome_mode) mvwaddstr(w, 0, (BOX_W - (int)strlen(title)) / 2, title);

    int folder_n = 0;
    if (!convert_src_dir_.empty()) {
        std::error_code ec;
        namespace fs = std::filesystem;
        for (fs::directory_iterator it(convert_src_dir_, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && convertSupportedInput(it->path().string())) ++folder_n;
    }

    mvwaddstr(w, 2, 3, "Convert to another format:");
    auto row = [&](int y, bool on, const std::string& s) {
        if (!on) wattron(w, A_DIM);
        mvwaddnstr(w, y, 3, s.c_str(), BOX_W - 5);
        if (!on) wattroff(w, A_DIM);
    };
    row(4, !convert_single_.empty(), convert_single_.empty()
        ? "[1] This file: (no audio file focused)"
        : "[1] This file: " + sanitizeForDisplay(std::filesystem::path(convert_single_).filename().string()));
    row(5, !convert_src_dir_.empty(), convert_src_dir_.empty()
        ? "[2] This folder: (n/a here)"
        : "[2] This folder: " + std::to_string(folder_n) + " file(s)");
    row(6, !marked_.empty(), marked_.empty()
        ? "[3] Marked set: (none - press u to mark)"
        : "[3] Marked set: " + std::to_string((int)marked_.size()) + " file(s)");
    mvwaddstr(w, 8, 3, "[1/2/3] pick   [Esc] cancel");
    wrefresh(w); delwin(w);
}

// convert-core: pick the output format + quality (single-select + the shared
// per-row quality editor). Quality edits the rip config knobs (the reuse).
void UIManager::drawConvertConfirm() {
    const int BOX_W = 62, BOX_H = 12 + kRipFormatCount;
    int y0 = (screen_rows_ - BOX_H) / 2, x0 = (screen_cols_ - BOX_W) / 2;
    if (y0 < 0) y0 = 0; if (x0 < 0) x0 = 0;
    WINDOW* w = newwin(BOX_H, BOX_W, y0, x0);
    if (!w) return;
    wbkgd(w, config_.awesome_mode ? COLOR_PAIR(CP_DIM) : COLOR_PAIR(0));
    werase(w);
    const char* title = " CONVERT TO ";
    panelFrame(w, title, true);
    if (!config_.awesome_mode) mvwaddstr(w, 0, (BOX_W - (int)strlen(title)) / 2, title);

    namespace fs = std::filesystem;
    std::string scope;
    if (convert_scope_ == 1) scope = "1 file: " + fs::path(convert_single_).filename().string();
    else if (convert_scope_ == 2) scope = "folder: " + fs::path(convert_src_dir_).filename().string();
    else if (convert_scope_ == 3) scope = std::to_string((int)marked_.size()) + " marked file(s)";
    mvwaddnstr(w, 2, 3, sanitizeForDisplay(scope).c_str(), BOX_W - 5);

    mvwaddstr(w, 4, 3, "Output format");
    mvwaddstr(w, 4, BOX_W - 15, "(1-6 select)");
    for (int i = 0; i < kRipFormatCount; ++i) {
        const auto& r = kRipFormats[i];
        bool alt = r.id == RipFormat::Mp3 ? config_.mp3_cbr
                 : r.id == RipFormat::Opus ? !config_.opus_vbr
                 : r.id == RipFormat::M4a ? !config_.aac_vbr : false;
        std::string q = encoderQualityLabel(r.id, alt, config_.mp3,
            r.id == RipFormat::Mp3 ? config_.mp3_cbr_bitrate
            : r.id == RipFormat::M4a ? config_.aac_cbr_bitrate : config_.opus_bitrate,
            config_.flac_level, config_.wavpack_mode, config_.aac_vbr_level);
        const char* cur = (i == convert_focus_) ? "> " : "  ";
        mvwprintw(w, 5 + i, 3, "%s(%c) %d  %-8s %s",
                  cur, (convert_fmt_ == r.id) ? '*' : ' ', i + 1, r.label, q.c_str());
    }
    {
        const auto& fr = kRipFormats[convert_focus_];
        bool alt = fr.id == RipFormat::Mp3 ? config_.mp3_cbr
                 : fr.id == RipFormat::Opus ? !config_.opus_vbr
                 : fr.id == RipFormat::M4a ? !config_.aac_vbr : false;
        if (const char* hint = encoderAxisHint(fr.id, alt))
            mvwaddstr(w, 5 + kRipFormatCount, 5, hint);
    }
    int note_y = 6 + kRipFormatCount;
    mvwaddstr(w, note_y, 3, "Output is 44.1 kHz (sources are resampled).");
    if (convert_scope_ == 1 && !convert_single_.empty()) {
        int sr = 0;
        try {
#ifdef _WIN32
            TagLib::FileRef f(utf8_to_wide(convert_single_).c_str(), true);
#else
            TagLib::FileRef f(convert_single_.c_str(), true);
#endif
            if (!f.isNull() && f.audioProperties()) sr = f.audioProperties()->sampleRate();
        } catch (...) {}
        if (sr > 44100) {
            wattron(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
            mvwprintw(w, note_y + 1, 3, "Source is %d kHz; output will be 44.1 kHz.", sr / 1000);
            wattroff(w, COLOR_PAIR(CP_STATUS_ERR) | A_BOLD);
        }
    }
    mvwaddstr(w, BOX_H - 2, 3, "[Enter] convert   [Esc] cancel");
    wrefresh(w); delwin(w);
}

void UIManager::drawMBSearch() {
    const int BOX_W = std::min(screen_cols_ - 4, 72);
    const int BOX_H = std::min(screen_rows_ - 4, 24);
    int y0 = (screen_rows_ - BOX_H) / 2;
    int x0 = (screen_cols_ - BOX_W) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;

    // Snapshot fields written by worker callbacks under mb_mutex_
    // so draw never races with the search/fetch thread.
    bool        snap_searching;
    std::string snap_status;
    std::vector<MBSearchResult> snap_results;
    int         snap_field, snap_list_cursor;
    {
        std::lock_guard<std::mutex> lk(mb_mutex_);
        snap_searching   = mb_search_.searching;
        snap_status      = mb_search_.status_msg;
        snap_results     = mb_search_.results;
        snap_field       = mb_search_.field;
        snap_list_cursor = mb_search_.list_cursor;
    }
    // Input-only fields (artist_buf, album_buf, cursors) are only ever
    // written on the UI thread, so no lock needed for those.

    // Reuse or create the modal window — avoids newwin/delwin churn every 80ms
    // which can corrupt ncurses window list on Windows after many iterations.
    if (!mb_search_win_ || getmaxx(mb_search_win_) != BOX_W
                        || getmaxy(mb_search_win_) != BOX_H) {
        if (mb_search_win_) { delwin(mb_search_win_); mb_search_win_ = nullptr; }
        mb_search_win_ = newwin(BOX_H, BOX_W, y0, x0);
    } else {
        mvwin(mb_search_win_, y0, x0);
    }
    WINDOW* w = mb_search_win_;
    if (!w) return;
    // Themed base bg (Awesome) like the panes so frame colours + rounded corners
    // render on this reused window; set each draw so a theme toggle updates it.
    // Classic: plain default (see drawRipConfirm for the wingui rationale).
    wbkgd(w, config_.awesome_mode ? COLOR_PAIR(CP_DIM) : COLOR_PAIR(0));
    werase(w);

    // Frame + title. Route the title through panelFrame so Awesome themes it
    // (rounded cyan frame + inset label); panelFrame draws no title in Classic,
    // so keep the manual centered one there.
    const char* title = " MUSICBRAINZ SEARCH ";
    panelFrame(w, title, true);
    if (!config_.awesome_mode)
        mvwaddstr(w, 0, (BOX_W - (int)strlen(title)) / 2, title);

    // Artist field
    mvwaddstr(w, 2, 3, "Artist :");
    {
        int fw = BOX_W - 18;
        int cur = mb_search_.artist_cursor;
        int sc  = std::max(0, cur - fw + 1);
        std::string view = mb_search_.artist_buf.size() > (size_t)sc
                         ? mb_search_.artist_buf.substr((size_t)sc, (size_t)fw) : "";
        mvwprintw(w, 2, 12, "[%-*s]", fw, view.c_str());
        if (snap_field == 0) {
            int cx = 13 + std::min(cur - sc, fw - 1);
            char cc = (cur < (int)mb_search_.artist_buf.size())
                    ? mb_search_.artist_buf[(size_t)cur] : ' ';
            wattron(w, A_REVERSE);
            mvwaddch(w, 2, cx, (unsigned char)cc);
            wattroff(w, A_REVERSE);
        }
    }

    // Album field
    mvwaddstr(w, 4, 3, "Album  :");
    {
        int fw = BOX_W - 18;
        int cur = mb_search_.album_cursor;
        int sc  = std::max(0, cur - fw + 1);
        std::string view = mb_search_.album_buf.size() > (size_t)sc
                         ? mb_search_.album_buf.substr((size_t)sc, (size_t)fw) : "";
        mvwprintw(w, 4, 12, "[%-*s]", fw, view.c_str());
        if (snap_field == 1) {
            int cx = 13 + std::min(cur - sc, fw - 1);
            char cc = (cur < (int)mb_search_.album_buf.size())
                    ? mb_search_.album_buf[(size_t)cur] : ' ';
            wattron(w, A_REVERSE);
            mvwaddch(w, 4, cx, (unsigned char)cc);
            wattroff(w, A_REVERSE);
        }
    }

    // Divider
    mvwhline(w, 6, 1, ACS_HLINE, BOX_W - 2);

    // Status line
    if (snap_searching) {
        mvwaddstr(w, 7, 3, "Searching...");
    } else if (!snap_status.empty()) {
        mvwaddnstr(w, 7, 3, snap_status.c_str(), BOX_W - 6);
    } else {
        mvwaddstr(w, 7, 3, "Tab or Enter to search");
    }

    // Results list
    if (!snap_results.empty()) {
        mvwaddstr(w, 8, 3, "Up/Down to select, Enter to load:");

        const int LIST_START = 9;
        const int LIST_ROWS  = BOX_H - LIST_START - 3;
        bool in_list = (snap_field == 2);
        int top = std::max(0, snap_list_cursor - LIST_ROWS + 1);

        for (int i = 0; i < LIST_ROWS; ++i) {
            int idx = top + i;
            if (idx >= (int)snap_results.size()) break;
            const auto& r = snap_results[(size_t)idx];
            bool hi = in_list && (idx == snap_list_cursor);

            std::string year  = r.date.size() >= 4 ? r.date.substr(0, 4) : r.date;
            std::string src   = r.from_discogs ? " [D]" : "";
            // Sanitize to ASCII first: avoids raw-multibyte mojibake under the
            // non-UTF-8 ncurses locale, and makes the byte-based substr() below
            // safe (it can no longer slice through a multibyte sequence).
            std::string art_full = sanitizeForDisplay(r.artist);
            std::string ttl_full = sanitizeForDisplay(r.title);
            std::string art_s = art_full.size() > 20 ? art_full.substr(0, 19) + ">" : art_full;
            std::string ttl_s = ttl_full.size() > 22 ? ttl_full.substr(0, 21) + ">" : ttl_full;

            char line[256];
            std::snprintf(line, sizeof(line), "%2d. %-20s  %-22s  %4s  %2s%s",
                idx + 1, art_s.c_str(), ttl_s.c_str(),
                year.c_str(), r.country.substr(0, 2).c_str(), src.c_str());

            if (hi) {
                // Selected result row — use the themeable 'selected' pair so the
                // modal matches the playlist/dir selection instead of raw reverse.
                wattron(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
                mvwprintw(w, LIST_START + i, 2, "%-*s", BOX_W - 4, line);
                wattroff(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
            } else {
                mvwaddnstr(w, LIST_START + i, 2, line, BOX_W - 4);
            }
        }
    }

    // Footer
    mvwhline(w, BOX_H - 2, 1, ACS_HLINE, BOX_W - 2);
    mvwaddstr(w, BOX_H - 1, 2, "Tab:field  Enter:search/select  Up/Down:list  Esc:close");

    wrefresh(w);
    // Don't delwin — window is cached in mb_search_win_ and reused each frame
}

// ─── handleMBSearchInput ──────────────────────────────────────────────────────

void UIManager::handleMBSearchInput(int ch) {
    auto& s = mb_search_;

    // Helper: insert char into a string at cursor position
    auto insertChar = [](std::string& buf, int& cur, char c) {
        if (buf.size() < 128) {
            buf.insert((size_t)cur, 1, c);
            ++cur;
        }
    };
    auto deleteBack = [](std::string& buf, int& cur) {
        if (cur > 0) { buf.erase((size_t)(cur - 1), 1); --cur; }
    };

    if (ch == 27) {  // Escape — close modal
        if (s.searching) mb_lookup_.cancel();
        mb_search_ = {};
        if (mb_search_win_) { delwin(mb_search_win_); mb_search_win_ = nullptr; }
        ui_overlay_ = UIOverlay::None;
        redraw_needed_.store(true);
        return;
    }
    // Pass global control keys (Ctrl+R, Ctrl+Y, Ctrl+Q, Ctrl+L etc.) through
    // to the normal handler — don't trap them in the search modal.
    if (ch < 32 && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\b'
        && ch != KEY_BACKSPACE) {
        mb_search_ = {};
        if (mb_search_win_) { delwin(mb_search_win_); mb_search_win_ = nullptr; }
        ui_overlay_ = UIOverlay::None;
        redraw_needed_.store(true);
        handleInput(ch);  // re-dispatch to normal handler
        return;
    }

    // ── Navigation when results are showing ───────────────────────────────────
    if (s.field == 2 && !s.results.empty()) {
        if (ch == KEY_UP) {
            if (s.list_cursor > 0) { --s.list_cursor; redraw_needed_.store(true); }
            return;
        }
        if (ch == KEY_DOWN) {
            if (s.list_cursor < (int)s.results.size() - 1)
                { ++s.list_cursor; redraw_needed_.store(true); }
            return;
        }
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
            // Snapshot the selected result under mb_mutex_ — the search callback
            // writes mb_search_.results under this same mutex, so we must hold
            // it while reading to avoid a data race on the vector/strings.
            bool        p_discogs = false;
            std::string p_mbid, p_title, p_artist, p_date;
            {
                std::lock_guard<std::mutex> lk(mb_mutex_);
                if (s.list_cursor < 0 || s.list_cursor >= (int)s.results.size()) return;
                p_discogs = s.results[(size_t)s.list_cursor].from_discogs;
                p_mbid    = s.results[(size_t)s.list_cursor].mbid;
                p_title   = s.results[(size_t)s.list_cursor].title;
                p_artist  = s.results[(size_t)s.list_cursor].artist;
                p_date    = s.results[(size_t)s.list_cursor].date;
            }

            // Cancel any running worker — all result data safely copied above.
            mb_lookup_.cancel();

            if (p_discogs) {
                // Discogs result: the search endpoint carries no tracklist, so
                // fetch the release detail (/releases/{id}) — mirrors the MB
                // second-fetch — and populate tracks the same way an MB pick does.
                std::string discogs_id = p_mbid;
                const std::string pfx = "discogs:";
                if (discogs_id.rfind(pfx, 0) == 0)
                    discogs_id = discogs_id.substr(pfx.size());

                s.searching  = true;
                s.status_msg = "Fetching Discogs release data...";
                redraw_needed_.store(true);

                bool started = mb_lookup_.lookupDiscogsRelease(discogs_id,
                    [this](bool ok, const MBRelease& rel, const std::string& err) {
                        std::lock_guard<std::mutex> lk(mb_mutex_);
                        mb_search_.searching = false;
                        if (!ok) {
                            mb_status_       = "Discogs fetch failed: " + err;
                            mb_status_ticks_ = 0;
                            mb_search_.status_msg = mb_status_;
                            redraw_needed_.store(true);
                            return;
                        }
                        mb_error_.clear();
                        mb_release_ = rel;
                        mb_titles_pending_.store(true);   // apply titles on UI thread
                        if (!rel.title.empty())
                            mb_album_ = (rel.artist.empty() ? "" : sanitizeForDisplay(rel.artist) + " - ")
                                      + sanitizeForDisplay(rel.title)
                                      + (rel.date.size() >= 4
                                          ? " (" + rel.date.substr(0, 4) + ")" : "");
                        mb_status_       = "Discogs: Metadata loaded - " + mb_album_;
                        mb_status_ticks_ = 0;
                        mb_search_close_pending_.store(true);
                        redraw_needed_.store(true);
                    });

                if (!started) {
                    s.searching  = false;
                    s.status_msg = "Error: lookup busy, press Enter again.";
                    redraw_needed_.store(true);
                }
                return;
            }

            // MusicBrainz result — fetch full release by MBID.
            s.searching  = true;
            s.status_msg = "Fetching full release data...";
            redraw_needed_.store(true);

            bool started = mb_lookup_.lookupByMbid(p_mbid,
                [this](bool ok, const MBRelease& rel, const std::string& err) {
                    std::lock_guard<std::mutex> lk(mb_mutex_);
                    mb_search_.searching = false;
                    if (!ok) {
                        mb_status_       = "MB fetch failed: " + err;
                        mb_status_ticks_ = 0;
                        mb_search_.status_msg = mb_status_;
                        redraw_needed_.store(true);
                        return;
                    }
                    mb_error_.clear();
                    mb_release_ = rel;
                    mb_titles_pending_.store(true);   // apply titles on UI thread
                    if (!rel.title.empty())
                        mb_album_ = sanitizeForDisplay(rel.title) + (rel.date.size() >= 4
                                   ? " (" + rel.date.substr(0, 4) + ")" : "");
                    mb_status_       = "MB: Metadata loaded - " + mb_album_;
                    mb_status_ticks_ = 0;
                    mb_search_close_pending_.store(true);
                    redraw_needed_.store(true);
                });

            if (!started) {
                s.searching  = false;
                s.status_msg = "Error: lookup busy, press Enter again.";
                redraw_needed_.store(true);
            }
            return;
        }
        // Tab or any other key in result mode — go back to input fields
        if (ch == '\t') { s.field = 0; redraw_needed_.store(true); return; }
    }

    // ── Tab: cycle fields (artist → album → trigger search → results) ─────────
    if (ch == '\t') {
        if (s.field == 0) { s.field = 1; redraw_needed_.store(true); return; }
        if (s.field == 1) {
            // Tab from album field triggers the search
            if (!s.artist_buf.empty() || !s.album_buf.empty()) {
                s.searching  = true;
                s.status_msg = "Searching...";
                s.results.clear();
                s.list_cursor = 0;
                redraw_needed_.store(true);

                std::string art = s.artist_buf, alb = s.album_buf;
                mb_lookup_.search(art, alb,
                    [this](bool ok, std::vector<MBSearchResult> results,
                           const std::string& info) {
                        std::lock_guard<std::mutex> lk(mb_mutex_);
                        mb_search_.searching = false;
                        if (!ok) {
                            mb_search_.status_msg = "No results found on MusicBrainz or Discogs.";
                            mb_status_      = mb_search_.status_msg;
                            mb_status_ticks_ = 0;
                        } else {
                            mb_search_.results    = std::move(results);
                            mb_search_.list_cursor = 0;
                            mb_search_.field      = 2;  // move focus to list
                            bool discogs = !mb_search_.results.empty()
                                         && mb_search_.results[0].from_discogs;
                            mb_search_.status_msg = discogs
                                ? "MusicBrainz: no match. Showing Discogs results:"
                                : "MusicBrainz results (" + info + "):";
                        }
                        redraw_needed_.store(true);
                    });
            }
            return;
        }
    }

    // ── Enter in input fields also triggers search ────────────────────────────
    if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && s.field < 2) {
        if (!s.artist_buf.empty() || !s.album_buf.empty()) {
            s.searching  = true;
            s.status_msg = "Searching...";
            s.results.clear();
            s.list_cursor = 0;
            redraw_needed_.store(true);

            std::string art = s.artist_buf, alb = s.album_buf;
            mb_lookup_.search(art, alb,
                [this](bool ok, std::vector<MBSearchResult> results,
                       const std::string& info) {
                    std::lock_guard<std::mutex> lk(mb_mutex_);
                    mb_search_.searching = false;
                    if (!ok) {
                        mb_search_.status_msg = "No results found on MusicBrainz or Discogs.";
                        mb_status_       = mb_search_.status_msg;
                        mb_status_ticks_ = 0;
                    } else {
                        mb_search_.results     = std::move(results);
                        mb_search_.list_cursor = 0;
                        mb_search_.field       = 2;
                        bool discogs = !mb_search_.results.empty()
                                      && mb_search_.results[0].from_discogs;
                        mb_search_.status_msg = discogs
                            ? "MusicBrainz: no match. Showing Discogs results:"
                            : "MusicBrainz results (" + info + "):";
                    }
                    redraw_needed_.store(true);
                });
        }
        return;
    }

    // ── Text editing in active input field ────────────────────────────────────
    std::string& buf = (s.field == 0) ? s.artist_buf : s.album_buf;
    int&         cur = (s.field == 0) ? s.artist_cursor : s.album_cursor;

    switch (ch) {
        case KEY_LEFT:  if (cur > 0) --cur; break;
        case KEY_RIGHT: if (cur < (int)buf.size()) ++cur; break;
        case KEY_HOME:  cur = 0; break;
        case KEY_END:   cur = (int)buf.size(); break;
        case KEY_BACKSPACE: case 127: case '\b': deleteBack(buf, cur); break;
        case KEY_DC:    // Delete key
            if (cur < (int)buf.size()) buf.erase((size_t)cur, 1);
            break;
        default:
            if (ch >= 32 && ch < 127) insertChar(buf, cur, (char)ch);
            break;
    }
    redraw_needed_.store(true);
}
