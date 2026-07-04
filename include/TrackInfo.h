// TrackInfo.h — track metadata record (Phase 2 slice B: hoisted verbatim from
// AudioManager.h so LocalFileSource can carry its metadata without depending on
// the manager's header). Pure struct, no platform or library dependencies;
// AudioManager.h includes this, so every existing consumer sees it transitively
// with zero call-site changes.
#pragma once
#include <cstdint>
#include <string>

struct TrackInfo {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    int  duration_sec = 0;
    int  bitrate_kbps = 0;
    int  sample_rate  = 0;
    int  channels     = 0;
    std::string genre;
    std::string comment;
    int  year         = 0;
    int  track_num    = 0;
    int  bpm          = 0;   // 0 = not yet detected
    float replaygain_db = 0.0f;
    std::uintmax_t file_size_bytes = 0;
};
