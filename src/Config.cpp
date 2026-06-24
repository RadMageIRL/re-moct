#include "Config.h"
#include "StringUtils.h"

#include <fstream>
#include <algorithm>
#include <filesystem>
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

void DigiConfig::addRadioStation(const std::string& url) {
    removeRadioStation(url);
    radio_stations.insert(radio_stations.begin(), url);
    if ((int)radio_stations.size() > RADIO_MAX)
        radio_stations.resize((size_t)RADIO_MAX);
}

void DigiConfig::removeRadioStation(const std::string& url) {
    radio_stations.erase(
        std::remove(radio_stations.begin(), radio_stations.end(), url),
        radio_stations.end());
}

bool DigiConfig::isRadioStation(const std::string& url) const {
    return std::find(radio_stations.begin(), radio_stations.end(), url) != radio_stations.end();
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
    recent_tracks.clear();
    track_stats.clear();

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
            if (!val.empty()) radio_stations.push_back(val);
        }
        else if (key == "recent") {
            // Silently drop any CD paths that leaked into a pre-fix config
            if (!isCDTrackPath(val)) recent_tracks.push_back(val);
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
    if (playlist_current >= playlist_paths.size() && !playlist_paths.empty())
        playlist_current = 0;
    if ((int)recent_tracks.size() > RECENT_MAX)
        recent_tracks.resize((size_t)RECENT_MAX);
}

void DigiConfig::save() const {
    std::ofstream f(configPath(), std::ios::trunc);
    if (!f) return;
    f << "# RE-MOCT configuration - auto-generated\n";
    f << "last_dir="         << last_dir         << "\n";
    f << "playlist_current=" << playlist_current << "\n";
    f << "volume="           << volume           << "\n";
    f << "repeat_mode="      << repeat_mode      << "\n";
    f << "shuffle="          << (shuffle ? "1" : "0") << "\n";
    f << "toast_enabled="    << (toast_enabled ? "1" : "0") << "\n";
    f << "eq_enabled="       << (eq_enabled ? "1" : "0") << "\n";
    f << "discord_presence=" << (discord_presence ? "1" : "0") << "\n";
    if (!lastfm_key.empty())     f << "lastfm-key="     << lastfm_key     << "\n";
    if (!lastfm_secret.empty())  f << "lastfm-secret="  << lastfm_secret  << "\n";
    if (!lastfm_session.empty()) f << "lastfm-session=" << lastfm_session << "\n";
    if (!lastfm_user.empty())    f << "lastfm-user="    << lastfm_user    << "\n";
    if (!lastfm_pending.empty()) f << "lastfm-pending=" << lastfm_pending << "\n";
    if (!listenbrainz_token.empty()) f << "lb-token=" << listenbrainz_token << "\n";
    if (!listenbrainz_user.empty())  f << "lb-user="  << listenbrainz_user  << "\n";
    for (int b = 0; b < 10; ++b)
        f << "eq_" << b << "=" << eq_gains[b] << "\n";
    for (const auto& p : playlist_paths)  f << "track="    << p << "\n";
    for (const auto& b : bookmarks)       f << "bookmark=" << b << "\n";
    for (const auto& fv : fav_tracks)
        if (!isCDTrackPath(fv))            f << "fav="     << fv << "\n";
    for (const auto& st : radio_stations)  f << "station="  << st << "\n";
    for (const auto& r : recent_tracks)
        if (!isCDTrackPath(r))
            f << "recent=" << r << "\n";
    // Write stats — cap at STATS_MAX, skip any CD paths
    int written = 0;
    for (const auto& kv : track_stats) {
        if (written >= STATS_MAX) break;
        if (isCDTrackPath(kv.first)) continue;  // should not exist but guard anyway
        f << "stat=" << kv.first << "|" << kv.second.play_count
          << "|" << (long long)kv.second.last_played << "\n";
        ++written;
    }
}
