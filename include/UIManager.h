#pragma once
#include "CursesSeam.h"
#include <algorithm>
#include <string>
#include <vector>
#include <array>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <optional>
#include "MBLookup.h"
#include "CDRipper.h"
#include "RadioBrowser.h"
#include "PodcastFeed.h"    // podcasts slice 2: PodcastEpisode/PodcastFeed for the [Podcasts] section
#include "PodcastClient.h"  // podcasts slice 2: the async feed fetch result type
#include "PodcastIndex.h"   // podcasts slice 6: Podcast Index byterm search result types
#include <cstdint>          // podcasts slice 3: uint64_t/int32_t download-progress + cancel state
#include <deque>            // podcasts slice 4: the download queue
#include "LastFm.h"
#include "ListenBrainz.h"
#include "DiscordRP.h"
#include "AudioManager.h"
#include "VizFFT.h"
#include "miniaudio.h"
#include "LrcData.h"
#include "Mp4Chapters.h"
#include "AwesomeThemes.h"
#include "CoverArtRender.h"
#include "ArtMissCache.h"   // time-bounded art negative cache (radio-art-refresh-fix)
#include "GainScan.h"       // batch ReplayGain over a folder (batch-r128)
#include "ConvertJob.h"     // convert-core: decode -> IEncoder batch convert engine
#include "MarkSet.h"        // convert-core: the browser's path-keyed marked set
#include "core/INotify.h"
#include "core/IMediaControl.h"   // OS media control seam (osmedia-seam)
#include "MediaRouter.h"          // OS-command marshal + split sink
#include "SeekCoalescer.h"        // shared seek lanes (TUI [/] + OS SetPosition/Seek)

class PlaylistManager;
struct DigiConfig;

enum class Pane      { DirBrowser, Playlist };
enum class RightPane { Playlist, Visualizer, Help, TrackInfo, Bookmarks, Lyrics, About, Devices, EQ, Queue, Chapters, SearchResults };
enum class SearchSource { Playlist, Browser };   // which list \-search targets (from focus_)

// Modal overlays drawn on top of the normal layout
enum class UIOverlay { None, RipConfirm, MBSearch, RecPanel, ConvertScope, ConvertConfirm, PlaylistFormat, PodcastPlayConflict, PodcastIndexCreds };

class UIManager {
public:
    // notify == nullptr -> the production platform notifier (core::notifier());
    // tests inject a fake here (constructor injection — no setNotify global).
    // media == nullptr -> the production platform impl (core::mediaControl());
    // tests inject a fake here (constructor injection, the notify_ pattern).
    UIManager(PlaylistManager& playlist, AudioManager& audio,
              DigiConfig& config,
              const std::string& initial_dir = "",
              core::INotify* notify = nullptr,
              core::IMediaControl* media = nullptr);
    ~UIManager();

    void run();
    void requestRedraw() { redraw_needed_.store(true); }
    // Podcast episode finished (slice 3): if the ended track is the playing episode,
    // mark it played and stop, returning true so the auto-advance callback in main
    // does NOT advance into the music queue. Public: called from that callback.
    bool onEpisodeTrackEnd();
    // wingui only: repaint live during Windows' modal resize-drag loop (the app's
    // getch() loop is blocked there, so without this the window is blank mid-drag).
    // Registered as PDCurses' window-resized callback; a no-op elsewhere.
    void onWinguiLiveResize();
    // wingui only: repaint tick pumped from a timer inside Windows' modal
    // move/size loop, so the marquee/spectrum keep animating during a title-bar
    // move (WM_MOVE never fires the resize callback). Renders one tickFrame().
    void onWinguiPaintTick();
    const std::string& currentDir() const { return current_dir_; }
#ifdef PDCURSES
    // wingui only (Alt+Enter): toggle a borderless window that fills the monitor,
    // and back to the previous framed size. The curses grid reflows via the resize
    // callback, exactly like a user drag.
    void toggleWinguiFullscreen();
#endif

private:
    // Notifications seam (slice 7): injected fake in tests, core::notifier() in prod.
    core::INotify* notify_;
    // OS media control seam (osmedia-seam): SMTC / MPRIS, injected fake in tests,
    // core::mediaControl() in prod. Bidirectional - outbound now-playing +
    // inbound transport via media_router_ (marshalled to the UI-loop drain).
    core::IMediaControl* media_;
    MediaRouter         media_router_;
    void wireMediaControl();           // ctor: register sinks + command handler
    // updateScrobbler tail: publish now-playing to the OS on a real change
    // (the resolved Discord-feed values, reused - no second assembler).
    void publishMedia(const std::string& artist, const std::string& track,
                      const std::string& album, const std::string& art_url,
                      const std::vector<uint8_t>& art_bytes, int pos, int dur);
    // Local-file embedded cover, extracted once per file (path-keyed) so the
    // per-tick OS-media publish never re-reads the tag (osmedia-art-floor).
    std::string          media_art_path_;
    std::vector<uint8_t> media_art_cache_;
    core::MediaStatus   mediaStatus() const;
    // Last-published OS-media identity (independent of the Discord toggle so the
    // two features are orthogonal).
    std::string media_last_artist_, media_last_track_, media_last_art_;
    bool        media_last_valid_ = false;
    void        manualNext();         // the case 'n' body, one home (OS Next routes here too)
    void        manualPrevious();     // the case 'p' body, one home
    // Same name/arity as the pre-slice-7 free function, so every toast call site
    // in UIManager.cpp compiles unchanged (class scope hides the 4-arg adapter in
    // Toast.h); forwards to that adapter with the injected notifier. Defined in
    // UIManager.cpp.
    void showTrackToast(const std::string& title, const std::string& artist,
                        const std::string& album);

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
    // popup-artifact-repaint: on overlay dismiss, mark stdscr (the gutter/insets layer
    // a popup physically overwrites but whose ncurses buffer is unchanged) and every
    // pane dirty, so the following drawAll() re-pushes exactly the clobbered cells in
    // z-order without a clearok full-screen flash.
    void repaintAfterOverlay();
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
    bool saveTagEdits();   // true only if the file was actually written; UI updates gated on it
    // Queue-aware preload (XF C3): per-tick convergence of the armed next
    // decoder onto resolveNextPath (NextArm.h). Replaces the old per-call-site
    // maybePreloadNext arming. Runs right after audio_.pollEvents() in run() -
    // pollEvents fires the callbacks that advance the playlist index, so the
    // resolver must see the post-advance state. Guards keep arming scoped to
    // active LOCAL FILE playback (never CD/stream/podcast/stopped).
    void reconcileNextArm();
    void drawRipConfirm();
    void drawMBSearch();
    void drawRecPanel();   // stream-record R2: the [Rec] panel (^E)
    void drawConvertScope();    // convert-core: pick scope (this file/folder/marked/pane/playlist)
    void drawPlaylistFormat();  // Shift+S on a playlist file: reformat to m3u8/pls/xspf, or Save as
    void drawConvertConfirm();  // convert-core: pick output format + quality
    // One frame of the main loop's post-input body (marquee/viz/breath advance +
    // the draw block), extracted so run() and the wingui modal-move paint tick
    // render identically. See run() / onWinguiPaintTick.
    void tickFrame();
    // viz-live-under-overlay: stage the animated background panes (title/marquee,
    // cwd, progress, and the spectrum when shown) without flushing; the caller
    // flushes (doupdate, or an overlay draw fn's wrefresh compositing on top).
    void drawAnimatedPanes();
    // Dispatch the current modal overlay to its draw fn (each wrefreshes itself).
    void drawOverlay();
    // Absolute path of browser entry idx, respecting the browser mode (favs/
    // recent entries are already absolute; normal-dir entries join current_dir_).
    // "" for pseudo-entries ("..", "[Drives]", ...) and drive/radio modes.
    std::string browserEntryPath(int idx) const;
    // rec-cover-art: the radio-art machinery's identity/pickup/trigger halves,
    // extracted from refreshRadioArt so the recording wiring can drive the
    // SAME fetch path (same guard, caches, providers) with the pane closed.
    std::string radioSongIdentity(const std::string& np,
                                  std::string& artist, std::string& title);
    void radioArtPickup(const std::string& song_key);
    void radioArtKick(const std::string& artist, const std::string& title,
                      const std::string& song_key);
    void radioArtFloor(const std::string& song_key);   // radio-art-refresh-fix: the
                                                       // empty-key reset BOTH drivers share
    void handleMBSearchInput(int ch);
    void drawProgress();
    void drawCmdLine();
    void drawGotoBar();   // inline path input at cmdline row

    void handleInput(int ch);
    void handleGotoInput(int ch);   // input handler when goto bar is active
    void toggleFocus();
    void navigateDown();
    void navigateUp();
    // Page / Home / End for the focused list pane (playlist or browser).
    // Cursor-movement only: playlist scroll = the slice-5 draw-time invariant;
    // the browser has NO invariant, so these clamp dir_scroll_ themselves.
    void navigatePage(int dir);          // +1 = PgDn, -1 = PgUp; page = visible-1
    void navigateHomeEnd(bool to_end);   // false = Home (row 0), true = End (last)
    void activateSelection();

    Pane      focus_           = Pane::DirBrowser;
    RightPane right_pane_      = RightPane::Playlist;
    RightPane prev_right_pane_ = RightPane::Playlist;
    // Awesome mode: a permanent full-width Spectrum strip below the panes,
    // toggled with 'v' (default on). Classic mode ignores this and keeps its
    // RightPane::Visualizer overlay instead. The strip is only actually created
    // (win_viz_ non-null in Awesome) when this is on AND the terminal is tall
    // enough; vizStripShown() is the live "is the strip on screen" test.
    bool awesome_viz_strip_ = true;
    bool vizStripShown() const;   // Awesome + strip actually on screen (win_viz_ != null)
    bool running_       = false;
    std::atomic<bool> redraw_needed_ { true };
    int  help_scroll_   = 0;   // scroll position in help pane
    void resizeWindows();

    // Time display mode
    bool show_remaining_ = false;
    bool show_clock_     = false;
    bool show_hidden_    = false;
    // iHeart deep-analysis capture toggle (Ctrl+A). Host-tracked since slice c:
    // IHeartDeepLog moved into the streaming plugin, so the state is pushed across
    // the ABI (audio_.setDeepLog) rather than read back from the plugin. Diagnostic,
    // not persisted.
    bool deeplog_on_     = false;
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
    // Queue-aware preload (XF C3), UI-thread-only mirrors for NextArm.h's
    // reconcile: the last path handed to preloadNext, and the failure latch.
    // Never read back from AudioManager - next_path_ is audio-thread-mutated.
    std::string armed_next_;
    std::string failed_next_;
    // Last nowPlayingRow() seen by the run-loop follow-sync (-1 = none). The
    // F3 cursor snap triggers on a CHANGE in this, not in playlist_.current():
    // starting a stream never moves current(), so song->radio / CD->radio were
    // invisible to a current()-gated snap. -1 resets when nothing is playing.
    int last_now_playing_row_ = -1;

    // The playlist row currently PLAYING, stream-aware - unlike playlist_.current(),
    // which is a raw index and goes stale in stream mode (audio_.currentTrack() still
    // holds the last file; a queue-launched station has no row at all). Returns
    // nullopt when nothing plays, when stopped, or when the playing stream matches no
    // row (queue-launched station - it shows on the now-playing line, lights no row).
    // This is the canonical "which row is lit" test; drawPlaylist() and the info/tag
    // panes route through it. It deliberately does NOT replace current(); current()
    // keeps its index-identity meaning for seek-stamp and auto-advance. See lessons.md.
    std::optional<std::size_t> nowPlayingRow() const;

    // The TrackInfo to display for the playing item, or nullptr in stream mode (a
    // stream has no TrackInfo; callers show stream metadata separately). File/CD only.
    const TrackInfo* nowPlayingTrack() const;

    // The single owner of pl_scroll_'s relationship to pl_cursor_: clamps the cursor
    // into range, then scrolls the minimum needed to bring it inside the visible
    // window. Enforced once at the top of drawPlaylist(); every path that moves the
    // cursor just lets the next draw reveal it. Idempotent. See lessons.md - do NOT
    // reintroduce per-handler "if cursor < scroll then nudge" math.
    void ensurePlaylistCursorVisible();
    void ensureDirCursorVisible();   // browser twin: clamp dir_scroll_ to keep dir_cursor_ shown
    std::string browserSectionLabel() const;   // display-only: names the current browser list
                                               // (dir leaf / feed title / section) for messages

    // Screen dims
    int screen_rows_ = 0;
    int screen_cols_ = 0;

    // Visualizer
    static constexpr int VIZ_BINS = 64;   // finer spectrum: fills a wide strip with
                                          // thin bars (bin cost is integration, not DFT)
    std::array<float, VIZ_BINS> viz_bars_     {};
    std::array<float, VIZ_BINS> viz_smoothed_ {};
    // Slice C: real radix-2 FFT replaces the stride-4 DFT that aliased
    // everything above ~sr/8 (~5.5 kHz) - the top ~10 bars were noise-derived.
    // Members, not locals: tables/scratch precomputed once, magnitude() is
    // allocation-free (per-frame hot path). Proven by tests/viz_fft_test.cpp.
    VizFFT<AudioManager::VIZ_BUF_SIZE> viz_fft_;
    std::array<float, AudioManager::VIZ_BUF_SIZE / 2> viz_fft_mag_ {};
    // Coupled per-band peak normalization (viz-normalize A+B). MEMBERS with slow
    // decay persisting across calls - the bug this replaced was a loop-scope
    // `static float peak_mag` acting as one global AGC shared by all 64 bands,
    // so every band normalized against the bass peak and the top pinned low.
    std::array<float, VIZ_BINS> viz_peak_ {};   // per-band rolling peak
    float viz_global_peak_ = 0.001f;            // global rolling peak (coupling floor)
    void computeVizBins();

    // Input bar state (goto dir / save M3U / load M3U)
    enum class InputMode { Goto, SaveM3U, LoadM3U, StreamURL, StreamName, RadioSearch, LastfmKey, LastfmSecret, ListenBrainzToken, PlaylistSearch, RecDir, RecOffset, PodcastAddUrl, PodcastIndexSearch, PodcastIndexKey, PodcastIndexSecret };
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

    // Whether a playlist row's tags can be edited, and why not if not. One source of
    // truth shared by the 'e' entry guard and saveTagEdits's reasoning about locking.
    enum class TagEditability { Editable, PlayingLocked, NotAFile, Empty };
    TagEditability tagEditability(const std::string& path) const;

    // Track info cache — avoids TagLib re-read on every drawTrackInfo call
    std::string info_cached_path_;
    TrackInfo   info_cached_track_;

    // Cover art in the Info pane (half-block cells). Rendered + colour-allocated
    // once per (track, art-box size), cached and blitted each frame. See
    // refreshInfoArt / drawArt in UIManager.cpp.
    std::string        info_art_key_;    // "<path>|<cols>x<rows>" the COMMITTED art reflects
    cover::Rendered    info_art_;        // decoded half-block grid (info_art_.ok gates drawing)
    // The curses pair table is global. Only one art image's colours can occupy
    // slots kArtColourBase.. / pairs kArtPairBase.. at a time, so exactly one
    // variable may hold those indices - and it must record whose they are. Shared
    // by info (file) and radio art: whichever the Info pane is drawing owns it,
    // reallocated at the blit when the key changes (see drawTrackInfo).
    std::string        art_pairs_key_;   // render key whose colours currently occupy the table
    std::vector<short> art_pairs_;       // pair index per cell, parallel to the owning grid
    void refreshInfoArt(const std::string& path, int box_cols, int box_rows);
    bool allocArtColorPairs(const cover::Rendered& art, std::vector<short>& out_pairs);

    // Async local-file cover decode: the file read (TagLib) + stb decode run on a
    // worker so drawTrackInfo NEVER blocks the UI thread (a synchronous decode
    // stalled input on Linux, where the draw loop repaints frequently - a quick
    // 'e'/Esc around a stall felt dropped). Colour-pair allocation stays on the UI
    // thread (curses palette). A small MRU cache of recent (path|box) decodes keeps
    // tag-edit toggles and track re-visits instant (no re-decode, no re-read).
    std::thread          info_art_thread_;
    std::atomic<bool>    info_art_active_{false};   // decode worker in flight
    std::atomic<bool>    info_art_done_{false};     // decoded grid ready for pickup
    std::mutex           info_art_mtx_;
    std::string          info_art_want_key_;        // "path|box" the worker decodes for (guarded)
    cover::Rendered      info_art_result_;          // decoded grid from the worker (guarded)
    struct ArtCacheEntry { std::string key; cover::Rendered art; };
    std::vector<ArtCacheEntry> info_art_cache_;     // small MRU cache (FIFO-evicted)
    static constexpr std::size_t kInfoArtCacheMax = 6;
    void startInfoArtDecode(const std::string& path, const std::string& key,
                            int box_cols, int box_rows);
    void commitInfoArt(const std::string& key, const cover::Rendered& r);   // UI thread: alloc + show
    const cover::Rendered* artCacheGet(const std::string& key);
    void artCachePut(const std::string& key, const cover::Rendered& r);

    // ── Radio cover art (Info pane) ───────────────────────────────────────────
    // Live-stream art shadows the Discord committed-song decision: on a committed
    // radio track-change, resolve a cover URL (the station-supplied iHeart cover
    // if any, else a urlBySong lookup) off the UI thread, GET the bytes, and
    // half-block render them into the SAME Info-pane art box as local files. No
    // confident match => the bundled RE-MOCT logo floor (its own cache key so it
    // never thrashes against real covers). Best-effort/decorative: a slow fetch
    // shows the logo now and fills the cover when it lands (never stalls the UI).
    std::thread          radio_art_thread_;
    std::atomic<bool>    radio_art_active_{false};   // fetch worker in flight
    std::atomic<bool>    radio_art_done_{false};     // bytes ready for pickup
    std::mutex           radio_art_mtx_;
    std::string          radio_art_want_key_;        // "artist\ttitle" the worker fetches for (guarded)
    std::vector<uint8_t> radio_art_result_;          // downloaded image bytes (guarded; {} => no match)
    bool                 radio_fetch_station_ = false; // worker used the station URL (skip neg-cache on miss)
    // Bytes cache (one committed song): avoids re-GET on rotation-return and on box resize.
    std::string          radio_bytes_key_;           // song key the bytes belong to ("" = none/floor)
    std::vector<uint8_t> radio_bytes_;               // cached cover bytes ({} + resolved => logo floor)
    bool                 radio_bytes_resolved_ = false; // fetch finished for radio_bytes_key_
    std::string          radio_last_station_art_;    // station art URL a resolution was based on
    // Decoded-block cache (keyed on song + box + logo-vs-cover), parallel to info_art_.
    std::string          radio_render_key_;
    cover::Rendered      radio_art_;      // pair indices live in the shared art_pairs_ (see above)
    // Bundled RE-MOCT logo, decoded once and reused as the floor (own cache key).
    std::vector<uint8_t> logo_bytes_;                // loaded lazily from remoct_logo.jpg
    bool                 logo_load_tried_ = false;
    void refreshRadioArt(int box_cols, int box_rows);              // UI thread: resolve/fetch/decode
    void startRadioArtFetch(const std::string& artist, const std::string& title,
                            const std::string& station_art);       // spawn the off-UI fetch worker
    const std::vector<uint8_t>& logoBytes();                       // lazy-load the bundled logo bytes

    // CD state
    std::string cd_drive_letter_;
    int         cd_poll_ticks_   = 0;
    int         cd_fail_count_   = 0;

    // Focus-aware search (\): pick-to-jump, deliberately NOT a filter. The results
    // overlay lists matches; picking one moves the ACTIVE pane's cursor to the row's
    // REAL index and closes - the list is never narrowed or reordered, so there is
    // no display-index/real-index duality (the proxy-vs-direct bug class the three
    // cursor bugs came from). Matching is a case-insensitive substring over the row's
    // display text. search_source_ (set from focus_ at \-time) selects the source:
    //   Playlist -> playlist_ / pl_cursor_ / jumpToPlaylistIndex
    //   Browser  -> dir_display_ / dir_cursor_ / jumpToBrowserIndex (all sub-modes)
    // search_labels_ stores each match's display string as it was at search time; the
    // pick UI compares it to the live row on draw and on Enter (validate-on-use), so a
    // list rebuilt underneath the overlay (dir poll, async feed load, anything) closes
    // it rather than ever jumping to a stale row. Immune to which/how-many rebuild
    // sites exist - no invalidate-on-change wiring needed.
    std::vector<std::size_t> search_results_;   // indices into the active source
    std::vector<std::string> search_labels_;    // matched display text, parallel to results
    SearchSource search_source_ = SearchSource::Playlist;
    int         search_cursor_ = 0;             // cursor within search_results_
    std::string search_query_;                  // shown in the results header
    void drawSearchResults();
    void jumpToPlaylistIndex(std::size_t idx);
    void jumpToBrowserIndex(std::size_t idx);   // browser twin: keeps focus on DirBrowser

    // Slice 3: lazily reopen the CD handle for an action (play / rip / MB) on a
    // stopped-but-loaded disc — stop() closes the handle (probe B2: holding it
    // open idle spins the drive up audibly), so any action must reopen first.
    // Routes through audio_.openCD(), which re-reads the TOC + re-inits the
    // device. openCD can fail TRANSIENTLY right after file playback (WASAPI
    // uninit->init settling), so failure is handled non-destructively: bounded
    // retry (3 attempts, ~200 ms apart), each failure logged with its failure
    // point (CDSource::lastOpenFail). Purge + clear cd_drive_letter_ + toast
    // ONLY on a confirmed empty tray (drive answered: TocRead/NoAudioTracks) -
    // that is the eject-while-stopped detection point now that the poll purge
    // is gone. A DeviceOpen (busy) failure keeps the disc state and just toasts
    // "drive busy" - a stale row self-heals on the next action; unloading a
    // present disc does not. Returns true iff the handle is open & ready;
    // callers bail on false.
    bool reopenCDForAction(const std::string& drive);
    // Remove all playlist + queue rows for a CD drive letter (shared by the
    // reopen-eject path and the reader-thread eject path).
    void purgeCDRows(const std::string& drive);

    // Shift+E in [Drives]: eject the highlighted CD drive. Stops + fully unloads
    // first when it's the loaded disc (slice-3 teardown, incl. the media-removal
    // unlock on close); a drive RE-MOCT never loaded gets a bare seam handle just
    // to send the eject. Refuses while a rip is active on the loaded disc.
    void ejectDrive(const std::string& drive_entry);

    // Drive entries (as listed in dir_entries_, e.g. "F:\" / "/dev/sr0") that are
    // CD drives with media PRESENT AT ENUMERATION TIME. Computed once per
    // enterDriveList() (entry / F12) - never polled (probe B2: repeated media
    // checks spin an idle drive up audibly). Insert a disc while sitting in
    // [Drives] and the eject hint appears only after F12 - by design.
    std::vector<std::string> cd_drives_with_media_;
    bool driveHasMedia(const std::string& entry) const {
        return std::find(cd_drives_with_media_.begin(),
                         cd_drives_with_media_.end(), entry)
               != cd_drives_with_media_.end();
    }
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
    UIOverlay   overlay_drawn_ = UIOverlay::None;   // last overlay actually rendered; the
                                                    // None-transition triggers repaintAfterOverlay()
    // Session rip-format selection (rip-format-select): seeded ONCE from
    // config_.rip_formats in the ctor, toggled by the modal's digit keys,
    // NEVER written back to config (the conf key is the default only).
    // Deliberately not reset on eject — user preference, not disc data.
    std::vector<RipFormat> rip_sel_;
    // encoder-bitrate-mode: the `>` row-focus cursor for the per-row quality
    // editor (Up/Down move it, Left/Right cycle the focused row's axis, [M]
    // flips its mode). Session state, seeded to 0; the rip modal focuses over
    // kRipFormats, the [Rec] panel over its 3 rows (Opus/Mp3/Copy).
    int rip_focus_ = 0;
    int rec_focus_ = 0;
    // convert-core: the marked-file set (path-keyed, survives re-sort/refresh/
    // dir-change), the batch convert engine, and the convert overlay state.
    MarkSet    marked_;
    ConvertJob convert_job_;
    int        convert_scope_ = 0;             // 1 file, 2 folder, 3 marked, 4 pane, 5 playlist file
    std::string convert_pl_file_;              // [5]: the focused playlist file whose entries transcode
    // Shift+S reformat popup (PlaylistFormat overlay): the focused playlist file to
    // re-serialize into another container, and the format-row cursor.
    std::string plexp_src_;                    // focused playlist file under the S popup
    int         plexp_focus_ = 0;              // format focus: 0 m3u8 / 1 pls / 2 xspf
    std::string convert_src_dir_;              // folder scope: the dir to enumerate
    std::string convert_single_;               // file scope: the one source path
    RipFormat  convert_fmt_ = RipFormat::Flac; // single-select output format
    int        convert_focus_ = 0;             // ConvertConfirm row cursor
    // Stream-record R2: the [Rec] panel's SETTINGS (session state seeded from
    // config, like rip_sel_ - config is load-once, the panel never writes
    // back). Lifecycle state (recording/elapsed/cuts/dropped) is deliberately
    // NOT mirrored here - every render reads the recorder's own atomic
    // accessors, so panel/badge and engine can never drift.
    RipFormat   rec_fmt_      = RipFormat::Opus;   // single-select: Opus | Mp3
    bool        rec_copy_     = false;             // 3rd radio: Copy (as broadcast,
                                                   // no re-encode); wins over rec_fmt_
    bool        rec_split_on_ = true;              // split on title change
    std::string rec_dir_;                          // "" = recordingsDir() at start
    int         rec_offset_ms_ = 1200;             // split-trim hold (session, [T])
    bool        rec_ads_discard_ = false;          // ad-aware: [A] Save/Discard
    int         rec_panel_tick_ = 0;               // ~2 Hz live-state refresh
    std::string rec_art_pushed_key_;               // rec-cover-art: dedupe onArt pushes
    // batch-r128: the folder ReplayGain scan (worker-threaded engine) + the
    // ^O tag/force/cancel prompt state (the prompt text lives in rip_status_,
    // the app's de-facto status line; no new draw plumbing).
    GainScan    gain_scan_;
    bool        rgscan_prompt_ = false;
    std::string rgscan_dir_;
    // [REC] badge pulse: phase advanced ~every 0.64s by the tick loop while
    // recording; drawTitleBar overpaints the badge in red with the phase attr.
    int         rec_pulse_ = 0, rec_pulse_tick_ = 0;
    std::string rip_status_;   // shown in cmdline during/after rip
    int         rip_msg_ticks_ = 0;  // auto-clear counter

    // [THEME:name] tag on the cwd line, shown on a theme switch (Ctrl+T / F8):
    // bold, then dimmed near the end, gone after ~10s. Counts ~80ms loop ticks
    // (timeout(80)). Initialised past the timeout so it is hidden at startup.
    int         theme_tag_ticks_ = 125;

    // Cmdline status line (both platforms). On Linux it is also the toast echo
    // (KEPT past slice 5): a real notify-send toast renders on a desktop with a
    // notification daemon, but headless/no-daemon Linux (WSL2, CI, SSH) shows
    // nothing — so every showTrackToast message ALSO surfaces here for a few
    // seconds (otherwise toast-only flows like ^B "logged in as" / ^G "approve
    // in browser" look dead). Same shape as rip_status_/rip_msg_ticks_.
    // The iHeart mode confirms (Ctrl+K feed / F6 re-pin) set it DIRECTLY on both
    // platforms — they are in-place mode state, not a notification, so they never
    // go through the toast path — and draw in yellow (CP_MODE) via the flag below;
    // every other setter resets the flag so the next message goes back to the
    // normal status colour.
    std::string status_msg_;
    int         status_msg_ticks_ = 0;
    bool        status_msg_yellow_ = false;

    // Transient warning on the cmdline bar (both platforms), e.g. "stop playback
    // first to edit tags". Rendered red in drawCmdLine() and expired after ~5s in
    // the main loop - a persistent replacement for a one-shot paint the next
    // repaint would clobber before it could be read. Not a toast: a warning is an
    // in-place status, not a notification.
    std::string warn_msg_;
    int         warn_msg_ticks_ = 0;

    // Recently played virtual dir state
    bool in_recent_      = false;
    bool in_favs_        = false;
    bool in_radio_       = false;
    bool in_radio_search_ = false;       // showing radio-browser results in [Radio]
    bool in_books_       = false;        // showing the [Books] audiobook list
    // [Podcasts] two-level state, mirroring [Radio] + its in_radio_search_ sub-mode:
    //   in_podcasts_ && !in_podcast_feed_  -> level 1, subscribed feed list
    //   in_podcasts_ &&  in_podcast_feed_  -> level 2, one feed's episode list
    bool in_podcasts_    = false;
    bool in_podcast_feed_ = false;
    bool in_podcastindex_search_ = false;  // slice 6: showing Podcast Index results at level 1
    std::string podcast_feed_url_;       // the feed whose episodes are shown at level 2
    // Chapter table for the currently playing book (empty if none / not a book).
    std::vector<Mp4Chapter> current_chapters_;
    std::string             chapters_for_path_;   // path current_chapters_ reflects
    // `sidecar`, when given, is a cached <podcast:chapters> JSON document to fall
    // back on if the file itself yields nothing. ONLY the ; handler's episode
    // branch passes one - it alone knows which episode a path belongs to - so
    // books and plain files reach the identical one-argument behaviour and never
    // gain so much as an extra filesystem probe.
    void refreshChaptersIfNeeded(const std::string& path,
                                 const std::string& sidecar = std::string());
    int  currentChapterIndex() const;             // index into current_chapters_, -1 if none
    void jumpChapter(int dir);                     // -1 prev (position-aware), +1 next; ±5s fallback
    // Audiobook resume (Phase C): latch the active book's live position and
    // persist it on stop / track-switch / exit; seek back to it on (re)start.
    std::string book_progress_path_;
    double      book_progress_pos_   = 0.0;
    double      book_progress_dur_   = 0.0;
    bool        book_resume_pending_ = false;
    void updateBookProgress();
    void flushBookProgress();
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
    ArtMissCache      discord_art_neg_;             // radio keys whose art lookup MISSED —
                                                    // time-bounded (radio-art-refresh-fix Fix #2:
                                                    // a transient failure self-heals after the
                                                    // TTL; a genuine no-art song stays
                                                    // rate-limited). UI thread only. Stream-only;
                                                    // shared by the pane and Discord paths.
    void startDiscordArtLookup(const std::string& artist,
                               const std::string& album,
                               const std::string& key,
                               bool song = false);
    long        scrob_start_ = 0;        // unix time the current track started
    bool        scrob_done_  = false;    // already scrobbled this track
    void        updateScrobbler();       // called each tick; fires now-playing + scrobble
    std::vector<RadioStation> radio_results_;
    // [Podcasts] level-2 backing store: the episodes of the entered feed. The
    // visible rows in dir_display_ are pre-formatted from these (title, date,
    // duration), mirroring how radio_results_ backs the radio search list.
    std::vector<PodcastEpisode> podcast_episodes_;

    // Podcast feed fetch worker - mirrors the info_art / radio_art async idiom so
    // subscribing to or entering a feed never freezes the TUI (feeds run large,
    // up to ~22 MB). Single in-flight; a finished fetch is installed per-frame by
    // pollPodcastFetch(). podcast_fetch_want_url_ is the staleness guard: a result
    // for a feed the user has since navigated away from is discarded.
    enum class PodcastFetchPurpose { Subscribe, Enter };
    std::thread           podcast_fetch_thread_;
    std::atomic<bool>     podcast_fetch_active_{false};  // worker in flight
    std::atomic<bool>     podcast_fetch_done_{false};    // result ready for pickup
    std::mutex            podcast_fetch_mtx_;
    std::string           podcast_fetch_want_url_;       // feed URL the worker fetches (guarded)
    PodcastFetchPurpose   podcast_fetch_purpose_ = PodcastFetchPurpose::Enter;  // (guarded)
    PodcastClient::Result podcast_fetch_result_;         // worker output (guarded)
    // Guard token for the feed-loading status pin: holds the exact "Loading ..." text
    // shown at fetch start. The tick loop keeps status_msg_ alive while a fetch is in
    // flight ONLY when status_msg_ still equals this - so an unrelated status set mid-
    // fetch expires on its own instead of inheriting the pin. Cleared on pickup.
    std::string           podcast_fetch_pin_;
    void startPodcastFetch(const std::string& url, PodcastFetchPurpose purpose);
    void pollPodcastFetch();        // UI thread, per-frame: install a finished fetch
    void enterPodcastSection();     // enter [Podcasts]: build the level-1 feed list
    void showPodcastFeedList();     // (re)populate dir_entries_/dir_display_ at level 1

    // Podcast Index search (slice 6) - find NEW feeds by term, mirroring the radio
    // results sub-mode (in_podcastindex_search_ + pi_results_ rendered through the
    // shared dir_entries_/dir_display_). Search runs on a worker thread (the podcast-
    // fetch async idiom, NOT radio's blocking call) so a slow/dead endpoint or a
    // disconnected network never hangs the UI. Single in-flight; installed per-frame.
    std::vector<PodcastIndexResult> pi_results_;
    std::thread              pi_search_thread_;
    std::atomic<bool>        pi_search_active_{false};
    std::atomic<bool>        pi_search_done_{false};
    std::mutex               pi_search_mtx_;
    std::string              pi_search_term_;              // shown in the results header (guarded)
    PodcastIndexSearchResult pi_search_result_;            // worker output (guarded)
    void startPodcastIndexSearch(const std::string& term); // kick the async byterm search
    void pollPodcastIndexSearch();  // UI thread, per-frame: install finished results
    void showPodcastIndexResults(); // populate dir_entries_/dir_display_ with pi_results_
    void drawPodcastIndexCreds();   // the [S]ignup / [E]nter / [Esc] first-use credentials modal
    void openUrlInBrowser(const std::string& url);  // shared launcher (ShellExecute / xdg-open)

    // Episode download-then-play (slice 3) + managed download queue (slice 4).
    // Episodes must be LOCAL files to seek / resume / finish / show chapters
    // (StreamSource is live-only), so an episode is downloaded to a cache file then
    // played via the normal LocalFileSource path. Streaming-to-disk with rip-style %
    // progress; ONE transfer at a time; cancellable so a mid-download quit aborts fast.
    //
    // Slice 4: a FIFO queue (max 5) drives that single worker one item at a time.
    // Both "download for later" and "play needs a download" feed the queue; a play
    // request jumps to the front. play_when_done makes the item auto-play when it
    // finishes; attempts drives retry-3-then-skip.
    struct PodcastQueueItem {
        std::string url, dest, id, title;
        std::string art_url;                // slice 5: episode-else-feed art URL
        bool        art_is_feed = false;    // true -> disk-cacheable show art
        std::string art_disk;               // feed-art cache path (when art_is_feed)
        std::string chapters_url;           // v1.4.0: <podcast:chapters> doc, primed on completion
        bool play_when_done = false;
        int  attempts = 0;
    };
    std::deque<PodcastQueueItem> podcast_queue_;            // pending downloads (FIFO, front = next)
    static constexpr int         PODCAST_QUEUE_MAX = 5;
    PodcastQueueItem             podcast_dl_item_;          // the ACTIVE download (UI-thread only)
    std::thread                  podcast_dl_thread_;
    std::atomic<bool>            podcast_dl_active_{false};   // worker in flight
    std::atomic<bool>            podcast_dl_done_{false};     // finished, ready for pickup
    std::atomic<bool>            podcast_dl_ok_{false};       // worker result (set before done)
    std::atomic<std::uint64_t>   podcast_dl_received_{0};     // bytes so far (progress)
    std::atomic<std::uint64_t>   podcast_dl_total_{0};        // total bytes (0 = unknown)
    std::int32_t                 podcast_dl_cancel_ = 0;      // int32 cancel flag (atomic_ref)
    std::string                  podcast_dl_status_;          // rip-style cmdline line
    int                          podcast_dl_ticks_ = 0;       // linger counter (~5s after done)
    int                          podcast_conflict_index_ = -1;// episode pending the play-conflict popup

    void enqueueEpisodeDownload(int episode_index, bool play_when_done, bool front);
    void pumpPodcastQueue();                    // start the next queued item if idle
    void startActiveDownload(const PodcastQueueItem& item);  // spawn the worker for one item
    void deleteEpisodeDownload(int episode_index);           // remove the cached file (state kept)
    bool episodeQueued(const std::string& id) const;         // in the pending queue?
    std::string episodeCacheFile(const std::string& feed_url, const PodcastEpisode& ep) const;  // pure, no mkdir
    void drawPodcastPlayConflict();             // the [W]ait / [P]lay-now / [Esc] popup

    // The podcast episode currently playing (resume latch + played-on-finish).
    std::string  podcast_playing_id_;          // episode id of the playing local file
    std::string  podcast_playing_path_;        // its local cache path
    double       podcast_playing_pos_    = 0.0;// latched position (persisted on transition)
    double       podcast_playing_dur_    = 0.0;// duration (for the finished threshold)
    bool         podcast_resume_pending_ = false;  // one-shot silent seek to the saved pos

    void startEpisodePlay(int episode_index);  // download-if-needed, then play
    void playEpisodeFile(const std::string& local_path, const std::string& id,
                         const std::string& art_url, bool art_is_feed,
                         const std::string& art_disk,
                         const std::string& chapters_url);   // play a cached episode (slice 5: art; v1.4.0: chapters)

    // Podcast episode art (slice 5): the playing episode's cover in the Info pane,
    // mirroring the radio-art path (URL -> CoverArt::bytesByUrl -> cover::render), NOT
    // info_art_ (which decodes a local file's embedded art). Single in-flight async
    // worker; feed show-art is disk-cached for offline, episode-level art is on-demand.
    std::thread                 podcast_art_thread_;
    std::atomic<bool>           podcast_art_active_{false};
    std::atomic<bool>           podcast_art_done_{false};
    std::mutex                  podcast_art_mtx_;
    std::string                 podcast_art_want_url_;    // (guarded) URL the worker fetched for
    std::vector<std::uint8_t>   podcast_art_result_;      // (guarded) fetched bytes
    std::string                 podcast_art_have_url_;    // URL whose bytes are in podcast_art_bytes_
    std::vector<std::uint8_t>   podcast_art_bytes_;       // resolved bytes (UI thread)
    cover::Rendered             podcast_art_;             // rendered grid (drawn like radio_art_)
    std::string                 podcast_art_render_key_;  // "<url>|<box>" of podcast_art_
    // The playing episode's resolved art (set by playEpisodeFile).
    std::string                 podcast_playing_art_url_;
    bool                        podcast_playing_art_is_feed_ = false;
    std::string                 podcast_playing_art_disk_;
    // The playing episode's feed chapters URL (v1.4.0), stashed like the art so the
    // per-tick chapter path and the play-time revalidation both have it without a
    // lookup. Empty when the episode publishes none. This is what keeps the weekly
    // revalidation running for the listener who plays but never opens the list.
    std::string                 podcast_playing_chapters_url_;
    // Chapter-pane origin: when ; loaded chapters from a PODCAST episode's cache
    // file, these carry the playEpisodeFile identity so selecting a chapter can
    // start the episode AS a podcast (art, no-scrobble, played state). Empty
    // id = the chapter list belongs to a plain file or book.
    std::string                 chapters_ep_id_;
    std::string                 chapters_ep_art_url_;
    bool                        chapters_ep_art_is_feed_ = false;
    std::string                 chapters_ep_art_disk_;
    std::string                 chapters_ep_chapters_url_;  // the episode's feed chapters URL (for play-from-chapter)
    // Was the audio actually on disk when ; built this list? Chapters can now be
    // VIEWED for an episode that was never downloaded (they come from the feed),
    // so "no file at Enter time" has two different honest answers and this tells
    // them apart: never downloaded, or downloaded and deleted since.
    bool                        chapters_ep_downloaded_ = false;

    // Feed-published chapters (v1.4.0). A <podcast:chapters> JSON document is
    // cached beside the episode as a two-file sidecar - <cachefile>.chapters.json
    // (the raw document) and <cachefile>.chapters.src (the URL that produced it) -
    // written by the download worker so a downloaded episode keeps its chapters
    // OFFLINE, and fetched lazily on ; for an episode that was never downloaded.
    // At most one network hit per episode; never a bulk fetch on feed refresh.
    //
    // The fetch is a single-flight worker with a want-key, the art/feed idiom:
    // want_ep_id_ is "what the user last asked for", so hammering ; is idempotent,
    // a fetch already in flight is never restarted or aborted, and a result that
    // lands for a row the user has moved off still reaches its cache - it just
    // does not open the pane.
    std::thread                 podcast_chap_thread_;
    std::atomic<bool>           podcast_chap_active_{false};
    std::atomic<bool>           podcast_chap_done_{false};
    std::atomic<bool>           podcast_chap_ok_{false};   // set before done_
    std::mutex                  podcast_chap_mtx_;
    std::string                 podcast_chap_key_;         // (guarded) episode id being fetched
    std::string                 podcast_chap_base_;        // (guarded) its episode cache path
    std::string                 chapters_want_ep_id_;      // "" = nothing outstanding
    std::string                 chapters_want_url_;
    std::string                 chapters_want_base_;
    // Who asked for the pending fetch. A ; browse is an explicit request: on
    // failure it toasts, on success it opens the pane. A play-time revalidation is
    // a background nicety like the download-worker priming: on failure it is logged
    // and dropped in silence, on success it folds into the live list with no pane.
    // The playing-episode fold is routed by result key regardless of this flag.
    bool                        chapters_want_is_browse_ = false;

    void startChaptersFetch();     // spawn for the current want, if the worker is idle
    void pollPodcastChapters();    // UI thread, per-frame: install a landed fetch
    // Resolve the playing episode's feed chapters at play time (B): fold a usable
    // sidecar in immediately, else - respecting the weekly revalidation, which may
    // delete a stale one - kick a background refetch that folds in when it lands.
    void resolvePlayingChapters(const std::string& local_path, const std::string& chapters_url);
    // THE playing-item predicate: true iff the current audio is a podcast episode
    // (state-derived, since episodes play as local files). One definition so every
    // consumer decides on purpose instead of inheriting music behaviour.
    bool isPlayingPodcast() const;
    void startPodcastArtFetch(const std::string& url, bool is_feed, const std::string& disk);
    // Resolve the playing episode's RSS-art BYTES independent of the Info pane (picks up
    // a finished fetch, kicks one if needed). Factored from refreshPodcastArt so the OS
    // media card can show the art with the Info pane closed, mirroring radio_bytes_.
    void resolvePodcastArtBytes(const std::string& url, bool is_feed, const std::string& disk);
    void refreshPodcastArt(const std::string& url, bool is_feed, const std::string& disk,
                           int box_cols, int box_rows);   // UI thread, per-frame
    void resolveEpisodeArt(const PodcastEpisode& ep, std::string& url,
                           bool& is_feed, std::string& disk) const;
    std::string feedArtDiskPath(const std::string& feed_url, const std::string& art_url) const;
    void pollPodcastDownload();                // UI thread, per-frame: finish -> play + linger
    void updatePodcastProgress();              // per-tick resume latch (mirrors updateBookProgress)
    void flushPodcastProgress();               // persist resume pos on transition / quit
    std::string episodeCachePath(const std::string& feed_url, const PodcastEpisode& ep);
    void migrateEpisodeCacheNames();           // feed entry: legacy Unicode cache names -> ASCII

    int  fav_cursor_     = 0;

    void        refreshDir();
    std::string formatTime(double seconds) const;
    // Directory watch
    std::filesystem::file_time_type dir_mtime_ {};
    int dir_poll_ticks_ = 0;
    // Marquee scroll state for long track names
    int         marquee_offset_  = 0;

    // Seek coalescing: holding [/] fires key-repeats, and applying a full device
    // stop/seek/start per repeat is choppy on MP3 (bit-reservoir warm-up after each
    // seek). Coalesce rapid repeats into ~one seek per cooldown while keeping single
    // taps instant. The run loop flushes the tail after the key is released.
    // The lanes (relative [/] + absolute OS SetPosition) live in SeekCoalescer,
    // shared so both routes flush through the one seekBy home (osmedia-seam).
    SeekCoalescer seek_;
    std::chrono::steady_clock::time_point last_seek_apply_ {};
    void        requestSeek(double delta);      // relative ([/] + MPRIS Seek)
    void        requestSeekAbs(double target);  // absolute (OS SetPosition)
    void        flushPendingSeek();
    bool        seekPending() const { return seek_.pending(); }
    int         marquee_ticks_   = 0;
    std::string marquee_last_path_;  // detect track change to reset offset
    static constexpr int MARQUEE_PAUSE  = 20;
    static constexpr int MARQUEE_SPEED  = 3;
    int  text_scroll_offset_ = 0;   // shared offset for all truncated text rows
    int  text_scroll_ticks_  = 0;

    // KITT scanner (radio status bar). A bright head + fading gradient tail sweeps
    // the idle gap between the now-playing title and the [LIVE] tag, bouncing end
    // to end. Rides the existing ~80ms draw heartbeat (advances on a wall-clock
    // step so keypress/tick jitter doesn't change its speed); theme-coloured via
    // the viz roles so each palette gets its own scanner. Drawn in drawProgress'
    // single stream-bar repaint pass (no separate refresh) so it can't fight the
    // title for the region. Always animates while live/connected.
    static constexpr double kScannerStepMs = 70.0;   // ~1 cell per heartbeat; tune by eye
    static constexpr int    kScannerTail   = 14;     // trailing cells behind the head (~4x the head)
    int    scanner_pos_ = 0;         // head column within the scanner track (0..track_w-1)
    int    scanner_dir_ = 1;         // +1 / -1 sweep direction
    std::chrono::steady_clock::time_point scanner_last_ {};  // last wall-clock advance

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
    static constexpr short CP_SELECTED_UNFOCUSED = 13;
    static constexpr short CP_VIZ_TIP    = 14;  // viz fractional tip: peak fg on default bg
    // Segmented "LED" spectrum: the viz band colours on the base/dark bg (like the
    // tip). A lower half-block (▄) drawn in these leaves a dark gap above each cell,
    // so a bar reads as stacked LEDs. Peak band reuses CP_VIZ_TIP.
    static constexpr short CP_VIZ_LOW_B  = 15;
    static constexpr short CP_VIZ_MID_B  = 16;
    static constexpr short CP_VIZ_HIGH_B = 17;
    static constexpr short CP_MODE       = 18;  // iHeart mode confirms (Ctrl+K/F6) on the cmdline: yellow on default bg

    // Art half-block cells allocate curses colours and pairs above the theme's
    // fixed CP_* range. The pair table is global; a collision here would let a
    // theme apply silently repaint art cells. Keep this assertion loud.
    static constexpr short kArtColourBase = 64;   // theme truecolour slots run 16..~31
    static constexpr short kArtPairBase   = 20;   // theme pairs run 1..CP_VIZ_HIGH_B

    static_assert(CP_VIZ_HIGH_B < kArtPairBase,
                  "theme colour pairs have grown into the art pair range; "
                  "raise kArtPairBase and re-check p_budget in allocArtColorPairs()");

    void initColours();
    void loadTheme(short* fg, short* bg);   // overrides defaults from theme.conf
    // Re-inits the colour pairs from kAwesomeThemes[config_.awesome_theme] without
    // rebuilding windows (truecolor via init_color, or nearest-ANSI-16 fallback).
    // Idempotent; safe to call repeatedly (F8 cycle, Ctrl+T into Awesome).
    void applyAwesomeTheme();
};
