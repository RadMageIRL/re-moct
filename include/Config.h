#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstddef>
#include <ctime>

struct TrackStats {
    int       play_count   = 0;
    std::time_t last_played = 0;  // unix timestamp, 0 = never
};

struct DigiConfig {
    std::string              last_dir;
    std::vector<std::string> playlist_paths;
    std::size_t              playlist_current = 0;

    // Last.fm scrobbling
    std::string              lastfm_key;       // API key (user-provided)
    std::string              lastfm_secret;    // shared secret (user-provided)
    std::string              lastfm_session;   // session key (obtained via auth)
    std::string              lastfm_user;      // username (from auth)
    std::string              lastfm_pending;   // in-flight auth token (survives restart)
    std::string              listenbrainz_token; // user token (listenbrainz.org/settings)
    std::string              listenbrainz_user;  // username (from validate-token)
    float                    volume           = 1.0f;
    int                      repeat_mode      = 0;
    bool                     shuffle          = false;
    bool                     toast_enabled    = false;
    // EQ state
    bool  eq_enabled = false;
    float eq_gains[10] {};

    // Discord Rich Presence toggle (Ctrl+D)
    bool  discord_presence = false;

    // OS media control (osmedia-seam): publish now-playing to the OS media
    // surface (Windows SMTC / Linux MPRIS) and accept its transport keys /
    // scrubber. Default on; config-only toggle (no keybinding).
    bool  os_media_control = true;

    // ── Rip output (rip-format-select) ────────────────────────────────────
    // rip_formats is the DEFAULT selection only: it seeds the session's
    // format set once at startup; the modal's toggles are never written back.
    // Defaults must equal the pre-config literals (both formats, FLAC level
    // 5, LAME V0) so an absent/default config rips byte-identically.
    std::string rip_formats = "flac,mp3";  // comma-separated kRipFormats labels
    int         flac_level  = 5;           // FLAC compression 0-8
    std::string mp3         = "V0";        // LAME quality V0-V9 (VBR axis)
    bool        mp3_cbr     = false;       // MP3 mode: false = VBR (V-scale), true = CBR
    int         mp3_cbr_bitrate = 256000;  // MP3 CBR bitrate, snapped to {96,128,256,320}k
    int         opus_bitrate = 128000;     // Opus bitrate, 6000-510000 (= kOpusDefaultBitrate)
    bool        opus_vbr    = true;        // Opus mode: true = VBR (default), false = CBR
    std::string wavpack_mode = "normal";   // fast|normal|high|very_high (= kWavPackDefaultMode)
    bool        aac_vbr     = true;        // AAC/M4A mode: true = VBR ladder (default), false = CBR
    int         aac_vbr_level = 4;         // AAC VBR quality 1-5 (5 best; = kAacDefaultVbrLevel)
    int         aac_cbr_bitrate = 128000;  // AAC CBR bitrate, snapped to {96,128,256,320}k

    // ── Stream recording (stream-record R2 / encoder-bitrate-mode) ─────────
    // Session seeds only, like rip_formats - the [Rec] panel's toggles are
    // never written back. Recording carries its OWN quality set (rec_mp3 /
    // rec_opus_* below): the earlier "reuses the rip mp3 / opus_bitrate knobs"
    // invariant is intentionally retired so radio can be right-sized (Opus 96)
    // independently of a high-quality CD rip (V0).
    std::string rec_format = "opus";  // opus | mp3 | m4a | copy (single-select default;
                                      // copy = as-broadcast capture, slice B)
    std::string rec_mp3          = "V5";    // rec MP3 V-scale (right-sized default)
    bool        rec_mp3_cbr      = false;   // rec MP3 mode: false = VBR, true = CBR
    int         rec_mp3_cbr_bitrate = 256000; // rec MP3 CBR bitrate, snapped to ladder
    int         rec_opus_bitrate = 96000;   // rec Opus bitrate (right-sized default)
    bool        rec_opus_vbr     = true;    // rec Opus mode: true = VBR, false = CBR
    bool        rec_aac_vbr      = true;    // rec AAC/M4A mode: true = VBR, false = CBR
    int         rec_aac_vbr_level = 4;      // rec AAC VBR quality 1-5 (5 best)
    int         rec_aac_cbr_bitrate = 128000; // rec AAC CBR bitrate, snapped to ladder
    bool        rec_split  = true;    // split on metadata title change
    std::string rec_dir;              // "" = <music>/re-moct/recordings, resolved at record start
    // split-trim: hold acting on a title change by this many ms so the closing
    // cut keeps its outro. Signed by design; negative (lead) is RESERVED and
    // clamps to 0 until implemented (docs/split-trim-ad-aware-plan.md).
    int         split_offset_ms = 1200;
    // ad-aware: save (route non-song cuts to <Station>/ads/) | discard (do not
    // write them - trusts station metadata, the informed opt-in).
    std::string rec_ads = "save";

    // UI theme toggle (Ctrl+T): false = Classic, true = Awesome (rounded panels)
    bool  awesome_mode = false;
    int   awesome_theme = 0;   // index into kAwesomeThemes (Ctrl+T Awesome look; F8 cycles)
    // Spectrum style (F2): false = classic solid bars (default), true = 80s segmented
    // "LED" bars (stacked half-block segments, colour-by-height).
    bool  viz_led = false;
    bool  prefer_digital_stream = false;   // iHeart: try web-player (ad-reduced) rendition; raw broadcast otherwise
    bool  iheart_probe_minted   = false;   // probe: use a minted anonymous profileId on the digital handshake (deep-log A/B only)
    int   repin_mode            = 2;       // iHeart re-pin (F6): 0=off, 1=ad-escape (hard ad-evidence only), 2=hybrid (evidence or spot segments in the timed window; default), 3=timed (legacy duration-only - will thrash talk shows), 4=live-edge (browser-style: follow the live edge, drift-triggered, always current - twitchier on a laggy connection)

    // Nerd Font pane-title icons (Ctrl+N). Requires a Nerd Font terminal font.
    bool  nerd_icons = false;

    // Follow the playing track: auto-move the playlist cursor to the playing row on
    // track change (F3). false = cursor stays where you left it (browse undisturbed).
    bool  follow_playing = true;

    // Per-row filetype column in the playlist (F11): FLAC/MP3/... between title
    // and duration, MOC-style. Default off.
    bool  show_filetype = false;

    // Windows PDCursesMod wingui build ONLY: the GDI window owns its font (unlike
    // a terminal, where the font is the terminal's). This names the face used, so
    // box-drawing corners + viz blocks + Nerd icons render. Empty => the built-in
    // default ("JetBrainsMono NFM"). Any .ttf dropped in <exeDir>/fonts/ is loaded
    // process-privately at startup, so a bundled font need not be installed.
    std::string wingui_font;

    // Windows wingui build ONLY: the last GDI window size (grid rows x cols),
    // remembered across launches. We can't lean on PDCurses' own registry
    // persistence because we wipe that key each launch to keep our font choice
    // (see initWinguiFont), so we persist the size here instead. 0 => unset
    // (use the wingui default). Restored via resize_term() at startup.
    int wingui_cols = 0;
    int wingui_rows = 0;

    // Bookmarks: list of saved directory paths
    std::vector<std::string> bookmarks;

    // Recently played: list of track paths, most recent first, max 50
    std::vector<std::string> recent_tracks;
    static constexpr int     RECENT_MAX = 50;

    // Favourites: curated list of audio file paths, max 50, FIFO on overflow
    std::vector<std::string> fav_tracks;
    static constexpr int     FAV_MAX = 50;

    void addFavTrack(const std::string& path);
    void removeFavTrack(const std::string& path);
    bool isFav(const std::string& path) const;

    // Saved internet-radio stations (URLs), max 50, FIFO on overflow
    std::vector<std::string> radio_stations;
    // Optional friendly names keyed by station URL (parallel to radio_stations).
    // Absent entry => fall back to the URL-derived label. Persisted as a tab on
    // the station= line; backward-compatible with old name-less entries.
    std::unordered_map<std::string, std::string> radio_station_names;
    static constexpr int     RADIO_MAX = 50;
    void addRadioStation(const std::string& url, const std::string& name = "");
    void removeRadioStation(const std::string& url);
    bool isRadioStation(const std::string& url) const;
    std::string radioStationName(const std::string& url) const;   // "" if none

    // Subscribed podcast feeds (feed URLs, most-recent-first), max 50, FIFO on
    // overflow. Mirrors the radio storage shape. The side maps cache the show
    // title and feed-art URL so the [Podcasts] list renders without re-fetching;
    // both are refreshed on each successful feed fetch. Persisted plaintext as
    // "podcast=<url>\t<title>\t<art>" (podcast feeds are non-sensitive, same
    // category as radio/books; NOT routed through secret::protect).
    std::vector<std::string> podcast_feeds;
    std::unordered_map<std::string, std::string> podcast_feed_titles;
    std::unordered_map<std::string, std::string> podcast_feed_art;
    static constexpr int     PODCAST_MAX = 50;
    void addPodcastFeed(const std::string& url, const std::string& title = "",
                        const std::string& art = "");
    void removePodcastFeed(const std::string& url);
    bool isPodcastFeed(const std::string& url) const;
    std::string podcastFeedTitle(const std::string& url) const;   // "" if none
    std::string podcastFeedArt(const std::string& url) const;     // "" if none

    // Audiobooks: curated book paths (most-recent-first) + per-book resume
    // position in seconds. Mirrors the radio storage shape (list + side map).
    std::vector<std::string> audiobooks;
    std::unordered_map<std::string, double> audiobook_pos;
    static constexpr int     BOOKS_MAX = 200;
    void   addAudiobook(const std::string& path);
    void   removeAudiobook(const std::string& path);
    bool   isSavedBook(const std::string& path) const;
    void   setBookPos(const std::string& path, double sec);
    double bookPos(const std::string& path) const;   // 0.0 if none

    // Per-track play statistics keyed by file path
    std::unordered_map<std::string, TrackStats> track_stats;
    static constexpr int STATS_MAX = 5000;  // cap to avoid unbounded growth

    void addRecentTrack(const std::string& path);
    void recordPlay(const std::string& path);   // increment count + timestamp

    static std::string configPath();
    static std::string themePath();
    void load();
    void save() const;
};
