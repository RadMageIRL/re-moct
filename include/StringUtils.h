#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cctype>    // deinvertArtist's tolower
#include <cstdint>
#include <ctime>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ─── Thread-safe localtime ────────────────────────────────────────────────────
// std::localtime returns a pointer into a shared static buffer and is not
// thread-safe. This fills caller-owned storage instead. Returns false on failure.
inline bool localtimeSafe(std::time_t t, std::tm& out) {
#ifdef _WIN32
    return ::localtime_s(&out, &t) == 0;
#else
    return ::localtime_r(&t, &out) != nullptr;
#endif
}

// ─── Filesystem-safe path component ──────────────────────────────────────────
// One path COMPONENT (a file or directory name), never a full path: strips the
// characters Windows forbids (superset of POSIX) plus control bytes, and the
// trailing dots/spaces Windows silently drops. Empty in -> "Unknown" so a
// caller can never produce a nameless file. Moved verbatim from
// CDRipper::sanitizePath (stream-record R1) so StreamRecorder can share it
// without linking CDRipper's TU into tests; CDRipper::sanitizePath delegates.
inline std::string sanitizePathComponent(const std::string& s) {
    static const std::string ill = R"(\/:*?"<>|)";
    std::string o; o.reserve(s.size());
    for (unsigned char c : s)
        o += (c < 32 || ill.find((char)c) != std::string::npos) ? '_' : (char)c;
    while (!o.empty() && (o.back()=='.'||o.back()==' ')) o.pop_back();
    return o.empty() ? "Unknown" : o;
}

// ─── Article de-inversion ─────────────────────────────────────────────────────
// "Surname, The" -> "The Surname" for cleaner scrobble/tag metadata. Strict:
// only fires when the ENTIRE tail after the last comma is a bare article
// (the/a/an), so real comma-containing names ("Tyler, The Creator",
// "Earth, Wind & Fire") are left untouched. Deliberately does NOT transform
// "Lastname, Firstname" - too ambiguous to guess safely. Moved verbatim from
// the UIManager file-static (stream-record R2) so StreamRecorder's
// parseNowPlaying applies the same rule to filenames AND tags.
inline std::string deinvertArtist(const std::string& a) {
    auto pos = a.rfind(',');
    if (pos == std::string::npos || pos == 0) return a;
    std::string head = a.substr(0, pos);
    std::string tail = a.substr(pos + 1);
    size_t ts = tail.find_first_not_of(" \t");
    if (ts == std::string::npos) return a;             // nothing after the comma
    tail = tail.substr(ts);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) head.pop_back();
    if (head.empty()) return a;
    std::string low = tail;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (low == "the" || low == "a" || low == "an")
        return tail + " " + head;                      // preserve article casing
    return a;
}

// ─── Wide string conversion ───────────────────────────────────────────────────
// Windows: MultiByteToWideChar (UTF-16, the baseline — astral glyphs become
// surrogate pairs; the terminal folds them to '?', accepted in lessons.md).
// Linux: a straight codepoint decode further down (after utf8_next) — wchar_t
// is UTF-32 there, so the ncursesw wide-draw path gets one wchar_t per glyph.
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
// Synthetic CD path format: "<spec>:CD Track NN" — "F:CD Track 01" on Windows
// (single drive letter), "sr0:CD Track 01" on Linux (SG_IO device, slice 6).
// <spec> is the drive key: 1+ chars, no ':'. The "CD Track " tag is anchored
// right after the first colon and the suffix must be digits only, so a real path
// that merely contains the substring — e.g. "D:\Music\Best CD Tracks\01.flac" —
// never matches. Generalizing from the old index-1 colon (colon-delimited spec)
// is behavior-identical for every Windows single-char drive.
inline bool isCDTrackPath(const std::string& p) {
    auto colon = p.find(':');
    if (colon == std::string::npos || colon == 0) return false;  // need a non-empty spec
    size_t tag = colon + 1;
    if (p.compare(tag, 9, "CD Track ") != 0) return false;       // tag right after ':'
    size_t digits = tag + 9;
    if (digits >= p.size()) return false;                        // need >= 1 digit
    for (size_t i = digits; i < p.size(); ++i)
        if (p[i] < '0' || p[i] > '9') return false;              // digits-only track number
    return true;
}

// Internet-radio playlist entry: stored verbatim as an http(s):// URL (see
// PlaylistManager::addStream). Used to keep streams out of file-only machinery
// (the crossfade preloader) and out of saved playlists.
inline bool isStreamPath(const std::string& p) {
    return p.rfind("http://", 0) == 0 || p.rfind("https://", 0) == 0;
}

// Returns the 1-based track number encoded in a synthetic CD path
// ("<letter>:CD Track NN"), or -1 if `p` is not a CD-track path. Never throws —
// isCDTrackPath() guarantees a digits-only suffix, and the try/catch guards the
// pathological overflow case. Use this instead of a raw find("CD Track ")+stoi,
// which throws std::invalid_argument on real paths that merely contain the
// substring (and std::terminate when that happens on a worker thread).
inline int cdTrackNumber(const std::string& p) {
    if (!isCDTrackPath(p)) return -1;
    auto pos = p.find("CD Track ");
    try { return std::stoi(p.substr(pos + 9)); }   // digits begin right after "CD Track "
    catch (...) { return -1; }
}

// Parse drive spec ("D" on Windows, "sr0" on Linux) and track number from a CD
// path. Returns false if not a valid CD path. (Colon-delimited spec — identical
// to the old single-char parse for every Windows drive letter.)
inline bool parseCDPath(const std::string& p, std::string& drive, int& track_num) {
    if (!isCDTrackPath(p)) return false;
    drive = p.substr(0, p.find(':'));
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
            case 0x2032:                                        out += '\''; break;  // prime
            case 0x201C: case 0x201D: case 0x201E: case 0x201F: out += '"';  break;
            case 0x2033:                                        out += '"';  break;  // double prime
            // Dash / hyphen variants — all fold to ASCII '-'. Previously only
            // U+2013/U+2014 were handled, so look-alike dashes such as U+2010,
            // U+2011, U+2012, U+2015 and U+2212 fell through to the '?' catch-all
            // below (the "Prep-School" -> "Prep?School" bug).
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
            case 0x2015: case 0x2212: case 0x00AD:              out += '-'; break;
            case 0x2026: out += '.'; break;                                          // ellipsis
            case 0x2022: out += '*'; break;                                          // bullet
            // Space variants -> plain space; zero-width forms are dropped.
            case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A:
            case 0x202F: case 0x205F: case 0x3000:              out += ' '; break;
            case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: break;               // zero-width
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

// ─── Column-aware UTF-8 text measurement (terminal display width) ─────────────
// Self-contained: codepoint display columns independent of the platform wcwidth
// (UCRT's wchar_t is 16-bit and its wcwidth unreliable; this keeps the Linux port
// byte-identical too). Width is 0 for combining/zero-width marks, 2 for East-Asian
// wide glyphs and most emoji, 1 otherwise. These power column-correct truncation,
// padding and marquee scrolling feeding the wide ncurses API. They do NOT fold to
// ASCII — sanitizeForDisplay is the lossy fallback; these preserve the codepoints.

// Decode one UTF-8 codepoint at s[i], advancing i past it. Malformed bytes return
// the raw byte as a codepoint and advance by one, so callers always make progress.
inline uint32_t utf8_next(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; return c; }
    uint32_t cp; size_t n;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { ++i; return c; }                          // stray continuation / invalid lead
    if (i + n > s.size()) { ++i; return c; }          // truncated tail → lead as raw byte
    for (size_t j = 1; j < n; ++j) {
        unsigned char cc = (unsigned char)s[i + j];
        if ((cc & 0xC0) != 0x80) { ++i; return c; }   // bad continuation → lead as raw byte
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += n;
    return cp;
}

// Linux twin of utf8_to_wide (see the Windows version above): wchar_t is
// UTF-32 here, so one decoded codepoint per wchar_t — the ncursesw wide-draw
// path gets whole glyphs with no surrogate handling needed.
#ifndef _WIN32
inline std::wstring utf8_to_wide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) w += (wchar_t)utf8_next(s, i);
    return w;
}
#endif

// Display columns for one codepoint (Kuhn-style East-Asian width).
inline int cpWidth(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;          // C0/C1 controls
    if ((cp >= 0x0300 && cp <= 0x036F) ||                        // combining diacriticals
        (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05BD) ||
        (cp >= 0x0610 && cp <= 0x061A) ||
        (cp >= 0x064B && cp <= 0x065F) ||
        (cp == 0x0670) ||
        (cp >= 0x06D6 && cp <= 0x06DC) ||
        (cp >= 0x200B && cp <= 0x200F) ||                        // zero-width space..RLM
        (cp >= 0x202A && cp <= 0x202E) ||                        // bidi embeddings
        (cp >= 0x2060 && cp <= 0x2064) ||
        (cp == 0xFEFF))                                          // BOM / ZWNBSP
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||                        // Hangul Jamo
        (cp >= 0x2E80 && cp <= 0x303E) ||                        // CJK radicals..symbols
        (cp >= 0x3041 && cp <= 0x33FF) ||                        // Kana..CJK compat
        (cp >= 0x3400 && cp <= 0x4DBF) ||                        // CJK Ext-A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||                        // CJK Unified
        (cp >= 0xA000 && cp <= 0xA4CF) ||                        // Yi
        (cp >= 0xAC00 && cp <= 0xD7A3) ||                        // Hangul syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||                        // CJK compat ideographs
        (cp >= 0xFE10 && cp <= 0xFE19) ||                        // vertical forms
        (cp >= 0xFE30 && cp <= 0xFE6F) ||                        // CJK compat / small forms
        (cp >= 0xFF00 && cp <= 0xFF60) ||                        // fullwidth forms
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||                        // fullwidth signs
        (cp >= 0x1F000 && cp <= 0x1F02F) ||                      // mahjong / domino
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||                      // emoji & pictographs
        (cp >= 0x20000 && cp <= 0x3FFFD))                        // CJK Ext-B+
        return 2;
    return 1;                                                    // includes Nerd Font PUA (1 cell)
}

// Total display columns of a UTF-8 string.
inline int dispWidth(const std::string& s) {
    int w = 0; size_t i = 0;
    while (i < s.size()) w += cpWidth(utf8_next(s, i));
    return w;
}

// Number of UTF-8 codepoints in s (malformed bytes count as one each). Used to size
// a marquee cycle in codepoints when the caller drives its own scroll offset.
inline int cpCount(const std::string& s) {
    int n = 0; size_t i = 0;
    while (i < s.size()) { utf8_next(s, i); ++n; }
    return n;
}

// Truncate to at most `cols` display columns on a codepoint boundary. A wide glyph
// that would straddle the limit is dropped (result is then one column short).
inline std::string truncateToWidth(const std::string& s, int cols) {
    if (cols <= 0) return {};
    std::string out; int w = 0; size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        int cw = cpWidth(utf8_next(s, i));
        if (w + cw > cols) break;
        out.append(s, start, i - start);
        w += cw;
    }
    return out;
}

// Keep the RIGHTMOST `cols` display columns (codepoint-aligned). A wide glyph that
// would straddle the left cut is dropped (result is then one column short). Used by
// the tag editor's live edit field, which shows the tail of an overflowing value.
inline std::string truncateToWidthRight(const std::string& s, int cols) {
    if (cols <= 0) return {};
    if (dispWidth(s) <= cols) return s;
    std::vector<std::pair<size_t,size_t>> cps;   // (byte offset, byte length) per codepoint
    std::vector<int> w;
    { size_t i = 0; while (i < s.size()) { size_t a = i; int cw = cpWidth(utf8_next(s, i));
                                           cps.push_back({a, i - a}); w.push_back(cw); } }
    int acc = 0; size_t start = s.size();
    for (int k = (int)cps.size() - 1; k >= 0; --k) {
        if (acc + w[(size_t)k] > cols) break;
        acc += w[(size_t)k];
        start = cps[(size_t)k].first;
    }
    return s.substr(start);
}

// Truncate to `cols`, then pad with spaces to EXACTLY `cols` display columns.
inline std::string padToWidth(const std::string& s, int cols) {
    if (cols <= 0) return {};
    std::string out = truncateToWidth(s, cols);
    int w = dispWidth(out);
    if (w < cols) out.append((size_t)(cols - w), ' ');
    return out;
}

// Column-aware marquee: exactly `cols` display columns of `s` starting at codepoint
// scroll position `off`, wrapping past a 3-space gap, never splitting a codepoint.
// If `s` already fits in `cols`, it is simply padded. A wide glyph landing on the
// right edge is rendered as a space so the field width stays exact.
inline std::string scrollToWidth(const std::string& s, int cols, int off) {
    if (cols <= 0) return {};
    if (dispWidth(s) <= cols) return padToWidth(s, cols);
    const std::string ring = s + "   ";
    std::vector<std::pair<size_t,size_t>> cps;   // (byte offset, byte length) per codepoint
    std::vector<int> cw;
    { size_t i = 0; while (i < ring.size()) { size_t a = i; int w = cpWidth(utf8_next(ring, i));
                                              cps.push_back({a, i - a}); cw.push_back(w); } }
    const int ncp = (int)cps.size();
    if (ncp == 0) return std::string((size_t)cols, ' ');
    const int start = ((off % ncp) + ncp) % ncp;
    std::string out; int w = 0, idx = start, stepped = 0;
    while (w < cols && stepped < ncp) {
        if (w + cw[(size_t)idx] > cols) { out += ' '; ++w; }     // wide glyph on right edge
        else { out.append(ring, cps[(size_t)idx].first, cps[(size_t)idx].second); w += cw[(size_t)idx]; }
        idx = (idx + 1) % ncp; ++stepped;
    }
    if (w < cols) out.append((size_t)(cols - w), ' ');
    return out;
}
