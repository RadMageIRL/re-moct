// UIManager.cpp

#ifdef _WIN32
#  ifndef NCURSES_STATIC
#    define NCURSES_STATIC
#  endif
#  include <windows.h>
#endif
// Enable the wide-character ncurses API (cchar_t / setcchar / mvwadd_wch). This
// MUST be defined before <ncurses.h> is included. Without it, this build's
// narrow string functions pass UTF-8 bytes through undecoded, so box-drawing
// glyphs (╭ ╮ ╰ ╯) render as Latin-1 mojibake. Wide chars render as one cell.
#ifndef NCURSES_WIDECHAR
#  define NCURSES_WIDECHAR 1
#endif
// Use the wide-char ncurses header for mvwaddwstr support
#ifdef __has_include
#  if __has_include(<ncursesw/ncurses.h>)
#    include <ncursesw/ncurses.h>
#  else
#    include <ncurses.h>
#  endif
#else
#  include <ncurses.h>
#endif

#include "UIManager.h"
#include "CoverArt.h"
#ifdef _WIN32
#include <shellapi.h>   // ShellExecuteA for Last.fm browser auth
#else
#include <termios.h>    // IXON off so ^Q/^S reach curses (ctor block below)
#include <unistd.h>
#endif
#include "StringUtils.h"
#include "AudioManager.h"
#include "PlaylistManager.h"
#include "Mp4Chapters.h"
#include "Config.h"
#include "LrcData.h"
#include "Toast.h"
#include "IHeartDeepLog.h"

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
#include <ctime>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "Log.h"
// Version tag shown in the status line and About panel. Per-platform suffix —
// on Windows this is the baseline string byte-identical; Linux tells the truth.
#ifdef _WIN32
static const char* const kVersionTag = "v1.0.0-rc1-win";
#else
static const char* const kVersionTag = "v1.0.0-rc1-linux";
#endif

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

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────
UIManager::UIManager(PlaylistManager& playlist, AudioManager& audio,
                     DigiConfig& config, const std::string& initial_dir,
                     core::INotify* notify)
    : notify_(notify ? notify : &core::notifier()),
      playlist_(playlist), audio_(audio), config_(config)
{
    if (!initscr()) throw std::runtime_error("initscr() failed");
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
    running_ = true;
}

// All toast call sites land here (member hides the 4-arg Toast.h adapter);
// content mapping stays in Toast.h, delivery behind the injected notifier.
void UIManager::showTrackToast(const std::string& title, const std::string& artist,
                               const std::string& album) {
    ::showTrackToast(title, artist, album, *notify_);
}

UIManager::~UIManager() {
    flushBookProgress();       // persist resume position if quitting mid-book
    running_ = false;
    lf_poll_active_.store(false);
    if (lf_poll_thread_.joinable()) lf_poll_thread_.join();
    lb_validate_active_.store(false);
    if (lb_validate_thread_.joinable()) lb_validate_thread_.join();
#ifdef _WIN32
    if (discord_art_thread_.joinable()) discord_art_thread_.join();
#endif
    if (mb_search_win_) { delwin(mb_search_win_); mb_search_win_ = nullptr; }
#ifdef _WIN32
    mb_lookup_.cancel();
    cd_ripper_.cancel();
#endif
    destroyWindows();
    endwin();
}

void UIManager::resizeWindows() {
#ifdef _WIN32
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
#else
    getmaxyx(stdscr, screen_rows_, screen_cols_);
#endif

    resize_term(screen_rows_, screen_cols_);
    destroyWindows();

    // Physically blank every cell on stdscr — critical when shrinking
    // so old content outside the new smaller bounds gets wiped
    clearok(stdscr, TRUE);
    werase(stdscr);
    for (int r = 0; r < screen_rows_; ++r)
        for (int c = 0; c < screen_cols_; ++c)
            mvaddch(r, c, ' ');
    wnoutrefresh(stdscr);
    doupdate();

    if (screen_rows_ < 9 || screen_cols_ < 40) {
        redraw_needed_.store(true);
        return;
    }

    createWindows();
    redraw_needed_.store(false);
    drawAll();
    doupdate();  // extra flush to guarantee frame hits the terminal
}

void UIManager::initColours() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();

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

void UIManager::createWindows() {
    // Awesome theme insets the two panes: a 1-col gutter on each outer edge and
    // a 1-col gap between them, giving the padded "floating panel" look. Classic
    // keeps the panes flush (gut = 0), so its geometry is byte-identical to before.
    const bool aw  = config_.awesome_mode;
    const int  gut = aw ? 1 : 0;
    const int  pane_rows  = screen_rows_ - 4;
    const int  avail_cols = screen_cols_ - (aw ? 3 : 0);   // left + mid + right gutters
    const int  left_cols  = avail_cols / 2;
    const int  right_cols = avail_cols - left_cols;
    const int  dir_x      = gut;                    // 0 classic, 1 awesome
    const int  right_x    = gut + left_cols + gut;  // == left_cols when gut == 0
    win_title_    = newwin(1,         screen_cols_, 0,              0);
    win_cwd_      = newwin(1,         screen_cols_, 1,              0);
    win_dir_      = newwin(pane_rows, left_cols,    2,              dir_x);
    win_playlist_ = newwin(pane_rows, right_cols,   2,              right_x);
    win_viz_      = newwin(pane_rows, right_cols,   2,              right_x);
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
    if (!seek_dirty_) return;
    double d = pending_seek_;
    pending_seek_ = 0.0;
    seek_dirty_   = false;
    // Drop the buffered seek if the track changed since it was requested (e.g. n/p or
    // auto-advance landed within the coalescing window) so it can't leak onto the new
    // track and start it mid-way.
    if ((int)playlist_.current() != seek_stamp_) return;
    last_seek_apply_ = std::chrono::steady_clock::now();
    audio_.seekBy(d);
    redraw_needed_.store(true);
}

void UIManager::requestSeek(double delta) {
    if (!seek_dirty_) seek_stamp_ = (int)playlist_.current();  // tie this seek to the current track
    pending_seek_ += delta;
    seek_dirty_    = true;
    // Apply immediately when outside the cooldown (so a single tap is instant);
    // otherwise accumulate and let the run loop flush once the window elapses.
    using namespace std::chrono;
    if (steady_clock::now() - last_seek_apply_ >= milliseconds(100))
        flushPendingSeek();
}

void UIManager::run() {
    last_playlist_current_for_sync_ = (int)playlist_.current();
    // Directory watch — poll every ~2s for changes
    constexpr int DIR_POLL_INTERVAL = 25;
    try { dir_mtime_ = fs::last_write_time(current_dir_); } catch (...) {}

    while (running_) {
        audio_.pollEvents();
        // Flush a coalesced seek once the user stops hammering [/] (the cooldown
        // makes single taps instant and turns held repeats into a few seeks, not many).
        if (seek_dirty_ &&
            std::chrono::steady_clock::now() - last_seek_apply_ >= std::chrono::milliseconds(100))
            flushPendingSeek();
        updateScrobbler();
        updateBookProgress();
#ifdef _WIN32
        if (audio_.takeStreamConnected()) {
            // Label from the URL actually streaming, not playlist_.current(): a
            // station launched from the override queue has no playlist row, so
            // current() is stale (the prior file) and would mislabel the toast.
            std::string t = stationLabel(audio_.streamUrl());
            showTrackToast("Streaming", t, "");
        }
        if (audio_.takeStreamFailed())
            showTrackToast("Radio stream connect FAILED", "", "");
#endif

        // Force periodic resize check and redraw every ~80ms
        static int resize_poll = 0;
        if (++resize_poll >= 1) {
            resize_poll = 0;
#ifdef _WIN32
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
#endif
            redraw_needed_.store(true);
        }

        // Force redraw when lyrics active (sync with playback position)
        if (right_pane_ == RightPane::Lyrics)
            redraw_needed_.store(true);

        // Force redraw when marquee is scrolling so title updates every tick
        {
            const auto& track = audio_.currentTrack();
            std::string np = track.artist.empty() ? track.title : track.artist + " - " + track.title;
            std::string right_approx = "  RE-MOCT v1.0.0-rc1 ";
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

#ifdef _WIN32
        // CD media check — only poll when nothing else is playing
        // Skip entirely during file playback: drive may sleep, that's fine
        if (!cd_drive_letter_.empty() && !audio_.cdMode()
            && audio_.state() == PlaybackState::Stopped) {
            ++cd_poll_ticks_;
            if (cd_poll_ticks_ >= 25) {  // 25 * 80ms = 2 seconds
                cd_poll_ticks_ = 0;
                if (!audio_.cdSource().checkMedia()) {
                    ++cd_fail_count_;
                    if (cd_fail_count_ >= 3) {
                        std::string prefix = cd_drive_letter_ + ":CD Track ";
                        playlist_.removeIf([&](const PlaylistEntry& e) {
                            return e.path.substr(0, prefix.size()) == prefix;
                        });
                        // Also remove any queued CD tracks for this drive
                        playlist_.queueRemoveIf([&](const PlaylistEntry& e) {
                            return e.path.substr(0, prefix.size()) == prefix;
                        });
                        cd_drive_letter_.clear();
                        cd_fail_count_ = 0;
                        cd_poll_ticks_ = 0;
                        pl_cursor_ = std::min(pl_cursor_, std::max(0, (int)playlist_.size() - 1));
                        pl_scroll_ = 0;
                        redraw_needed_.store(true);
                    }
                } else {
                    cd_fail_count_ = 0;
                }
            }
        }
        // When CD is playing, use the reader thread's media_removed_ flag
        if (audio_.cdMode() && audio_.cdSource().mediaRemoved()) {
            std::string prefix = cd_drive_letter_ + ":CD Track ";
            playlist_.removeIf([&](const PlaylistEntry& e) {
                return e.path.substr(0, prefix.size()) == prefix;
            });
            // Also remove any queued CD tracks — they're dangling after eject
            playlist_.queueRemoveIf([&](const PlaylistEntry& e) {
                return e.path.substr(0, prefix.size()) == prefix;
            });
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
            pl_scroll_ = 0;
            redraw_needed_.store(true);
        }
#endif
        if (playlist_.drainPending())
            redraw_needed_.store(true);
#ifdef _WIN32
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
                if (pl_cursor_ != cur) {
                    pl_cursor_ = cur;
                    int visible = paneVisibleRows(win_playlist_);
                    if (pl_cursor_ < pl_scroll_)
                        pl_scroll_ = pl_cursor_;
                    else if (pl_cursor_ >= pl_scroll_ + visible)
                        pl_scroll_ = pl_cursor_ - visible + 1;
                    redraw_needed_.store(true);
                }
            }
        }

        // ── 2. GRAB INPUT after geometry is confirmed valid ──
        int ch = getch();

        // KEY_RESIZE or Ctrl+L: force layout recalculation and redraw
        if (ch == KEY_RESIZE || ch == 12) {
            resizeWindows();
            continue;
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

        // Always recompute viz bins so bars animate smoothly
        if (right_pane_ == RightPane::Visualizer)
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
#ifdef _WIN32
            if (ui_overlay_ != UIOverlay::None) {
                if (ui_overlay_ == UIOverlay::RipConfirm) drawRipConfirm();
                else if (ui_overlay_ == UIOverlay::MBSearch) drawMBSearch();
                redraw_needed_.store(false);
            } else {
#endif
            drawAll();
            redraw_needed_.store(false);
#ifdef _WIN32
            }
#endif
        } else {
#ifdef _WIN32
            if (ui_overlay_ != UIOverlay::None) {
                // No change — modal stays on screen, do nothing
            } else {
#endif
            drawTitleBar();
            drawCwd();
            drawProgress();
            if (right_pane_ == RightPane::Visualizer) {
                drawVisualizer();
                wnoutrefresh(win_viz_);
            }
            wnoutrefresh(win_title_);
            wnoutrefresh(win_cwd_);
            wnoutrefresh(win_progress_);
            doupdate();
#ifdef _WIN32
            }
#endif
        }

        if (ch != ERR) {
            if (goto_active_)
                handleGotoInput(ch);
            else if (ui_overlay_ == UIOverlay::MBSearch)
                handleMBSearchInput(ch);
            else
                handleInput(ch);
            // Don't blindly redraw on every key while modal is open —
            // the modal redraws itself above
            if (ui_overlay_ == UIOverlay::None)
                redraw_needed_.store(true);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Visualizer DSP – DFT magnitude across log-spaced frequency bins
// ─────────────────────────────────────────────────────────────────────────────
void UIManager::computeVizBins() {
    static constexpr int N = AudioManager::VIZ_BUF_SIZE;
    static float samples[N];

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

    // Log-spaced frequency bin edges (20 Hz – 18 kHz mapped across VIZ_BINS)
    // For each bin compute DFT magnitude over that freq range
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
            // Empty range after clamping — happens for bands at/above Nyquist on
            // low-sample-rate sources. No bins map here; decay and skip so the
            // divisor below can never be zero (which produced NaN poisoning).
            viz_smoothed_[b] *= 0.5f;
            continue;
        }

        // DFT magnitude over [k_lo, k_hi)
        float mag = 0.0f;
        for (int k = k_lo; k < k_hi; ++k) {
            float re = 0.0f, im = 0.0f;
            float ang = -2.0f * 3.14159265f * k / N;
            for (int n = 0; n < N; n += 4) {   // stride 4 for speed
                re += samples[n] * std::cos(ang * n);
                im += samples[n] * std::sin(ang * n);
            }
            mag += std::sqrt(re*re + im*im);
        }
        mag /= (float)(N / 4) * (k_hi - k_lo);

        // Adaptive gain: track rolling peak and normalize against it
        // This auto-scales to the actual signal level
        static float peak_mag = 0.001f;
        if (mag > peak_mag) peak_mag = mag;
        else peak_mag *= 0.9995f;  // slow decay so gain doesn't chase noise

        float val = (peak_mag > 0.0f) ? (mag / peak_mag) : 0.0f;
        // Power curve: raises quiet parts, keeps peaks near top
        val = std::pow(val, 0.6f);
        val = std::clamp(val, 0.0f, 0.95f);

        // Exponential smoothing: very fast attack, moderate decay
        float prev = viz_smoothed_[b];
        float alpha = (val > prev) ? 0.85f : 0.25f;
        viz_smoothed_[b] = prev + alpha * (val - prev);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Drawing
// ─────────────────────────────────────────────────────────────────────────────
void UIManager::drawAll() {
    drawTitleBar();
    drawCwd();
    drawDirBrowser();
    if      (right_pane_ == RightPane::Playlist)   drawPlaylist();
    else if (right_pane_ == RightPane::Visualizer)  drawVisualizer();
    else if (right_pane_ == RightPane::TrackInfo)   drawTrackInfo();
    else if (right_pane_ == RightPane::Bookmarks)   drawBookmarks();
    else if (right_pane_ == RightPane::Lyrics)      drawLyrics();
    else if (right_pane_ == RightPane::About)       drawAbout();
    else if (right_pane_ == RightPane::Devices)     drawDevices();
    else if (right_pane_ == RightPane::EQ)          drawEq();
    else if (right_pane_ == RightPane::Queue)        drawQueue();
    else if (right_pane_ == RightPane::Chapters)     drawChapters();
    else                                             drawHelp();
    drawProgress();
    drawCmdLine();
    wnoutrefresh(win_title_);
    wnoutrefresh(win_cwd_);
    wnoutrefresh(win_dir_);
    if (right_pane_ == RightPane::Visualizer)
        wnoutrefresh(win_viz_);
    else
        wnoutrefresh(win_playlist_);
    wnoutrefresh(win_progress_);
    wnoutrefresh(win_cmdline_);
    doupdate();
}

#ifdef _WIN32
void UIManager::drawRipConfirm() {
    const int BOX_W = 68;
    const int BOX_H = 15;
    int y0 = (screen_rows_ - BOX_H) / 2;
    int x0 = (screen_cols_ - BOX_W) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;

    WINDOW* w = newwin(BOX_H, BOX_W, y0, x0);
    if (!w) return;
    werase(w);

    // Plain border + title — no wbkgd so no background color bleed
    panelFrame(w, "", true);
    const char* title = " SECURE AUDIO EXTRACTION ";
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
    // "Disc N tracks" left, "FLAC-5 + LAME V0 VBR" right-aligned on same line
    const char* fmt_str = "FLAC-5 + LAME V0 VBR";
    mvwprintw(w, 3, 3, "Disc   %d tracks", ntracks);
    mvwaddstr(w, 3, BOX_W - (int)strlen(fmt_str) - 3, fmt_str);
    if (!album_str.empty())
        mvwprintw(w, 4, 3, "%s", album_str.c_str());

    // Divider
    mvwhline(w, 5, 1, ACS_HLINE, BOX_W - 2);

    // Mode options — plain text, no color pairs on description lines
    mvwaddstr(w, 6, 3, "Select ripping mode:");
    struct { const char* key; const char* label; const char* desc; } opts[] = {
        { "[A]", "AccurateRip ", "Network CRC verify + offset correction" },
        { "[C]", "CUETools    ", "Disc-wide CRC32, no network required" },
        { "[Y]", "Local       ", "Best-effort rip, fast, no verification" },
        { "[B]", "Local 2-pass", "Best-effort + read-twice determinism check" },
        { "[N]", "Cancel      ", "Go back" },
    };
    for (int i = 0; i < 5; ++i)
        mvwprintw(w, 7 + i, 3, "%s %-12s  %s",
                  opts[i].key, opts[i].label, opts[i].desc);

    // Footer divider + output path
    mvwhline(w, 12, 1, ACS_HLINE, BOX_W - 2);
    mvwprintw(w, 13, 3, "Out  %s", disp_dir.c_str());

    wrefresh(w);
    delwin(w);
}
#endif


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
    if (current_chapters_.empty()) {               // not chaptered -> plain +/-5s seek
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
#ifdef _WIN32
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
    } else if (audio_.streamMode()) {
        // Live stream: show the station identity (its RADIO: label) so the top
        // line reflects the playing station, not the stale last-file track held
        // in currentTrack(). The song itself (ICY/HLS now-playing) stays on the
        // bottom progress row. Derive the label from the URL actually streaming
        // (stationLabel -> config name or derived), NOT playlist_.current(): a
        // station launched from the play queue never moves the playlist cursor
        // (queue items have no playlist row), so current() still points at the
        // previous song and would paint its stale title here. URL-based labeling
        // is cursor-independent and still upgrades when a user-supplied station
        // name lands in config.
        std::string label = stationLabel(audio_.streamUrl());
        np = label.empty() ? "(live stream)" : label;
    } else
#endif
    if (audio_.state() != PlaybackState::Stopped && !track.path.empty()) {
        np = track.artist.empty() ? track.title : track.artist + " - " + track.title;
        if (np.empty()) np = fs::path(track.path).filename().string();
        refreshChaptersIfNeeded(track.path);
        int ci = currentChapterIndex();
        if (ci >= 0) np += "  |  " + current_chapters_[(size_t)ci].title;
    }
    std::string badge;
#ifdef _WIN32
    if (audio_.cdMode()) {
        badge = audio_.cdSource().paused() ? "| " : (audio_.cdCurrentTrack() > 0 ? "> " : "  ");
    } else
#endif
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
#ifdef _WIN32
    if (audio_.cdMode())     modes += " [CD]";
    if (mb_fetching_.load())  modes += " [MB...]";
    else {
        std::lock_guard<std::mutex> lk(mb_mutex_);
        if (!mb_album_.empty() && audio_.cdMode())
            modes += " [" + mb_album_ + "]";
    }
#endif
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
}

void UIManager::drawCwd() {
    werase(win_cwd_);

    // Show full path, right-aligned truncation if too long
    // Format: " >> /full/path/to/current/directory "
    std::string path = in_drive_list_ ? "[Drives]" : current_dir_;

    // If path is wider than screen, show only the tail
    // Keep a leading "<<" indicator to show it's truncated
    const int maxw = screen_cols_ - 4;  // 4 = " >> " prefix
    std::string display = path;
    std::string prefix  = " >> ";
    if (dispWidth(display) > maxw) {
        display = truncateToWidthRight(display, maxw);   // column-aware tail
        prefix  = " << ";  // indicate left side is cut
    }

    std::string line = prefix + display;
    int cwx = config_.awesome_mode ? 1 : 0;   // align with inset pane frames
    std::wstring wcwd = utf8_to_wide(padToWidth(line, screen_cols_ - cwx));

    wattron(win_cwd_, COLOR_PAIR(CP_DIM));
    mvwaddnwstr(win_cwd_, 0, cwx, wcwd.c_str(), (int)wcwd.size());
    wattroff(win_cwd_, COLOR_PAIR(CP_DIM));
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
    if (in_drive_list_)   hdr = " [Drives] ";
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
        const auto& display = (idx < (int)dir_display_.size())
                            ? dir_display_[(size_t)idx] : name;
        bool cursor = (idx == dir_cursor_);
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

        const bool ico = config_.nerd_icons;
        // With icons on, reserve the prefix cell (the glyph replaces the "+"/space).
        std::string prefix = ico ? "  " : (is_dir ? "+ " : "  ");
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
            wchar_t s[2] = { is_dir ? L'\uf07b' : L'\uf001', 0 };  // folder / music
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
    std::string hdr = " Playlist [" + std::to_string(playlist_.size()) + "]";
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
    for (int i = 0; i < visible; ++i) {
        size_t idx = (size_t)(pl_scroll_ + i);
        if (idx >= playlist_.size()) break;
        const auto& e  = playlist_.at(idx);
        bool cursor  = ((int)idx == pl_cursor_);
        // The lit ("playing") row. In radio mode audio_.currentTrack() still holds
        // the last file, so a stale file row would stay lit after switching to
        // radio. Key off the URL actually streaming, NOT playlist_.current(): a
        // station launched from the override queue has no playlist row, so
        // current() is stale. A playlist-launched station's row path == streamUrl
        // and lights; a queue-launched station matches no row and lights nothing
        // (it shows on the now-playing line instead). In file mode, match the file.
        bool playing = (audio_.state() != PlaybackState::Stopped) &&
                       (audio_.streamMode()
                          ? (e.path == audio_.streamUrl())
                          : (e.path == audio_.currentTrack().path));
        short rpair = CP_DIM; attr_t rattr = A_BOLD;
        if      (cursor && focused) { rpair = CP_SELECTED;  rattr = A_BOLD; }
        else if (cursor)            { rpair = CP_SELECTED_UNFOCUSED; rattr = A_BOLD; }
        else if (playing)           { rpair = CP_STATUS_OK; rattr = A_BOLD; }
        else                        { rpair = CP_DIM;       rattr = A_BOLD; }
        wattron(win_playlist_, COLOR_PAIR(rpair) | rattr);
        const bool ico = config_.nerd_icons;
        std::string mark = (playing && !ico) ? "> " : "  ";
        std::string dur  = formatTime(e.duration_sec);
        int nw = cw - (int)mark.size() - (int)dur.size() - 3;
        std::string name = (nw > 0) ? scrollToWidth(e.display_title, nw, text_scroll_offset_) : "";
        std::string line = " " + mark + name + " " + dur + " ";
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
        if (aw) panelFrame(win_viz_, " Visualizer [v:back] ",
                           focus_ == Pane::Playlist, L'\uf001');
        return;
    }

    // Draw bars
    const int bar_area_rows = rows - 2;  // -1 header, -1 bottom border row
    const int bar_area_cols = cols - 2;  // -1 each side border
    if (bar_area_rows < 2 || bar_area_cols < 4) return;

    // Each bar is 2 cols wide + 1 col gap = 3 cols per bar slot
    // Pack as many bins as fit
    const int bar_w    = 2;   // bar width in columns
    const int bar_gap  = 1;   // gap between bars
    const int bar_slot = bar_w + bar_gap;
    const int n_bars   = std::min(VIZ_BINS, bar_area_cols / bar_slot);

    for (int b = 0; b < n_bars; ++b) {
        float val = viz_smoothed_[b * VIZ_BINS / n_bars];
        // Fractional bar height: full cells + a sub-cell remainder in eighths, for
        // smooth motion instead of whole-row jumps.
        float exact   = std::clamp(val, 0.0f, 1.0f) * bar_area_rows;
        int   full    = (int)exact;
        int   eighths = (int)((exact - full) * 8.0f + 0.5f);
        if (eighths >= 8) { ++full; eighths = 0; }
        full = std::clamp(full, 0, bar_area_rows);

        int col = 1 + b * bar_slot;

        // Colour by frequency position
        short pair;
        float pos = (float)b / n_bars;
        if      (pos < 0.33f) pair = CP_VIZ_LOW;
        else if (pos < 0.66f) pair = CP_VIZ_MID;
        else                  pair = CP_VIZ_HIGH;

        // Draw bar from bottom up — 2 cols wide
        for (int r = 0; r < bar_area_rows; ++r) {
            int screen_row = rows - 1 - r;
            if (r < full) {
                // Solid cell (space + background colour). Topmost full cell takes the
                // peak colour only when no fractional cell rides above it.
                short draw_pair = (r == full - 1 && eighths == 0) ? CP_VIZ_PEAK : pair;
                wattron(win_viz_, COLOR_PAIR(draw_pair));
                for (int w = 0; w < bar_w && col + w < cols - 1; ++w)
                    mvwaddch(win_viz_, screen_row, col + w, ' ');
                wattroff(win_viz_, COLOR_PAIR(draw_pair));
            } else if (r == full && eighths > 0) {
                // Fractional top: a lower-block glyph (▁..▇) in the tip colour on the
                // default background, giving 8× sub-cell vertical resolution.
                cchar_t cc;
                wchar_t s[2] = { (wchar_t)(0x2580 + eighths), 0 };  // U+2581..U+2587
                setcchar(&cc, s, A_NORMAL, CP_VIZ_TIP, nullptr);
                for (int w = 0; w < bar_w && col + w < cols - 1; ++w)
                    mvwadd_wch(win_viz_, screen_row, col + w, &cc);
            }
        }
    }

    if (aw) {
        panelFrame(win_viz_, " Visualizer [v:back] ", focus_ == Pane::Playlist, L'\uf001');
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
        { "Left arrow",     "Go to parent directory"              },
        { "g",              "Goto directory  (Tab = complete)"    },
        { "Playlist",       "",                             true  },
        { "a",              "Add selection to playlist (recursive)"},
        { "d",              "Delete selected track"               },
        { "u  /  D",        "Move track up / down"                },
        { "c",              "Clear entire playlist"               },
        { "S  (Shift+S)",   "Save playlist as M3U file"           },
        { "l",              "Load / append M3U playlist file"     },
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
        { "?",              "Toggle this help  (j/k to scroll)"  },
        { "q",              "Add track to play queue"             },
        { "Q  (Shift+Q)",   "Show / hide queue pane"              },
        { "Ctrl+R",         "Fetch CD metadata from MusicBrainz" },
        { "Ctrl+Y",         "Rip CD  (A=AccurateRip  C=CUETools  Y=Local  B=Local 2-pass)" },
        { "Ctrl+D",         "Toggle Discord Rich Presence" },
        { "Ctrl+T",         "Toggle Classic / Awesome theme" },
        { "~",              "Reload theme.conf colours (live)" },
        { "Ctrl+N",         "Toggle Nerd Font icons (needs Nerd Font)" },
        { "Ctrl+A",         "Toggle deep-analysis iHeart log (diagnostic)" },
        { "Ctrl+K",         "Stream mode: Web Player (fewer ads) / Raw broadcast" },
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
void UIManager::saveTagEdits() {
    if (tag_edit_path_.empty()) return;
    try {
#ifdef _WIN32
        auto wp = utf8_to_wide(tag_edit_path_);
        TagLib::FileRef ref(wp.c_str(), false, TagLib::AudioProperties::Fast);
#else
        TagLib::FileRef ref(tag_edit_path_.c_str(), false, TagLib::AudioProperties::Fast);
#endif
        if (ref.isNull() || !ref.tag()) return;
        auto* tag = ref.tag();
        tag->setTitle (TagLib::String(tag_edit_values_[0], TagLib::String::UTF8));
        tag->setArtist(TagLib::String(tag_edit_values_[1], TagLib::String::UTF8));
        tag->setAlbum (TagLib::String(tag_edit_values_[2], TagLib::String::UTF8));
        tag->setGenre (TagLib::String(tag_edit_values_[3], TagLib::String::UTF8));
        if (!tag_edit_values_[4].empty()) {
            try { tag->setYear((unsigned int)std::stoi(tag_edit_values_[4])); }
            catch (...) {}
        }
        ref.save();
        info_cached_path_.clear();  // invalidate so drawTrackInfo re-reads on next open

        // Update playlist display title so it reflects immediately
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
    } catch (...) {}
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
        { "Version v1.0.0-rc1-win  |  C++20  |  ncurses  |  miniaudio  |  TagLib", false },
#else
        { "Version v1.0.0-rc1-linux  |  C++20  |  ncurses  |  miniaudio  |  TagLib", false },
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

void UIManager::drawTrackInfo() {
    WINDOW* w = win_playlist_;
    werase(w);
    int rows, cols;
    getmaxyx(w, rows, cols);

    // Which track to show: highlighted in playlist if focused there, else current
    std::size_t idx = (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size())
                    ? (std::size_t)pl_cursor_
                    : playlist_.current();

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

        // Render label: value pairs
        // Editable fields: Title(0) Artist(1) Album(2) Genre(3) Year(4)
        static const char* editable[] = {"Title","Artist","Album","Genre","Year"};
        const int label_w = 13;
        int row = 2;
        for (const auto& f : fields) {
            if (row >= rows - 1) break;

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
            mvwaddnstr(w, row, 0, lbl.c_str(), label_w + 2);
            wattroff(w, is_selected ? (COLOR_PAIR(CP_SELECTED)|A_BOLD)
                                    : (COLOR_PAIR(CP_TITLE)|A_BOLD));

            // Value
            int val_w = cols - label_w - 3;
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
                    mvwaddnwstr(w, row, label_w + 2, wdisp.c_str(), (int)wdisp.size());
                    // Blink cursor position
                    int cur_col = std::min(dispWidth(val), val_w - 1);
                    mvwaddch(w, row, label_w + 2 + cur_col, '_');
                    wattroff(w, COLOR_PAIR(CP_SELECTED) | A_BOLD);
                } else {
                    wattron(w, edit_idx >= 0 && !tag_edit_mode_
                              ? (COLOR_PAIR(CP_DIM)|A_BOLD) : COLOR_PAIR(CP_DIM));
                    std::wstring wval = utf8_to_wide(scrollToWidth(val, val_w, text_scroll_offset_));
                    mvwaddnwstr(w, row, label_w + 2, wval.c_str(), (int)wval.size());
                    wattroff(w, edit_idx >= 0 && !tag_edit_mode_
                               ? (COLOR_PAIR(CP_DIM)|A_BOLD) : COLOR_PAIR(CP_DIM));
                }
            }
            ++row;
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
#ifdef _WIN32
    if (audio_.cdMode()) {
        pos = (double)audio_.cdPositionSec();
        dur = (double)audio_.cdDurationSec();
    }
#endif
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
#ifdef _WIN32
    if (audio_.cdMode()) {
        meta = "1411 kbps  44.1 kHz  stereo  CD";
        // Live BPM is detected from the playback stream just like file mode; show
        // it here too (the CD meta string above is otherwise fixed Red Book values).
        if (audio_.currentBpm() > 0)
            meta += "  " + std::to_string(audio_.currentBpm()) + " bpm";
        else if (audio_.state() == PlaybackState::Playing)
            meta += "  bpm:...";
    } else {
#endif
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
#ifdef _WIN32
    }
#endif
    {
        int vp = (int)(audio_.volume() * 100.0f + 0.5f);
        if (!meta.empty()) meta += "  ";
        meta += "vol:" + std::to_string(vp) + "%";
    }
    int bw = cols - (int)ts.size() - (int)meta.size() - 6;
    if (bw < 4) bw = 4;
    int filled = (dur > 0) ? (int)((pos/dur)*bw) : 0;
    filled = std::clamp(filled, 0, bw);
#ifdef _WIN32
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
        // now-playing title, with a [LIVE]/[BUFFERING] marker and volume.
        std::string title = audio_.streamNowPlaying();
        std::string right = audio_.streamBuffering() ? "[BUFFERING]" : "[LIVE]";
        right += "  vol:" + std::to_string((int)(audio_.volume()*100.0f+0.5f)) + "%";
        std::string left = title.empty() ? "(live stream)" : title;
        int avail = cols - (int)right.size() - 3;
        if (avail < 1) avail = 1;
        if ((int)left.size() > avail) left = left.substr(0, (size_t)avail);
        wattron(win_progress_, COLOR_PAIR(CP_TITLE));
        mvwaddstr(win_progress_, 0, 1, left.c_str());
        mvwaddstr(win_progress_, 0, cols - (int)right.size() - 1, right.c_str());
        wattroff(win_progress_, COLOR_PAIR(CP_TITLE));
        return;
    }
#endif
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
#ifdef _WIN32
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
#endif

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
// De-invert "Surname, The" -> "The Surname" for cleaner scrobble metadata.
// Strict: only fires when the entire tail after the comma is a bare article, so
// real comma-containing names ("Tyler, The Creator", "Earth, Wind & Fire") are
// left untouched. Verified against an isolated test of both forms.
static std::string deinvertArtist(const std::string& a) {
    auto pos = a.rfind(',');
    if (pos == std::string::npos || pos == 0) return a;
    std::string head = a.substr(0, pos);
    std::string tail = a.substr(pos + 1);
    size_t ts = tail.find_first_not_of(" \t");
    if (ts == std::string::npos) return a;             // nothing after the comma
    tail = tail.substr(ts);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) head.pop_back();
    if (head.empty()) return a;
    std::string low = tail;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (low == "the" || low == "a" || low == "an")
        return tail + " " + head;                      // preserve article casing
    return a;
}

static bool looksLikeRealTrack(const std::string& artist, const std::string& track) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string a = lower(artist), t = lower(track);
    if (a.empty() || t.empty()) return false;
    if (a == "unknown" || t == "unknown" ||
        a == "unknown artist" || a == "unknown_artist") return false;
    if (t == "live") return false;   // "<station> - LIVE" floor (exact match; spares songs like "Live and Let Die")
    static const char* junk[] = {
        "ad|", "ad |", "commercial break", "commercial-break",
        "advertisement", "station id", "station-id", "spot block", "spotblock"
    };
    for (const char* j : junk)
        if (a.find(j) != std::string::npos || t.find(j) != std::string::npos)
            return false;
    return true;
}

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

#ifdef _WIN32
// Spawn a one-shot worker to resolve an album-cover URL (iTunes/Deezer) off the
// UI thread. Single in-flight at a time; the result is picked up by the deferred
// art-commit in updateScrobbler. No-op if a lookup is already running.
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
#endif

void UIManager::updateScrobbler() {
#ifdef _WIN32
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
#ifdef _WIN32
        && !config_.discord_presence
#endif
        )
        return;   // nothing to drive (no scrobbler, no Discord) -> no-op

    auto st = audio_.state();
    if (st != PlaybackState::Playing && st != PlaybackState::Paused) {
        scrob_artist_.clear(); scrob_track_.clear();   // stopped -> reset
#ifdef _WIN32
        if (discord_active_) { discord_.clearActivity(); discord_active_ = false; }
        discord_artist_.clear(); discord_track_.clear();
#endif
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
#ifdef _WIN32
    // Discord Rich Presence — same resolved metadata, independent of scrobbling.
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
                else if (discord_art_neg_.find(key) == discord_art_neg_.end())
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
                    // the miss so the same song returning in rotation keeps the logo
                    // without re-hitting iTunes/Deezer. Logo is already on screen.
                    discord_art_neg_.insert(forkey);
                }
            } else if (!audio_.streamMode() && !discord_album_.empty()) {
                // The finished lookup was for a since-skipped track — retry for the
                // one we actually landed on (unless already cached). Covers file+cd.
                if (!(curkey == discord_art_cache_key_ && !discord_art_cache_url_.empty()))
                    startDiscordArtLookup(discord_artist_, discord_album_, curkey);
            }
        }
    }
#endif

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
#endif
}

void UIManager::startLastfmPoll(const std::string& token) {
#ifdef _WIN32
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
#endif
}

void UIManager::startListenBrainzValidate(const std::string& token) {
#ifdef _WIN32
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
#endif
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
#ifdef _WIN32
    if (ui_overlay_ == UIOverlay::MBSearch) { handleMBSearchInput(ch); return; }
    // ── Rip mode selection modal — intercepts all input when active ─────────
    if (ui_overlay_ == UIOverlay::RipConfirm) {
        RipMode chosen = RipMode::None;
        switch (ch) {
            case 'a': case 'A': chosen = RipMode::AccurateRip; break;
            case 'c': case 'C': chosen = RipMode::CUETools;    break;
            case 'y': case 'Y': chosen = RipMode::Local;       break;
            case 'b': case 'B': chosen = RipMode::LocalVerify; break;
            case 'n': case 'N': case 27:
                ui_overlay_ = UIOverlay::None;
                redraw_needed_.store(true);
                return;
            default: return;  // ignore everything else
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
        cd_ripper_.start(audio_, tracks, out_dir, rel, chosen,
            [this](const RipProgress& p) {
                rip_status_ = p.status_msg;
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            });
        return;
    }
#endif

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
            case 'e': case 'E': case 27:
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
            case 'f': case 'F':
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

    // v toggles visualizer (not from help)
    if (ch == 'v' || ch == 'V') {
        if (right_pane_ == RightPane::Visualizer)
            right_pane_ = RightPane::Playlist;
        else
            right_pane_ = RightPane::Visualizer;
        return;
    }

    // When help is showing, j/k scroll it; other keys still work globally
    if (right_pane_ == RightPane::Help) {
        switch (ch) {
            case 17: audio_.stop(); running_ = false; return;
            case 'j': case 'J': case KEY_DOWN: ++help_scroll_; redraw_needed_.store(true); return;
            case 'k': case 'K': case KEY_UP:   --help_scroll_; redraw_needed_.store(true); return;
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
                    saveTagEdits();
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
            case 'e': case 'E': {
                // Enter edit mode — only for real files, not CD tracks, not currently playing
                std::size_t idx = (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size())
                                ? (std::size_t)pl_cursor_ : playlist_.current();
                if (idx < playlist_.size()) {
                    const std::string& path = playlist_.at(idx).path;
                    if (isCDTrackPath(path) || path.empty()) {
                        return;  // CD track — silently ignore
                    }
                    if (path == audio_.currentTrack().path &&
                        audio_.state() != PlaybackState::Stopped) {
                        // Currently playing — warn in cmdline bar
                        werase(win_cmdline_);
                        wattron(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
                        mvwaddnstr(win_cmdline_, 0, 1,
                            "Stop playback first to edit tags  (s = stop)", screen_cols_ - 2);
                        wattroff(win_cmdline_, COLOR_PAIR(CP_STATUS_OK) | A_BOLD);
                        wrefresh(win_cmdline_);
                        return;
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
#ifdef _WIN32
        case 1:  // Ctrl+A — toggle deep-analysis iHeart capture log (diagnostic; not persisted)
        {
            bool on = IHeartDeepLog::toggle();
            if (on) showTrackToast("Deep log: ON", IHeartDeepLog::path(), "");
            else    showTrackToast("Deep log: OFF", "", "");
        }
            break;
#endif
#ifdef _WIN32
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
#endif
        case 20:  // Ctrl+T — toggle Classic / Awesome theme
            config_.awesome_mode = !config_.awesome_mode;
            config_.save();
            resizeWindows();        // rebuild panes with theme-aware geometry +
            redraw_needed_.store(true);   // full shrink-safe screen wipe
#ifdef _WIN32
            showTrackToast(config_.awesome_mode ? "Theme: Awesome"
                                               : "Theme: Classic", "", "");
#endif
            break;
        case 14:  // Ctrl+N — toggle Nerd Font title icons (needs a Nerd Font)
            config_.nerd_icons = !config_.nerd_icons;
            config_.save();
            redraw_needed_.store(true);
#ifdef _WIN32
            showTrackToast(config_.nerd_icons ? "Nerd icons: ON (needs Nerd Font)"
                                              : "Nerd icons: OFF", "", "");
#endif
            break;
        // Per-case Windows gates below (slice-2 fix): a single #ifdef here
        // used to span ^D/^B/^F/^G/^U/^Y/^R, silently deleting the portable
        // scrobbler-login and radio-URL keys from the Linux build (Dos-found:
        // ^U/^G/^B dead while ^N/^T/^L/^Q worked — a key probe proved the tty
        // delivers all of them; the gap was here). Gate each key on WHY:
        // ^D = Discord IPC (slice 4); ^F = the MBSearch overlay's draw/input
        // handlers are still Windows-gated; ^Y/^R = CD (slice 6, and the
        // RipConfirm modal handler is Windows-gated).
#ifdef _WIN32
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
#endif
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
            if (config_.lastfm_key.empty() || config_.lastfm_secret.empty()) {
                openInputBar(InputMode::LastfmKey, "");   // first-time: prompt in-app
            } else if (config_.lastfm_pending.empty()) {
                sclog("ctrl+G: begin auth");
                lastfmBeginAuth();
            } else {
                std::string sk, user;
                bool ok = LastFm::getSession(config_.lastfm_key, config_.lastfm_secret,
                                             config_.lastfm_pending, sk, user);
                sclog("ctrl+G: getSession ok=%d user=%s", (int)ok, user.c_str());
                if (ok) {
                    config_.lastfm_session = sk;
                    config_.lastfm_user    = user;
                    config_.lastfm_pending.clear();
                    config_.save();
                    lf_poll_active_.store(false);   // stop any background poll
                    showTrackToast("Last.fm: logged in as " + user, "", "");
                } else {
                    showTrackToast("Last.fm: auth failed - press Ctrl+G to retry", "", "");
                }
            }
            break;
        case 21:  // Ctrl+U — input an internet-radio stream URL
            if (ui_overlay_ == UIOverlay::None)
                openInputBar(InputMode::StreamURL, "");
            break;
#ifdef _WIN32
        case 25:  // Ctrl+Y — CD rip
            if (ui_overlay_ != UIOverlay::None) break;  // already showing modal
            if (audio_.cdMode() && !cd_ripper_.isActive()) {
                const auto& cd = audio_.cdSource();
                if (!cd.tracks().empty()) {
                    ui_overlay_ = UIOverlay::RipConfirm;
                    redraw_needed_.store(true);
                }
            } else if (audio_.cdMode() && cd_ripper_.isActive()) {
                // Already ripping — Ctrl+Y cancels
                cd_ripper_.cancel();
                rip_status_    = "Rip cancelled by user.";
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            } else {
                rip_status_    = "Ctrl+Y: Insert a CD and load it from [Drives] first.";
                rip_msg_ticks_ = 0;
                redraw_needed_.store(true);
            }
            break;
        case 18:  // Ctrl+R — MusicBrainz CD lookup
            if (audio_.cdMode() && !mb_fetching_.load()) {
                if (mb_lookup_.isActive()) break;  // already in progress
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
            } else if (!audio_.cdMode()) {
                // Not in CD mode — silently ignore
            }
            break;
#endif
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
        case 'e': case 'E':
            if (right_pane_ == RightPane::EQ)
                right_pane_ = RightPane::Playlist;
            else
                right_pane_ = RightPane::EQ;
            break;
        case '{':
            if (!audio_.cdMode()) { audio_.adjustSpeed(-0.02f); } break;
        case '}':
            if (!audio_.cdMode()) { audio_.adjustSpeed(+0.02f); } break;
        case 'f': case 'F':
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
        case 'n': case 'N': {
            auto play_next = [&](const std::string& path, bool from_queue = false) {
                if (!from_queue) pl_cursor_ = (int)playlist_.current();
#ifdef _WIN32
                std::string drive; int track_num;
                if (parseCDPath(path, drive, track_num)) {
                    if (!audio_.cdMode()) audio_.openCD(drive);
                    audio_.playCDTrack(track_num);
                    return;
                }
#endif
                if (audio_.cdMode()) audio_.closeCD();
                audio_.play(path);
                maybePreloadNext();
            };
            // Queue takes priority over playlist on manual next
            if (!playlist_.queueEmpty()) {
                if (auto qe = playlist_.queuePop(); qe.has_value()) {
                    audio_.clearNext();
                    const std::string& qpath = qe->path;
#ifdef _WIN32
                    std::string drive; int track_num;
                    if (parseCDPath(qpath, drive, track_num)) {
                        if (!audio_.cdMode()) audio_.openCD(drive);
                        audio_.playCDTrack(track_num);
                        // pl_cursor_ stays where it is — queue item has no playlist row
                        break;
                    }
#endif
                    if (audio_.cdMode()) audio_.closeCD();
                    play_next(qpath, true);  // from_queue=true — don't move cursor
                }
            } else if (auto p = playlist_.next(); p.has_value())
                play_next(p.value());
            else
                audio_.stop();
            break;
        }
        case 'p': case 'P': {
            auto play_prev = [&](const std::string& path) {
                pl_cursor_ = (int)playlist_.current();
#ifdef _WIN32
                std::string drive; int track_num;
                if (parseCDPath(path, drive, track_num)) {
                    if (!audio_.cdMode()) audio_.openCD(drive);
                    audio_.playCDTrack(track_num);
                    return;
                }
#endif
                if (audio_.cdMode()) audio_.closeCD();
                audio_.play(path);
            };
            if (auto p = playlist_.previous(); p.has_value())
                play_prev(p.value());
            break;
        }
        case '[': requestSeek(-5.0); break;
        case ']': requestSeek(+5.0); break;
        case ',': jumpChapter(-1); break;
        case '.': jumpChapter(+1); break;
        case ';':
            // Toggle the chapter list (only opens if the file has chapters)
            if (right_pane_ == RightPane::Chapters) {
                right_pane_ = RightPane::Playlist;
            } else if (!current_chapters_.empty()) {
                chapter_cursor_ = std::max(0, currentChapterIndex());
                right_pane_ = RightPane::Chapters;
            } else {
                showTrackToast("No chapters in this track", "", "");
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
        case 'u': case 'U':
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                playlist_.moveUp((std::size_t)pl_cursor_);
                if (pl_cursor_ > 0) {
                    --pl_cursor_;
                    if (pl_cursor_ < pl_scroll_) --pl_scroll_;
                }
                // Prevent cursor sync from snapping us away
                last_playlist_current_for_sync_ = (int)playlist_.current();
            }
            break;
        case 'D':
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                playlist_.moveDown((std::size_t)pl_cursor_);
                if (pl_cursor_ + 1 < (int)playlist_.size()) {
                    ++pl_cursor_;
                    if (pl_cursor_ >= pl_scroll_ + paneVisibleRows(win_playlist_)) ++pl_scroll_;
                }
                last_playlist_current_for_sync_ = (int)playlist_.current();
            }
            break;
        case 'd':
            if (focus_ == Pane::Playlist && pl_cursor_ < (int)playlist_.size()) {
                bool was_playing = ((size_t)pl_cursor_ == playlist_.current()
                                    && audio_.state() != PlaybackState::Stopped);
                playlist_.removeAt((size_t)pl_cursor_);
                if (was_playing) audio_.stop();
                // Keep cursor in bounds
                if (pl_cursor_ >= (int)playlist_.size() && pl_cursor_ > 0)
                    --pl_cursor_;
                // Keep scroll in bounds so cursor doesn't appear to jump
                int visible = win_playlist_ ? paneVisibleRows(win_playlist_) : 0;
                int max_scroll = std::max(0, (int)playlist_.size() - visible);
                if (pl_scroll_ > max_scroll) pl_scroll_ = max_scroll;
                if (pl_scroll_ > pl_cursor_) pl_scroll_ = pl_cursor_;
                // Deleting an entry above the playing track shifts current()'s
                // index; resync the marker so the follow logic doesn't read it as
                // a track change and snap the cursor to the playing row.
                last_playlist_current_for_sync_ = (int)playlist_.current();
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
        case KEY_DOWN: case 'j': case 'J': navigateDown(); break;
        case KEY_UP:   case 'k': case 'K': navigateUp();   break;
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
                        pl_cursor_ = (int)before;
                        pl_scroll_ = (int)before;
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
                        if (pl_scroll_ >= (int)after) pl_scroll_ = 0;
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
                    pl_cursor_ = (int)before;
                    pl_scroll_ = (int)before;
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
                    if (pl_scroll_ >= (int)after) pl_scroll_ = 0;
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
                    pl_cursor_  = (int)idx;
                    pl_scroll_  = 0;
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
        int v = paneVisibleRows(win_playlist_);
        if (pl_cursor_+1 < (int)playlist_.size()) {
            ++pl_cursor_;
            if (pl_cursor_ >= pl_scroll_+v) ++pl_scroll_;
        }
    }
}

void UIManager::navigateUp() {
    if (focus_ == Pane::DirBrowser) {
        if (dir_cursor_ > 0) { --dir_cursor_; if (dir_cursor_ < dir_scroll_) dir_scroll_ = dir_cursor_; }
    } else {
        if (pl_cursor_ > 0) { --pl_cursor_; if (pl_cursor_ < pl_scroll_) pl_scroll_ = pl_cursor_; }
    }
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
            pl_cursor_ = (int)idx; pl_scroll_ = 0;
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
#ifdef _WIN32
                std::string drive; int track_num;
                if (parseCDPath(path, drive, track_num)) {
                    if (!audio_.cdMode()) audio_.openCD(drive);
                    audio_.playCDTrack(track_num);
                    return;
                }
#endif
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
std::vector<std::string> UIManager::listDrives() {
    std::vector<std::string> drives;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) {
            std::string d; d += (char)('A'+i); d += ":\\";
            drives.push_back(d);
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
    werase(w);

    // Plain border + title (rounded in Awesome mode)
    panelFrame(w, "", true);
    const char* title = " MUSICBRAINZ SEARCH ";
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
