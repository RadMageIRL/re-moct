#pragma once
#include <string>
#include <cstdint>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ─── Wide string conversion ───────────────────────────────────────────────────
#ifdef _WIN32
inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}
#endif

// ─── CD track path detection ──────────────────────────────────────────────────
// Fake CD path format: "D:CD Track 01"
inline bool isCDTrackPath(const std::string& p) {
    return p.size() > 2 && p[1] == ':' &&
           p.find("CD Track ") != std::string::npos;
}

// Parse drive letter and track number from a CD path
// Returns false if not a valid CD path
inline bool parseCDPath(const std::string& p, std::string& drive, int& track_num) {
    if (!isCDTrackPath(p)) return false;
    drive = p.substr(0, 1);
    auto pos = p.find("CD Track ");
    try { track_num = std::stoi(p.substr(pos + 9)); }
    catch (...) { return false; }
    return true;
}

// Replace common Unicode punctuation with ASCII equivalents for terminal display
inline std::string sanitizeForDisplay(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += s[i++]; continue; }
        uint32_t cp = 0; size_t n = 1;
        if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { out += '?'; i++; continue; }
        for (size_t j = 1; j < n && i+j < s.size(); ++j)
            cp = (cp << 6) | ((unsigned char)s[i+j] & 0x3F);
        switch (cp) {
            case 0x2018: case 0x2019: case 0x201A: case 0x201B: out += '\''; break;
            case 0x201C: case 0x201D: case 0x201E: case 0x201F: out += '"';  break;
            case 0x2013: case 0x2014: out += '-'; break;
            case 0x2026: out += '.'; break;
            case 0x0153: out += "oe"; break;
            case 0x0152: out += "OE"; break;
            case 0x00E6: out += "ae"; break;
            case 0x00C6: out += "AE"; break;
            case 0x00E9: case 0x00E8: case 0x00EA: case 0x00EB: out += 'e'; break;
            case 0x00E0: case 0x00E2: out += 'a'; break;
            case 0x00E7: out += 'c'; break;
            case 0x00EE: case 0x00EF: out += 'i'; break;
            case 0x00F4: case 0x00F8: out += 'o'; break;
            case 0x00FC: case 0x00FB: case 0x00F9: out += 'u'; break;
            case 0x00C9: case 0x00C8: out += 'E'; break;
            case 0x00C0: out += 'A'; break;
            default:
                if (cp >= 0x20 && cp < 0x80) out += (char)cp;
                else if (n == 2) { out += s[i]; out += s[i+1]; }
                else out += '?';
                break;
        }
        i += n;
    }
    return out;
}
