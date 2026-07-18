#include "Config.h"
#include "StringUtils.h"
#include "AwesomeThemes.h"   // kNumAwesomeThemes for clamping awesome_theme on load

#include <fstream>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <ctime>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace fs = std::filesystem;

std::string DigiConfig::configPath() {
    std::string base;
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    base = (len > 0 && len < MAX_PATH) ? std::string(buf) + "\\RE-MOCT" : ".";
#else
    const char* home = getenv("HOME");
    base = home ? std::string(home) + "/.config/RE-MOCT" : ".";
#endif
    try { fs::create_directories(base); } catch (...) {}
    return base + "/remoct.conf";
}

std::string DigiConfig::themePath() {
    // Same directory as remoct.conf; swap the filename. Mirrors the dir logic
    // in configPath() rather than refactoring it, to keep this a surgical add.
    std::string base;
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    base = (len > 0 && len < MAX_PATH) ? std::string(buf) + "\\RE-MOCT" : ".";
#else
    const char* home = getenv("HOME");
    base = home ? std::string(home) + "/.config/RE-MOCT" : ".";
#endif
    try { fs::create_directories(base); } catch (...) {}
    return base + "/theme.conf";
}

void DigiConfig::addRecentTrack(const std::string& path) {
    // Never store CD audio tracks in recent list — hardware paths are volatile
    if (isCDTrackPath(path)) return;
    recent_tracks.erase(
        std::remove(recent_tracks.begin(), recent_tracks.end(), path),
        recent_tracks.end());
    recent_tracks.insert(recent_tracks.begin(), path);
    if ((int)recent_tracks.size() > RECENT_MAX)
        recent_tracks.resize((size_t)RECENT_MAX);
}

void DigiConfig::addFavTrack(const std::string& path) {
    // Audio files only — no CD paths, no dirs
    if (isCDTrackPath(path)) return;
    if (path.empty()) return;
    // Deduplicate — move to front if already present
    removeFavTrack(path);
    fav_tracks.insert(fav_tracks.begin(), path);
    // FIFO: drop oldest when over limit
    if ((int)fav_tracks.size() > FAV_MAX)
        fav_tracks.resize((size_t)FAV_MAX);
}

void DigiConfig::removeFavTrack(const std::string& path) {
    fav_tracks.erase(
        std::remove(fav_tracks.begin(), fav_tracks.end(), path),
        fav_tracks.end());
}

bool DigiConfig::isFav(const std::string& path) const {
    return std::find(fav_tracks.begin(), fav_tracks.end(), path) != fav_tracks.end();
}

void DigiConfig::addRadioStation(const std::string& url, const std::string& name) {
    // Preserve an existing stored name when re-added without one (e.g. replaying a
    // saved station), but let a non-empty name overwrite (rename). removeRadioStation
    // clears the map entry, so capture the name to keep before removing.
    std::string keep = name;
    if (keep.empty()) {
        auto it = radio_station_names.find(url);
        if (it != radio_station_names.end()) keep = it->second;
    }
    removeRadioStation(url);
    radio_stations.insert(radio_stations.begin(), url);
    if (!keep.empty()) radio_station_names[url] = keep;
    if ((int)radio_stations.size() > RADIO_MAX) {
        // Prune names for any URLs dropped off the FIFO tail.
        for (size_t i = (size_t)RADIO_MAX; i < radio_stations.size(); ++i)
            radio_station_names.erase(radio_stations[i]);
        radio_stations.resize((size_t)RADIO_MAX);
    }
}

void DigiConfig::removeRadioStation(const std::string& url) {
    radio_stations.erase(
        std::remove(radio_stations.begin(), radio_stations.end(), url),
        radio_stations.end());
    radio_station_names.erase(url);
}

bool DigiConfig::isRadioStation(const std::string& url) const {
    return std::find(radio_stations.begin(), radio_stations.end(), url) != radio_stations.end();
}

std::string DigiConfig::radioStationName(const std::string& url) const {
    auto it = radio_station_names.find(url);
    return it != radio_station_names.end() ? it->second : std::string();
}

void DigiConfig::addAudiobook(const std::string& path) {
    if (path.empty() || isCDTrackPath(path)) return;
    double keep = bookPos(path);                 // preserve resume across re-add
    removeAudiobook(path);                        // dedup (also clears pos)
    audiobooks.insert(audiobooks.begin(), path);
    if (keep > 0) audiobook_pos[path] = keep;
    if ((int)audiobooks.size() > BOOKS_MAX) {
        for (size_t i = (size_t)BOOKS_MAX; i < audiobooks.size(); ++i)
            audiobook_pos.erase(audiobooks[i]);
        audiobooks.resize((size_t)BOOKS_MAX);
    }
}

void DigiConfig::removeAudiobook(const std::string& path) {
    audiobooks.erase(std::remove(audiobooks.begin(), audiobooks.end(), path),
                     audiobooks.end());
    audiobook_pos.erase(path);
}

bool DigiConfig::isSavedBook(const std::string& path) const {
    return std::find(audiobooks.begin(), audiobooks.end(), path) != audiobooks.end();
}

void DigiConfig::setBookPos(const std::string& path, double sec) {
    if (path.empty()) return;
    if (sec < 0) sec = 0;
    audiobook_pos[path] = sec;
}

double DigiConfig::bookPos(const std::string& path) const {
    auto it = audiobook_pos.find(path);
    return it != audiobook_pos.end() ? it->second : 0.0;
}

void DigiConfig::recordPlay(const std::string& path) {
    if (path.empty()) return;
    if (isCDTrackPath(path)) return;  // CD tracks are volatile — never record stats
    auto& s = track_stats[path];
    ++s.play_count;
    s.last_played = std::time(nullptr);
}

void DigiConfig::load() {
    std::ifstream f(configPath());
    if (!f) return;
    playlist_paths.clear();
    bookmarks.clear();
    fav_tracks.clear();
    radio_stations.clear();
    radio_station_names.clear();
    recent_tracks.clear();
    track_stats.clear();
    audiobooks.clear();
    audiobook_pos.clear();

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();

        if      (key == "last_dir")         last_dir          = val;
        else if (key == "playlist_current") try { playlist_current = (std::size_t)std::stoul(val); } catch (...) {}
        else if (key == "volume")           try { volume          = std::stof(val); } catch (...) {}
        else if (key == "repeat_mode")      try { repeat_mode      = std::stoi(val); } catch (...) {}
        else if (key == "shuffle")          shuffle           = (val == "1");
        else if (key == "toast_enabled")    toast_enabled     = (val == "1");
        else if (key == "eq_enabled")       eq_enabled        = (val == "1");
        else if (key == "discord_presence") discord_presence  = (val == "1");
        else if (key == "os_media_control") os_media_control   = (val != "0");
        else if (key == "awesome_mode")      awesome_mode       = (val == "1");
        else if (key == "viz_led")            viz_led            = (val == "1");
        else if (key == "awesome_theme")     try { awesome_theme = std::stoi(val); } catch (...) {}
        else if (key == "prefer_digital_stream") prefer_digital_stream = (val == "1");
        else if (key == "iheart_probe_minted")   iheart_probe_minted   = (val == "1");
        else if (key == "repin_mode")            try { repin_mode = std::clamp(std::stoi(val), 0, 2); } catch (...) {}
        else if (key == "nerd_icons")         nerd_icons         = (val == "1");
        else if (key == "follow_playing")     follow_playing     = (val == "1");
        else if (key == "show_filetype")      show_filetype      = (val == "1");
        else if (key == "rip_formats")        rip_formats        = val;
        else if (key == "opus_bitrate")       try { opus_bitrate = std::clamp(std::stoi(val), 6000, 510000); } catch (...) {}
        else if (key == "wavpack_mode") {
            if (val == "fast" || val == "normal" || val == "high" || val == "very_high")
                wavpack_mode = val;
        }
        else if (key == "flac_level")         try { flac_level = std::clamp(std::stoi(val), 0, 8); } catch (...) {}
        else if (key == "mp3") {
            // Accept V0-V9 (case-insensitive); anything else keeps the default.
            if (val.size() == 2 && (val[0] == 'V' || val[0] == 'v') &&
                val[1] >= '0' && val[1] <= '9')
                mp3 = std::string("V") + val[1];
        }
        else if (key == "mp3_cbr")            mp3_cbr            = (val == "1");
        else if (key == "mp3_cbr_bitrate")    try { mp3_cbr_bitrate = std::clamp(std::stoi(val), 6000, 510000); } catch (...) {}
        else if (key == "opus_vbr")           opus_vbr           = (val != "0");
        else if (key == "aac_vbr")            aac_vbr            = (val != "0");
        else if (key == "aac_vbr_level")      try { aac_vbr_level = std::clamp(std::stoi(val), 1, 5); } catch (...) {}
        else if (key == "aac_cbr_bitrate")    try { aac_cbr_bitrate = std::clamp(std::stoi(val), 6000, 510000); } catch (...) {}
        else if (key == "rec_format") {
            // Lossy re-encode or the slice-B copy mode; anything else keeps opus.
            if (val == "opus" || val == "mp3" || val == "m4a" || val == "copy") rec_format = val;
        }
        else if (key == "rec_mp3") {
            if (val.size() == 2 && (val[0] == 'V' || val[0] == 'v') &&
                val[1] >= '0' && val[1] <= '9')
                rec_mp3 = std::string("V") + val[1];
        }
        else if (key == "rec_mp3_cbr")        rec_mp3_cbr        = (val == "1");
        else if (key == "rec_mp3_cbr_bitrate") try { rec_mp3_cbr_bitrate = std::clamp(std::stoi(val), 6000, 510000); } catch (...) {}
        else if (key == "rec_opus_bitrate")   try { rec_opus_bitrate = std::clamp(std::stoi(val), 6000, 510000); } catch (...) {}
        else if (key == "rec_opus_vbr")       rec_opus_vbr       = (val != "0");
        else if (key == "rec_aac_vbr")        rec_aac_vbr        = (val != "0");
        else if (key == "rec_aac_vbr_level")  try { rec_aac_vbr_level = std::clamp(std::stoi(val), 1, 5); } catch (...) {}
        else if (key == "rec_aac_cbr_bitrate") try { rec_aac_cbr_bitrate = std::clamp(std::stoi(val), 6000, 510000); } catch (...) {}
        else if (key == "rec_split")          rec_split          = (val == "1");
        else if (key == "rec_dir")            rec_dir            = val;
        else if (key == "split_offset_ms")    try { split_offset_ms = std::clamp(std::stoi(val), -5000, 5000); } catch (...) {}
        else if (key == "rec_ads") {
            if (val == "save" || val == "discard") rec_ads = val;
        }
        else if (key == "wingui_font")        wingui_font        = val;
        else if (key == "wingui_cols")        try { wingui_cols = std::stoi(val); } catch (...) {}
        else if (key == "wingui_rows")        try { wingui_rows = std::stoi(val); } catch (...) {}
        else if (key == "lastfm-key")       lastfm_key        = val;
        else if (key == "lastfm-secret")    lastfm_secret     = val;
        else if (key == "lastfm-session")   lastfm_session    = val;
        else if (key == "lastfm-user")      lastfm_user       = val;
        else if (key == "lastfm-pending")   lastfm_pending    = val;
        else if (key == "lb-token")          listenbrainz_token = val;
        else if (key == "lb-user")           listenbrainz_user  = val;
        else if (key.substr(0,3) == "eq_" && key.size() == 4) {
            int b = key[3] - '0';
            if (b >= 0 && b <= 9) try { eq_gains[b] = std::stof(val); } catch (...) {}
        }
        else if (key == "track")    playlist_paths.push_back(val);
        else if (key == "bookmark") bookmarks.push_back(val);
        else if (key == "fav") {
            if (!isCDTrackPath(val) && !val.empty())
                fav_tracks.push_back(val);
        }
        else if (key == "station") {
            if (!val.empty()) {
                // Optional friendly name after a tab: "station=<url>\t<name>".
                // Old name-less lines parse with tab==npos => name stays empty.
                std::string surl = val, sname;
                auto tab = val.find('\t');
                if (tab != std::string::npos) {
                    surl  = val.substr(0, tab);
                    sname = val.substr(tab + 1);
                }
                if (!surl.empty()) {
                    radio_stations.push_back(surl);
                    if (!sname.empty()) radio_station_names[surl] = sname;
                }
            }
        }
        else if (key == "recent") {
            // Silently drop any CD paths that leaked into a pre-fix config
            if (!isCDTrackPath(val)) recent_tracks.push_back(val);
        }
        else if (key == "book") {
            // Format: book=path|resume_seconds  (| is illegal in Windows paths)
            std::string bpath = val; double bpos = 0.0;
            auto bar = val.rfind('|');
            if (bar != std::string::npos) {
                bpath = val.substr(0, bar);
                try { bpos = std::stod(val.substr(bar + 1)); } catch (...) { bpos = 0.0; }
            }
            if (!bpath.empty() && !isCDTrackPath(bpath)) {
                audiobooks.push_back(bpath);
                if (bpos > 0) audiobook_pos[bpath] = bpos;
            }
        }
        else if (key == "stat") {
            // Format: stat=path|count|timestamp
            auto p1 = val.rfind('|');
            if (p1 == std::string::npos) continue;
            auto p2 = val.rfind('|', p1 - 1);
            if (p2 == std::string::npos) continue;
            std::string tpath = val.substr(0, p2);
            // Silently drop any CD stat entries from pre-fix configs
            if (isCDTrackPath(tpath)) continue;
            try {
                int count = std::stoi(val.substr(p2 + 1, p1 - p2 - 1));
                std::time_t ts = (std::time_t)std::stoll(val.substr(p1 + 1));
                track_stats[tpath] = { count, ts };
            } catch (...) {}
        }
    }

    volume      = std::max(0.0f, std::min(2.0f, volume));
    repeat_mode = std::max(0, std::min(2, repeat_mode));
    if (awesome_theme < 0 || awesome_theme >= kNumAwesomeThemes) awesome_theme = 0;
    if (playlist_current >= playlist_paths.size() && !playlist_paths.empty())
        playlist_current = 0;
    if ((int)recent_tracks.size() > RECENT_MAX)
        recent_tracks.resize((size_t)RECENT_MAX);
}

void DigiConfig::save() const {
    const std::string dest = configPath();
    const std::string tmp  = dest + ".tmp";

    // Strip line-structure-breaking control chars from a persisted value. CR/LF
    // would split one logical value across config lines on reload. Tabs are left
    // intact here (they are legal in paths and only dangerous in the station
    // name, which is handled at that call site below).
    auto nl = [](std::string s) {
        for (char& c : s) if (c == '\r' || c == '\n') c = ' ';
        return s;
    };

    // Write the full config to a sibling temp file first, then atomically swap it
    // over the real file. A crash, full disk, or power loss mid-write can only
    // damage the temp; the live config is replaced in one step or not at all.
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << "# RE-MOCT configuration - auto-generated\n";
        f << "last_dir="         << nl(last_dir)     << "\n";
        f << "playlist_current=" << playlist_current << "\n";
        f << "volume="           << volume           << "\n";
        f << "repeat_mode="      << repeat_mode      << "\n";
        f << "shuffle="          << (shuffle ? "1" : "0") << "\n";
        f << "toast_enabled="    << (toast_enabled ? "1" : "0") << "\n";
        f << "eq_enabled="       << (eq_enabled ? "1" : "0") << "\n";
        f << "discord_presence=" << (discord_presence ? "1" : "0") << "\n";
        f << "os_media_control=" << (os_media_control ? "1" : "0") << "\n";
        f << "awesome_mode="      << (awesome_mode ? "1" : "0") << "\n";
        f << "viz_led="           << (viz_led ? "1" : "0") << "\n";
        f << "awesome_theme="     << awesome_theme << "\n";
        f << "prefer_digital_stream=" << (prefer_digital_stream ? "1" : "0") << "\n";
        f << "iheart_probe_minted="   << (iheart_probe_minted ? "1" : "0") << "\n";
        f << "repin_mode="            << repin_mode << "\n";
        f << "nerd_icons="        << (nerd_icons ? "1" : "0") << "\n";
        f << "follow_playing="    << (follow_playing ? "1" : "0") << "\n";
        f << "show_filetype="     << (show_filetype ? "1" : "0") << "\n";
        f << "rip_formats="       << rip_formats << "\n";
        f << "flac_level="        << flac_level  << "\n";
        f << "mp3="               << mp3         << "\n";
        f << "mp3_cbr="           << (mp3_cbr ? "1" : "0") << "\n";
        f << "mp3_cbr_bitrate="   << mp3_cbr_bitrate << "\n";
        f << "opus_bitrate="      << opus_bitrate << "\n";
        f << "opus_vbr="          << (opus_vbr ? "1" : "0") << "\n";
        f << "wavpack_mode="      << wavpack_mode << "\n";
        f << "aac_vbr="           << (aac_vbr ? "1" : "0") << "\n";
        f << "aac_vbr_level="     << aac_vbr_level << "\n";
        f << "aac_cbr_bitrate="   << aac_cbr_bitrate << "\n";
        f << "rec_format="        << rec_format << "\n";
        f << "rec_mp3="           << rec_mp3 << "\n";
        f << "rec_mp3_cbr="       << (rec_mp3_cbr ? "1" : "0") << "\n";
        f << "rec_mp3_cbr_bitrate=" << rec_mp3_cbr_bitrate << "\n";
        f << "rec_opus_bitrate="  << rec_opus_bitrate << "\n";
        f << "rec_opus_vbr="      << (rec_opus_vbr ? "1" : "0") << "\n";
        f << "rec_aac_vbr="       << (rec_aac_vbr ? "1" : "0") << "\n";
        f << "rec_aac_vbr_level=" << rec_aac_vbr_level << "\n";
        f << "rec_aac_cbr_bitrate=" << rec_aac_cbr_bitrate << "\n";
        f << "rec_split="         << (rec_split ? "1" : "0") << "\n";
        if (!rec_dir.empty())     f << "rec_dir=" << rec_dir << "\n";
        f << "split_offset_ms="   << split_offset_ms << "\n";
        f << "rec_ads="           << rec_ads << "\n";
        if (!wingui_font.empty()) f << "wingui_font="    << wingui_font << "\n";
        if (wingui_cols > 0)      f << "wingui_cols="    << wingui_cols << "\n";
        if (wingui_rows > 0)      f << "wingui_rows="    << wingui_rows << "\n";
        if (!lastfm_key.empty())     f << "lastfm-key="     << nl(lastfm_key)     << "\n";
        if (!lastfm_secret.empty())  f << "lastfm-secret="  << nl(lastfm_secret)  << "\n";
        if (!lastfm_session.empty()) f << "lastfm-session=" << nl(lastfm_session) << "\n";
        if (!lastfm_user.empty())    f << "lastfm-user="    << nl(lastfm_user)    << "\n";
        if (!lastfm_pending.empty()) f << "lastfm-pending=" << nl(lastfm_pending) << "\n";
        if (!listenbrainz_token.empty()) f << "lb-token=" << nl(listenbrainz_token) << "\n";
        if (!listenbrainz_user.empty())  f << "lb-user="  << nl(listenbrainz_user)  << "\n";
        for (int b = 0; b < 10; ++b)
            f << "eq_" << b << "=" << eq_gains[b] << "\n";
        for (const auto& p : playlist_paths)  f << "track="    << nl(p) << "\n";
        for (const auto& b : bookmarks)       f << "bookmark=" << nl(b) << "\n";
        for (const auto& fv : fav_tracks)
            if (!isCDTrackPath(fv))            f << "fav="     << nl(fv) << "\n";
        for (const auto& st : radio_stations) {
            std::string url = nl(st);
            auto it = radio_station_names.find(st);
            if (it != radio_station_names.end() && !it->second.empty()) {
                // The url\tname delimiter means a tab in the *name* would mis-split
                // on reload, so fold tabs (and CR/LF) in the name to spaces.
                std::string name = nl(it->second);
                for (char& c : name) if (c == '\t') c = ' ';
                f << "station="  << url << "\t" << name << "\n";
            } else {
                f << "station="  << url << "\n";
            }
        }
        for (const auto& r : recent_tracks)
            if (!isCDTrackPath(r))
                f << "recent=" << nl(r) << "\n";
        for (const auto& bk : audiobooks)
            if (!isCDTrackPath(bk)) {
                auto it = audiobook_pos.find(bk);
                double pos = (it != audiobook_pos.end()) ? it->second : 0.0;
                f << "book=" << nl(bk) << "|" << pos << "\n";
            }
        // Write stats — cap at STATS_MAX, skip any CD paths
        int written = 0;
        for (const auto& kv : track_stats) {
            if (written >= STATS_MAX) break;
            if (isCDTrackPath(kv.first)) continue;  // should not exist but guard anyway
            f << "stat=" << nl(kv.first) << "|" << kv.second.play_count
              << "|" << (long long)kv.second.last_played << "\n";
            ++written;
        }
        f.flush();
        if (!f.good()) {                 // write failed — leave the live config untouched
            f.close();
            std::error_code rm; fs::remove(tmp, rm);
            return;
        }
    } // ofstream closed/flushed here, before the swap

    // Atomic replace. On Windows std::filesystem / _wrename won't overwrite an
    // existing target, so use MoveFileEx with REPLACE_EXISTING; WRITE_THROUGH
    // flushes the metadata to disk before returning. POSIX rename() already
    // replaces atomically.
#ifdef _WIN32
    bool ok = MoveFileExA(tmp.c_str(), dest.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    bool ok = !ec;
#endif
    if (!ok) {
        // Swap failed (locked target, cross-volume temp, ...). Fall back to a
        // best-effort non-atomic overwrite so settings still persist this run.
        std::error_code ec2;
        fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec2);
        std::error_code ec3; fs::remove(tmp, ec3);
    }
}
