#pragma once
#include <string>
#include <vector>

struct LrcLine {
    double      time_sec;   // timestamp in seconds
    std::string text;
};

struct LrcData {
    std::vector<LrcLine> lines;
    bool loaded = false;

    // Load from .lrc file path (returns false if not found or parse failure)
    bool loadFile(const std::string& lrc_path);

    // Given a .mp3/.flac/etc path, try to find a sibling .lrc file
    static std::string findLrc(const std::string& audio_path);

    // Return index of the current line for playback position (0-based, -1 if before first)
    int currentLine(double pos_sec) const;

    void clear() { lines.clear(); loaded = false; }
};
