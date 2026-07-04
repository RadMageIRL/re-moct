#include "LrcData.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

std::string LrcData::findLrc(const std::string& audio_path) {
    fs::path p(audio_path);
    fs::path lrc = p.parent_path() / (p.stem().string() + ".lrc");
    if (fs::exists(lrc)) return lrc.string();
    // Case-insensitive fallback: scan dir
    try {
        for (const auto& de : fs::directory_iterator(p.parent_path())) {
            std::string fn = de.path().filename().string();
            std::string stem_lower = fn;
            std::transform(stem_lower.begin(), stem_lower.end(), stem_lower.begin(), ::tolower);
            std::string want_lower = p.stem().string();
            std::transform(want_lower.begin(), want_lower.end(), want_lower.begin(), ::tolower);
            std::string ext_lower = de.path().extension().string();
            std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
            if (stem_lower == want_lower && ext_lower == ".lrc")
                return de.path().string();
        }
    } catch (...) {}
    return "";
}

bool LrcData::loadFile(const std::string& lrc_path) {
    clear();
    std::ifstream f(lrc_path);
    if (!f) return false;

    std::regex ts_re(R"(\[(\d{1,2}):(\d{2})[\.\:](\d{1,3})\])");
    std::regex offset_re(R"(\[offset\s*:\s*(-?\d+)\])", std::regex::icase);

    double offset_sec = 0.0;  // [offset:N] in milliseconds, positive = shift forward

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // Check for [offset:N] metadata tag first
        std::smatch om;
        if (std::regex_search(line, om, offset_re)) {
            try { offset_sec = std::stod(om[1].str()) / 1000.0; }
            catch (...) { offset_sec = 0.0; }   // absurd [offset:N] -> out_of_range
            continue;
        }

        auto ts_begin = std::sregex_iterator(line.begin(), line.end(), ts_re);
        auto ts_end   = std::sregex_iterator();
        if (ts_begin == ts_end) continue;

        std::string text;
        std::smatch last_match;
        for (auto it = ts_begin; it != ts_end; ++it) last_match = *it;
        std::size_t text_start = (std::size_t)last_match.position() + last_match.length();
        text = line.substr(text_start);
        std::size_t trim = text.find_first_not_of(" \t");
        if (trim != std::string::npos) text = text.substr(trim);

        for (auto it = ts_begin; it != ts_end; ++it) {
            const std::smatch& m = *it;
            int mins   = std::stoi(m[1].str());
            int secs   = std::stoi(m[2].str());
            std::string frac_str = m[3].str();
            while (frac_str.size() < 3) frac_str += "0";
            int ms = std::stoi(frac_str.substr(0, 3));
            double ts = mins * 60.0 + secs + ms / 1000.0;
            lines.push_back({ ts, text });
        }
    }

    if (lines.empty()) return false;

    // Apply offset to all timestamps
    if (offset_sec != 0.0) {
        for (auto& l : lines)
            l.time_sec = std::max(0.0, l.time_sec + offset_sec);
    }

    std::sort(lines.begin(), lines.end(),
        [](const LrcLine& a, const LrcLine& b) { return a.time_sec < b.time_sec; });

    loaded = true;
    return true;
}

int LrcData::currentLine(double pos_sec) const {
    if (lines.empty()) return -1;
    // Find the last line whose timestamp <= pos_sec
    int result = -1;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (lines[(size_t)i].time_sec <= pos_sec) result = i;
        else break;
    }
    return result;
}
