#pragma once
#ifdef _WIN32
#  ifndef NCURSES_STATIC
#    define NCURSES_STATIC
#  endif
#endif
#include <ncurses.h>
#include <string>
#include <vector>
#include <array>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#ifdef _WIN32
#include "MBLookup.h"
#include "CDRipper.h"
#include "RadioBrowser.h"
#include "LastFm.h"
#include "ListenBrainz.h"
#include "DiscordRP.h"
#endif
#include "AudioManager.h"
#include "miniaudio.h"
#include "LrcData.h"
#include "Mp4Chapters.h"

class PlaylistManager;
struct DigiConfig;

enum class Pane      { DirBrowser, Playlist };
enum class RightPane { Playlist, Visualizer, Help, TrackInfo, Bookmarks, Lyrics, About, Devices, EQ, Queue, Chapters };

// Modal overlays drawn on top of the normal layout
enum class UIOverlay { None, RipConfirm, MBSearch };

class UIManager {
public:
    UIManager(PlaylistManager& playlist, AudioManager& audio,
              DigiConfig& config,
              const std::string& initial_dir = "");
    ~UIManager();

    void run();
    void requestRedraw() { redraw_needed_.store(true); }
    const std::string& currentDir() const { return current_dir_; }

private:
    WINDOW* win_title_    = nullptr;
    WINDOW* win_cwd_      = nullptr;
    WINDOW* win_dir_      = nullptr;
    WINDOW* win_playlist_ = nullptr;
    WINDOW* win_viz_      = nullptr;
    WINDOW* win_progress_ = nullptr;
    WINDOW* win_cmdline_  = nullptr;

    void createWindows();
    void destroyWindows();
    // Theme-aware panel border. Classic: identical to box(w,0,0). Awesome: rounded
    // corners + accent colour, with an optional title embedded in the top edge.
    void panelFrame(WINDOW* w, const std::string& title = "", bool focused = true,
                    wchar_t icon = 0);
    // Rows of list content actually painted inside a pane. Classic spends one row
    // on the header bar; Awesome spends a top + bottom border row. Draw and scroll
    // math must share this, or the last entry hides under the frame.
    int  paneVisibleRows(WINDOW* w) const;

    void drawAll();
    void drawTitleBar();
    void drawCwd();
    void drawDirBrowser();
    void drawPlaylist();
    void drawVisualizer();
    void drawHelp();
    void drawTrackInfo();
    void drawBookmarks();
    void drawChapters();
    void drawLyrics();
    void drawAbout();
    void drawDevices();
    void drawEq();
    void drawQueue();
    void drawFavs();
    void saveTagEdits();
    void maybePreloadNext();
    void drawRipConfirm();
    void drawMBSearch();
    void handleMBSearchInput(int ch);
    void drawProgress();
    void drawCmdLine();
    void drawGotoBar();   // inline path input at cmdline row

    void handleInput(int ch);
    void handleGotoInput(int ch);   // input handler when goto bar is active
    void toggleFocus();
    void navigateDown();
    void navigateUp();
    void activateSelection();

    Pane      focus_           = Pane::DirBrowser;
    RightPane right_pane_      = RightPane::Playlist;
    RightPane prev_right_pane_ = RightPane::Playlist;
    bool running_       = false;
    std::atomic<bool> redraw_needed_ { true };
    int  help_scroll_   = 0;   // scroll position in help pane
    void resizeWindows();

    // Time display mode
    bool show_remaining_ = false;
    bool show_clock_     = false;
    bool show_hidden_    = false;
    int  sleep_minutes_  = 0;   // 0 = disabled
    std::chrono::steady_clock::time_point sleep_start_;  // show hidden (dot) files in browser   // show wall clock in title bar

    // Dir browser
    enum class BrowserSort { Name, Modified, Size };
    BrowserSort  browser_sort_ = BrowserSort::Name;
    std::string              current_dir_;
    std::vector<std::string> dir_entries_;
    std::vector<std::string> dir_display_;
    int dir_scroll_ = 0;
    int dir_cursor_ = 0;

    // Playlist
    int pl_scroll_ = 0;
    int pl_cursor_ = 0;
    int last_playlist_current_for_sync_ = 0;  // for move-up/down cursor fix

    // Screen dims
    int screen_rows_ = 0;
    int screen_cols_ = 0;

    // Visualizer
    static constexpr int VIZ_BINS = 28;
    std::array<float, VIZ_BINS> viz_bars_     {};
    std::array<float, VIZ_BINS> viz_smoothed_ {};
    void computeVizBins();

    // Input bar state (goto dir / save M3U / load M3U)
    enum class InputMode { Goto, SaveM3U, LoadM3U, StreamURL, StreamName, RadioSearch, LastfmKey, LastfmSecret, ListenBrainzToken };
    bool        goto_active_  = false;
    InputMode   input_mode_   = InputMode::Goto;
    std::string goto_input_;
    std::string pending_stream_url_;   // URL stashed between the Ctrl+U URL prompt and its name prompt
    int         goto_cursor_  = 0;
    // Tab-completion state
    std::vector<std::string> tab_matches_;
    int                      tab_idx_   = -1;
    std::string              tab_prefix_;

    void gotoOpen();
    void openInputBar(InputMode mode, const std::string& initial = "");
    // Name-aware station label: "RADIO: <stored name>" if Config has one for this
    // URL, else the URL-derived label. Single source of truth for [Radio]/playlist
    // station labels so persisted names show everywhere.
    std::string stationLabel(const std::string& url) const;
    void gotoClose(bool commit);
    void gotoTabComplete();
    std::vector<std::string> gotoGetMatches(const std::string& partial) const;

    PlaylistManager& playlist_;
    AudioManager&    audio_;
    DigiConfig&      config_;

    // Lyrics
    LrcData     lrc_;
    std::string lrc_loaded_path_;  // track path the LRC was loaded for

    // Bookmark popup state
    int bookmark_cursor_ = 0;
    int chapter_cursor_  = 0;

    // Device picker state
    struct DeviceEntry { std::string name; ma_device_id id; };
    std::vector<DeviceEntry> device_list_;
    int device_cursor_ = 0;

    // EQ state
    int         eq_cursor_       = 0;
    std::string eq_preset_name_  = "";
    int         q_cursor_        = 0;

    // Tag edit state (info pane)
    bool        tag_edit_mode_   = false;
    int         tag_edit_field_  = 0;
    std::string tag_edit_values_[5];
    std::string tag_edit_path_;

    // Track info cache — avoids TagLib re-read on every drawTrackInfo call
    std::string info_cached_path_;
    TrackInfo   info_cached_track_;

    // CD state
    std::string cd_drive_letter_;
    int         cd_poll_ticks_   = 0;
    int         cd_fail_count_   = 0;
#ifdef _WIN32
    MBLookup    mb_lookup_;
    std::atomic<bool> mb_fetching_ { false };
    std::string mb_error_;    // protected by mb_mutex_
    std::string mb_album_;    // protected by mb_mutex_
    MBRelease   mb_release_;  // cached full release for ripping, protected by mb_mutex_
    std::mutex  mb_mutex_;
    // Set true by a worker callback once mb_release_ is cached; the run loop then
    // applies the track titles to playlist_ on the UI thread. playlist_ must only
    // ever be touched by the UI thread (its entries_ vector is unsynchronised),
    // so worker callbacks never call playlist_ methods directly.
    std::atomic<bool> mb_titles_pending_ { false };
    void applyReleaseTitles(const MBRelease& rel);   // UI thread only

    // ── MusicBrainz manual search modal (Ctrl+F) ──────────────────────────────
    struct MBSearchState {
        int  field        = 0;   // 0=artist 1=album 2=results
        std::string artist_buf;
        std::string album_buf;
        int  artist_cursor = 0;
        int  album_cursor  = 0;
        std::vector<MBSearchResult> results;
        int  list_cursor   = 0;
        bool searching     = false;
        std::string status_msg;
    } mb_search_;
    std::string       mb_status_;
    int               mb_status_ticks_ = 0;
    static constexpr int MB_STATUS_LIFETIME = 62;
    std::atomic<bool> mb_search_close_pending_ { false };
    WINDOW*           mb_search_win_ = nullptr;  // cached modal window

    CDRipper    cd_ripper_;
    UIOverlay   ui_overlay_    = UIOverlay::None;
    std::string rip_status_;   // shown in cmdline during/after rip
    int         rip_msg_ticks_ = 0;  // auto-clear counter
#endif  // consecutive media check failures  // name of active preset, empty if custom

    // Recently played virtual dir state
    bool in_recent_      = false;
    bool in_favs_        = false;
    bool in_radio_       = false;
    bool in_radio_search_ = false;       // showing radio-browser results in [Radio]
    bool in_books_       = false;        // showing the [Books] audiobook list
    // Chapter table for the currently playing book (empty if none / not a book).
    std::vector<Mp4Chapter> current_chapters_;
    std::string             chapters_for_path_;   // path current_chapters_ reflects
    void refreshChaptersIfNeeded(const std::string& path);
    int  currentChapterIndex() const;             // index into current_chapters_, -1 if none
    void jumpChapter(int dir);                     // -1 prev (position-aware), +1 next; ±5s fallback
    std::string lastfm_pending_token_;   // (legacy; token now persisted in config)
    void lastfmBeginAuth();              // request token + open browser

    // Last.fm auth auto-poll: worker polls getSession; UI thread commits the result.
    std::atomic<bool> lf_poll_active_{false};
    std::atomic<bool> lf_poll_done_{false};
    std::mutex        lf_poll_mtx_;
    std::string       lf_poll_session_;
    std::string       lf_poll_user_;
    std::thread       lf_poll_thread_;
    void startLastfmPoll(const std::string& token);

    // ListenBrainz token validation: worker validates off the UI thread; the UI
    // thread commits the result (store token+user, save, toast) on the next tick.
    std::atomic<bool> lb_validate_active_{false};
    std::atomic<bool> lb_validate_done_{false};
    std::mutex        lb_validate_mtx_;
    std::string       lb_validate_token_;       // token that was validated (guarded)
    std::string       lb_validate_user_;        // username on success (guarded)
    bool              lb_validate_ok_ = false;  // result (guarded)
    std::thread       lb_validate_thread_;
    void startListenBrainzValidate(const std::string& token);

    // Last.fm scrobble state machine
    std::string scrob_artist_, scrob_track_, scrob_album_;
    std::string scrob_normid_;           // canonical identity of the committed track (relabel dedup)
    std::string discord_radio_art_;      // last iHeart digital cover URL pushed to Discord ("" = logo)
#ifdef _WIN32
    // Discord Rich Presence (Ctrl+D). Mirrors the scrobbler's track-change moment.
    DiscordRP   discord_{"1519141025195491338"};   // RE-MOCT application id
    bool        discord_active_       = false;      // an activity is currently set
    bool        discord_force_update_ = false;      // push current track on next tick
    std::string discord_artist_, discord_track_;    // last activity sent (change gate)
    std::string discord_album_;                     // for rebuilding state on art commit
    long        discord_start_ = 0;
    // Async album-art URL resolution (files: iTunes/Deezer lookup, off the UI thread).
    std::thread       discord_art_thread_;
    std::atomic<bool> discord_art_active_{false};   // worker in flight
    std::atomic<bool> discord_art_done_{false};     // result ready for pickup
    std::mutex        discord_art_mtx_;
    std::string       discord_art_url_;             // resolved url   (guarded)
    std::string       discord_art_key_;            // "artist\ttrack" the result is for (guarded)
    std::string       discord_art_cache_key_;       // last resolved key (UI thread only)
    std::string       discord_art_cache_url_;       // last resolved url (UI thread only)
    void startDiscordArtLookup(const std::string& artist,
                               const std::string& album,
                               const std::string& key);
#endif
    long        scrob_start_ = 0;        // unix time the current track started
    bool        scrob_done_  = false;    // already scrobbled this track
    void        updateScrobbler();       // called each tick; fires now-playing + scrobble
    std::vector<RadioStation> radio_results_;
    int  fav_cursor_     = 0;

    void        refreshDir();
    std::string formatTime(double seconds) const;
    std::string scrolledText(const std::string& text, int width) const;
    // Directory watch
    std::filesystem::file_time_type dir_mtime_ {};
    int dir_poll_ticks_ = 0;
    // Marquee scroll state for long track names
    int         marquee_offset_  = 0;
    int         marquee_ticks_   = 0;
    std::string marquee_last_path_;  // detect track change to reset offset
    static constexpr int MARQUEE_PAUSE  = 20;
    static constexpr int MARQUEE_SPEED  = 3;
    int  text_scroll_offset_ = 0;   // shared offset for all truncated text rows
    int  text_scroll_ticks_  = 0;

    // Drive browser
    bool in_drive_list_ = false;
    static std::vector<std::string> listDrives();
    void enterDriveList();
    void activateDrive(const std::string& drive_entry);

    static constexpr short CP_TITLE      = 1;
    static constexpr short CP_FOCUSED    = 2;
    static constexpr short CP_SELECTED   = 3;
    static constexpr short CP_PROGRESS   = 4;
    static constexpr short CP_STATUS_OK  = 5;
    static constexpr short CP_STATUS_ERR = 6;
    static constexpr short CP_BORDER     = 7;
    static constexpr short CP_DIM        = 8;
    static constexpr short CP_VIZ_LOW    = 9;
    static constexpr short CP_VIZ_MID    = 10;
    static constexpr short CP_VIZ_HIGH   = 11;
    static constexpr short CP_VIZ_PEAK   = 12;

    void initColours();
};
