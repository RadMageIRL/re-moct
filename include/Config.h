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

    // UI theme toggle (Ctrl+T): false = Classic, true = Awesome (rounded panels)
    bool  awesome_mode = false;
    int   awesome_theme = 0;   // index into kAwesomeThemes (Ctrl+T Awesome look; F8 cycles)
    bool  prefer_digital_stream = false;   // iHeart: try web-player (ad-reduced) rendition; raw broadcast otherwise
    bool  iheart_probe_minted   = false;   // probe: use a minted anonymous profileId on the digital handshake (deep-log A/B only)

    // Nerd Font pane-title icons (Ctrl+N). Requires a Nerd Font terminal font.
    bool  nerd_icons = false;

    // Windows PDCursesMod wingui build ONLY: the GDI window owns its font (unlike
    // a terminal, where the font is the terminal's). This names the face used, so
    // box-drawing corners + viz blocks + Nerd icons render. Empty => the built-in
    // default ("JetBrainsMono NFM"). Any .ttf dropped in <exeDir>/fonts/ is loaded
    // process-privately at startup, so a bundled font need not be installed.
    std::string wingui_font;

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
