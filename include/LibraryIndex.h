#pragma once

// ─── Library metadata index — the pure model, format, and hierarchy queries ───
//
// Library slice 1. A standalone, PURE unit: an index in, its serialised text
// out, and that text back in again. No I/O, no filesystem, no globals, no
// throw. The scanner that fills it (TagLib, directory walk, mtime
// revalidation) is slice 2; the [Library] section that draws it is slices 3-4.
// Nothing here knows either exists.
//
// WHY A HAND-ROLLED FORMAT. The config file already persists tab-delimited
// records, but it does so LOSSILY on purpose: Config.cpp folds tabs to spaces
// and strips CR/LF so a "podcast=<url>\t<title>\t<art>" line can never
// mis-split. That is right for a feed title, which is a display string. It is
// wrong here, because this file's first field is a PATH — identity, not
// decoration. A folded path names a file that does not exist, and it fails on
// exactly the tracks nobody checks. Tabs and newlines are both legal in POSIX
// filenames, so escaping is a correctness requirement rather than tidiness.
// Every string field is therefore escaped losslessly and round-trips byte-exact.
//
// PATHS ARE STRINGS, START TO FINISH. Constructing a std::filesystem::path from
// a std::string THROWS on Windows for INVALID UTF-8 — and even fs::exists(str, ec)
// throws, because the conversion runs before the error code applies. That took down
// the podcast list draw in podcast slice 5, which built a path out of feed TITLE text.
//
// CORRECTED 2026-07-26, measured on both toolchains: the trigger is INVALID UTF-8, not
// "non-ASCII", and libstdc++ on Windows decodes a narrow path as UTF-8 rather than as
// the ANSI codepage. Valid UTF-8 of any kind — accents, smart quotes, CJK, 4-byte
// emoji — constructs fine and round-trips byte-exact. The earlier wording here said
// "any byte the ANSI codepage cannot map", which was wrong and could not explain why
// UIManager's own directory_iterator has always worked over this collection's 137
// non-ASCII paths.
//
// The operative rule: paths that come FROM THE OS are safe; strings built from tag or
// feed text are not. This unit never converts a path to anything either way. Where a
// later slice must actually open a file, port::fopenUtf8 (_wfopen over utf8_to_wide) is
// the one sanctioned route.
//
// DEFENSIVE CONTRACT: never crash, never throw, never hang. A malformed file
// degrades to fewer records — ultimately to an empty index, which the caller
// reports as an honest "no library" rather than an error. One linear pass over
// the text and one sort per query; nothing quadratic.
//
// DEPENDENCIES: none beyond the standard library, deliberately. The test
// compiles this header and links nothing.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>     // slice 8: compilation detection, and the dedup-first queries
#include <unordered_set>
#include <vector>

namespace libidx {

// ── Format identity ─────────────────────────────────────────────────────────
// A version mismatch DISCARDS and rescans. There is no migration path and there
// is deliberately no plan for one: the index is a cache, entirely rebuildable
// from the files it describes, so carrying migration code would be pure cost.
//
// ── VERSION 1 DESCRIBES TWO SLIGHTLY DIFFERENT FILES, AND THAT IS DELIBERATE ──
//
// Slice 11 made the library index several roots instead of one. The file shape is:
//
//     remoct-library-index<TAB>1        line 0: magic + version, exactly 2 fields
//     root<TAB><path>                   line 1: the first root, exactly 2 fields
//     root<TAB><path>                   OPTIONAL, zero or more further root lines
//     <12 tab-separated fields>         one record per line, to the end
//
// THE RULE: a reader takes the RUN of `root` lines beginning at line 1. One root
// line is the shipped single-root file; two or more is a multi-root file. Both are
// version 1, and the version was deliberately NOT bumped.
//
// Why not: the version field means "discard and rescan", and nothing here requires
// discarding. A single-root file read by this parser yields a list of one and needs
// no rescan at all, so bumping would fire an incompatibility mechanism in a case
// that is compatible - and charge every existing user a full rescan (15.8 s for
// 2,155 files, measured at slice 2) to arrive at the index they already had.
//
// THE OTHER DIRECTION IS SAFE TOO, and this is the part worth writing down rather
// than leaving for someone to rediscover: an OLDER reader takes line 1 as the only
// root, then meets the second `root` line in its record loop, finds 2 fields where
// it wants 12, and counts it as a skipped record. It still loads EVERY record,
// including those under the roots it does not know about. No crash and no data loss.
// (`skipped_records` is not surfaced to the user anywhere - both loadIndexFile call
// sites omit the optional out-parameter - so that costs not even a warning.)
//
// The one accepted exposure: an older build believes in one root, so its next
// RESCAN walks only that root and drops the other roots' records. Recoverable by
// upgrading and rescanning.
inline constexpr const char* kMagic         = "remoct-library-index";
inline constexpr int         kFormatVersion = 1;

// Field count of one record line. A line that does not have exactly this many
// fields is skipped and counted, never guessed at.
inline constexpr std::size_t kFieldCount = 12;

// ── One track ───────────────────────────────────────────────────────────────
// Tag text is stored RAW. foldForDisplay runs at draw time in slices 3-4,
// not here: folding on the way in would be lossy, and it would also be
// insufficient (foldForDisplay passes ASCII control bytes straight through,
// as the chapters slice discovered).
//
// That split is unchanged, and the fold now takes far less with it: it
// normalizes typography and passes language through, so what is lost by folding
// on the way in is smaller than it was but is still loss. Raw in, fold at the
// draw. The same rule LocalFileSource and StreamSource follow for the scrobblers.
//
// Play count is deliberately ABSENT. Config already owns TrackStats
// {play_count, last_played} keyed by path; duplicating it here would create two
// sources of truth that drift apart. Slice 7's most-played and recently-played
// views join on `path` at query time.
struct LibraryTrack {
    std::string path;          // absolute; IDENTITY — must round-trip byte-exact
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string title;
    std::string genre;
    int32_t     track_no     = 0;
    int32_t     disc_no      = 0;
    int32_t     year         = 0;
    int32_t     duration_sec = 0;
    // The slice-2 revalidation key. A file whose path, mtime and size all match
    // its record is not re-read; everything else is. These two fields are the
    // entire reason that comparison is possible, so they round-trip exactly.
    int64_t     mtime        = 0;
    uint64_t    size         = 0;
};

struct LibraryIndex {
    // The music roots this index was built from, in configured order (slice 11).
    // A vector rather than a string so the compiler finds every reader - the same
    // reason slice 4 made the level an enum instead of adding a bool. Empty means
    // "the OS music folder", which is what an empty `root` meant before it.
    std::vector<std::string>  roots;
    std::vector<LibraryTrack> tracks;
    // Slice 8: album names (folded) judged to be compilations. DERIVED at load from
    // fields already on disk, never serialised - so this is not a format change and
    // every shipped index stays readable. Rebuilt by rebuildCompilations() after a
    // parse and after a scan, which are the only two ways an index comes to exist.
    std::unordered_set<std::string> compilations;
};

struct ParseResult {
    LibraryIndex index;
    bool         ok = false;        // header understood; index is usable
    std::size_t  skipped_records = 0;  // malformed lines dropped, honestly counted
};

namespace detail {

// ── Escaping ────────────────────────────────────────────────────────────────
// Backslash escapes for every byte that would otherwise break the line/field
// framing, plus NUL so a field is always safe to hand to C-string code later.
// Applied to EVERY string field including the path.
inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\0': out += "\\0";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Unknown escape sequences degrade to the escaped character itself, and a
// trailing lone backslash is dropped. Neither can arise from escape() above —
// they only appear in a hand-edited or corrupted file, and the contract there is
// "never throw", not "be clever".
inline std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') { out += s[i]; continue; }
        if (i + 1 >= s.size()) break;          // trailing backslash: drop it
        switch (s[++i]) {
            case '\\': out += '\\'; break;
            case 't':  out += '\t'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case '0':  out += '\0'; break;
            default:   out += s[i]; break;     // unknown: keep the character
        }
    }
    return out;
}

// ── Defensive numeric parsing ───────────────────────────────────────────────
// No exceptions, no strtol errno dance, and an explicit overflow guard: junk in
// a numeric field fails the record rather than silently reading as zero, which
// would put a track at year 0 instead of admitting the line is broken.
// Accumulates in UNSIGNED with a sign-dependent limit. The signed range is
// asymmetric - INT64_MIN's magnitude is one MORE than INT64_MAX - so a signed
// accumulator rejects "-9223372036854775808", a value std::to_string produces
// and this format therefore has to read back. The negation is built by modular
// arithmetic rather than unary minus, which would be undefined at exactly that
// value.
inline bool parseI64(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; i = 1; if (s.size() == 1) return false; }
    const uint64_t limit = neg ? static_cast<uint64_t>(INT64_MAX) + 1u
                               : static_cast<uint64_t>(INT64_MAX);
    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        uint64_t d = static_cast<uint64_t>(s[i] - '0');
        if (v > (limit - d) / 10) return false;          // overflow
        v = v * 10 + d;
    }
    out = neg ? static_cast<int64_t>(0u - v) : static_cast<int64_t>(v);
    return true;
}

inline bool parseU64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;     // overflow
        v = v * 10 + d;
    }
    out = v;
    return true;
}

inline bool parseI32(const std::string& s, int32_t& out) {
    int64_t v = 0;
    if (!parseI64(s, v)) return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    out = static_cast<int32_t>(v);
    return true;
}

// Split on RAW tabs. Safe to do before unescaping, and only before: an escaped
// tab is the two characters '\' 't' at this stage, so a separator is always a
// real separator. Doing it the other way round would re-split fields open.
inline std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            out.emplace_back(line, start, i - start);
            start = i + 1;
        }
    }
    return out;
}

// ASCII-only case fold for comparison. Deliberately not locale-aware: the
// application runs under setlocale(LC_ALL, "") with a CP1252 narrow encoding, so
// a locale-sensitive fold would behave differently per machine and make list
// order machine-dependent. Non-ASCII compares by byte, which is stable
// everywhere. Refining that is slice 7's polish, not correctness.
inline int icmp(const std::string& a, const std::string& b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca < 0x80) ca = static_cast<unsigned char>(std::tolower(ca));
        if (cb < 0x80) cb = static_cast<unsigned char>(std::tolower(cb));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

// Sort case-insensitively, but break ties by raw bytes so the order is TOTAL and
// therefore identical across runs and platforms. A UI list that reorders itself
// between launches reads as a bug.
inline bool iless(const std::string& a, const std::string& b) {
    const int c = icmp(a, b);
    return c != 0 ? c < 0 : a < b;
}

// Sort, then collapse case-variant duplicates keeping the first spelling in
// sorted order — deterministic, and "The Beatles" / "the beatles" become one
// row rather than two.
// Filename stem, by BYTES, with no fs::path anywhere - so this header stays free of
// <filesystem> and therefore of the whole invalid-UTF-8 throw hazard. libnav::pathStem
// delegates here rather than keeping a second copy.
inline std::string pathStemOf(const std::string& p) {
    const std::size_t slash = p.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot != 0) base.erase(dot);
    return base;
}

// ── ASCII folding, for SEARCH ONLY (slice 7) ─────────────────────────────────
// Folds a UTF-8 string to lowercase ASCII, doing case AND DIACRITICS in one pass,
// which is what lets a typed "bjork" match a tag reading "BJÖRK". Case folding alone
// would give "björk" and still miss.
//
// SCOPE, stated because it is a real limit and not an oversight: Latin-1 Supplement
// (U+00C0-U+00FF) and Latin Extended-A (U+0100-U+017F), which is the accented Latin a
// Western music collection actually contains. Anything else - CJK, Cyrillic, Greek,
// emoji - passes through unchanged and matches byte-exactly. This is a Latin fold, not
// a Unicode collation, and a full case table is not worth carrying for one feature.
//
// Not to be confused with foldForDisplay, which folds a PARTIAL set of accents for
// DRAWING and is deliberately left alone: display keeps its accents, matching does not.
inline const char* foldLatin1(uint32_t cp) {
    switch (cp) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return "a";
        case 0xC6: case 0xE6: return "ae";
        case 0xC7: case 0xE7: return "c";
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
        case 0xD0: case 0xF0: return "d";
        case 0xD1: case 0xF1: return "n";
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return "o";
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: return "u";
        case 0xDD: case 0xFD: case 0xFF: return "y";
        case 0xDE: case 0xFE: return "th";
        case 0xDF: return "ss";
        default: return nullptr;      // includes U+00D7 and U+00F7, which are not letters
    }
}
// Latin Extended-A is base-letter RUNS, so it folds by range rather than by 128 cases.
inline const char* foldLatinExtA(uint32_t cp) {
    if (cp >= 0x0100 && cp <= 0x0105) return "a";
    if (cp >= 0x0106 && cp <= 0x010D) return "c";
    if (cp >= 0x010E && cp <= 0x0111) return "d";
    if (cp >= 0x0112 && cp <= 0x011B) return "e";
    if (cp >= 0x011C && cp <= 0x0123) return "g";
    if (cp >= 0x0124 && cp <= 0x0127) return "h";
    if (cp >= 0x0128 && cp <= 0x0131) return "i";
    if (cp == 0x0132 || cp == 0x0133) return "ij";
    if (cp >= 0x0134 && cp <= 0x0135) return "j";
    if (cp >= 0x0136 && cp <= 0x0138) return "k";
    if (cp >= 0x0139 && cp <= 0x0142) return "l";
    if (cp >= 0x0143 && cp <= 0x014B) return "n";
    if (cp >= 0x014C && cp <= 0x0151) return "o";
    if (cp == 0x0152 || cp == 0x0153) return "oe";
    if (cp >= 0x0154 && cp <= 0x0159) return "r";
    if (cp >= 0x015A && cp <= 0x0161) return "s";
    if (cp >= 0x0162 && cp <= 0x0167) return "t";
    if (cp >= 0x0168 && cp <= 0x0173) return "u";
    if (cp >= 0x0174 && cp <= 0x0175) return "w";
    if (cp >= 0x0176 && cp <= 0x0178) return "y";
    if (cp >= 0x0179 && cp <= 0x017E) return "z";
    return nullptr;
}

// Fold into a caller-owned buffer, so a search loop reuses one allocation instead of
// making one per field per record.
inline void foldAsciiInto(const std::string& s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {                                   // ASCII: lowercase in place
            out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                          : static_cast<char>(c);
            ++i;
            continue;
        }
        uint32_t cp = 0; std::size_t n = 1;
        if      ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; n = 4; }
        else { out += static_cast<char>(c); ++i; continue; }   // invalid lead: keep byte
        if (i + n > s.size()) { out += static_cast<char>(c); ++i; continue; }  // truncated
        for (std::size_t j = 1; j < n; ++j)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3Fu);
        const char* f = (cp <= 0xFFu) ? foldLatin1(cp) : foldLatinExtA(cp);
        if (f) out += f;
        else   out.append(s, i, n);                       // outside the fold: verbatim
        i += n;
    }
}
inline std::string foldAscii(const std::string& s) {
    std::string o; foldAsciiInto(s, o); return o;
}

inline void sortUniqueCI(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end(), iless);
    v.erase(std::unique(v.begin(), v.end(),
                        [](const std::string& a, const std::string& b) {
                            return icmp(a, b) == 0;
                        }),
            v.end());
}

// ── Genre splitting (slice 10) ──────────────────────────────────────────────
//
// One track can carry several genres in one tag, and on the reference collection
// that is the ENTIRE shape of the mess: 35 distinct raw strings over 1,478 tagged
// records, with ZERO case variants. The separators are `/` and `;`.
//
// COMMAS AND HYPHENS ARE DELIBERATELY NOT SEPARATORS, and each has a real record
// behind it:
//   `Rock / Folk, World, & Country`  splitting commas leaves a dangling `& Country`
//   `Hip-Hop / Rap`                  splitting hyphens leaves `Hip` and `Hop`
// So `Pop-Rock` (12 records) stays its own genre, and `Post Punk` (5) and
// `Post-Punk` (2) stay two. Both are STATED IMPERFECTIONS rather than oversights:
// any rule that merged them would break `Hip-Hop`, which is 49 records and seven
// times as common. `R&B` (38) and `Stage & Screen` (12) survive for the same
// reason - `&` is not a separator either.
//
// Parts are trimmed, so the spacing variants in the wild (`Pop/Rock`,
// `Pop / Rock`, `Rock / Funk  /  Soul` with doubled spaces) all land on the same
// genres without a special case. Empty parts are dropped, so a trailing separator
// contributes nothing.
//
// MEASURED: on the real 2,157-record index this turns 35 raw strings into 27
// genres, and rolls 748 raw `Rock` up to 979.
inline std::vector<std::string> splitGenres(const std::string& g) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= g.size()) {
        std::size_t j = i;
        while (j < g.size() && g[j] != '/' && g[j] != ';') ++j;
        std::size_t b = i, e = j;
        while (b < e && (unsigned char)g[b] <= ' ') ++b;      // trim left
        while (e > b && (unsigned char)g[e - 1] <= ' ') --e;  // trim right
        if (e > b) out.push_back(g.substr(b, e - b));
        if (j >= g.size()) break;
        i = j + 1;
    }
    return out;
}

// ── The genre GROUPING key (slice 16) ───────────────────────────────────────
//
// `Post Punk` and `Post-Punk` are one genre written two ways. LIB-S10 recorded this
// as not-fixed, and it was right about the fix it considered: SPLITTING on hyphens
// breaks `Hip-Hop`, which is 49 records against Post-Punk's 2. But splitting is not
// the only operation available. Grouping on a key that ignores punctuation joins the
// two variants WITHOUT splitting anything, so `Hip-Hop` stays exactly one genre of
// exactly 49 records - nothing is being cut in half.
//
// MEASURED over every one of the 30 post-split genre values in the real collection:
// 29 rows out, and EXACTLY ONE group merges - `postpunk`, 5 + 2 = 7 records. The two
// values that a careless rule would eat both survive:
//   Hip-Hop              -> hiphop            49, alone
//   Folk, World, & Country -> folkworldcountry 41, alone (it is NOT `folk`, which is 3)
//   Pop-Rock             -> poprock           12, alone (neither `pop` nor `rock`)
//
// NON-ASCII BYTES PASS THROUGH UNTOUCHED and are never case-folded. Folding above
// ASCII needs a Unicode table this program does not have and should not grow, and a
// wrong fold merges genres that are not the same. So two values differing only in a
// high byte stay two rows - failing toward "too many rows", which is the recoverable
// direction. Same rule, same reason, as foldPathKey.
//
// THIS IS A DISPLAY AND COMPARE KEY. The tag on disk is not touched, the index file
// is not touched, and nothing normalised is ever stored - a rescan produces a
// byte-identical library.idx. Exactly the read-side shape foldPathKey has.
inline std::string genreKey(const std::string& g) {
    std::string o;
    o.reserve(g.size());
    for (char c : g) {
        const unsigned char u = (unsigned char)c;
        if (u > 0x7F)                  o.push_back(c);                    // never touched
        else if (u >= 'A' && u <= 'Z') o.push_back((char)(u - 'A' + 'a'));
        else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) o.push_back(c);
        // every other ASCII byte - space, hyphen, ampersand, comma, dot - is dropped
    }
    return o;
}

// Does this track carry `genre` among its (split) genres?
//
// SLICE 16: compares on genreKey, not on icmp. This is not optional and it is not a
// tidiness change. genres() now shows `Post Punk` as one row of 7, and this function
// is what the artist and album levels beneath it filter with - so an exact compare
// here would open that row onto the 5 tracks tagged `Post Punk` and hide the 2 tagged
// `Post-Punk`. A list and a filter that disagree is a row that lies about its own
// contents.
//
// One key, three readers: genres(), artists(idx, genre), albumsForGenre. No second
// rule, for the same reason there is exactly one foldPathKey.
inline bool trackHasGenre(const std::string& tag, const std::string& genre) {
    if (genre.empty()) return false;
    const std::string key = genreKey(genre);
    if (key.empty()) return false;      // a genre of pure punctuation matches nothing
    for (const std::string& g : splitGenres(tag))
        if (genreKey(g) == key) return true;
    return false;
}

// ── The play-stat join key (slice 10) ───────────────────────────────────────
//
// MEASURED ON DOS'S REAL FILES, and this is why the function exists rather than a
// bare string compare. Config::recordPlay keys track_stats on whatever path the
// PLAYLIST ENTRY held at play time, and those come from the folder browser, a
// loaded .m3u, a favourite, a bookmark - which do not agree on case. Of 295 real
// stat entries, 241 have a lowercase drive letter and 54 an uppercase one, while
// every index path comes from the OS walk and is uppercase.
//
//   byte-exact join  -> 20 of 295 matched, "never played" reads 99.1%
//   case-folded join -> 236 of 295 matched, "never played" reads 89.1%
//
// BOTH of those would have passed a glance, which is the LIB-S8 fixture lesson
// landing on the product instead of a fixture.
//
// WINDOWS ONLY. NTFS is case-insensitive, so two paths differing only in case name
// one file and folding is what "same file" MEANS there. Linux paths are
// case-sensitive and two such paths may be two different files, so folding there
// would merge them and invent play counts. This is a filesystem property, not a
// preference.
//
// Separators are normalised too: the same file reached as `a/b` and `a\b` is one
// file on Windows.
//
// This is a READ-SIDE fold. It does not touch the stored keys - normalising those
// and merging the 17 real split pairs mutates persisted user data and is LIB-S13.
inline std::string foldPathKey(const std::string& p) {
#ifdef _WIN32
    std::string o;
    o.reserve(p.size());
    for (char c : p) {
        unsigned char u = (unsigned char)c;
        if (c == '/') { o.push_back('\\'); continue; }
        o.push_back((u < 0x80 && u >= 'A' && u <= 'Z') ? (char)(u - 'A' + 'a') : c);
    }
    return o;
#else
    return p;
#endif
}

// ── Root paths (slice 11) ───────────────────────────────────────────────────
//
// Both of these sit beside foldPathKey and USE it rather than rolling a second
// comparison, because "same path" must mean one thing. That helper arrived in
// slice 10 for the play-stat join and already carries the platform rule:
// case-and-separator folded on Windows because NTFS is case-insensitive, byte-exact
// on Linux because two paths differing in case may be two different files.

// A root with trailing separators removed, so "D:\Music\" and "D:\Music" are the
// same root. A bare drive root ("C:\", "/") keeps its separator - stripping it
// would leave "C:", which names the drive's CURRENT DIRECTORY rather than its root.
//
// WHICH CHARACTERS COUNT AS SEPARATORS IS PLATFORM-DEPENDENT, and getting that
// wrong is not cosmetic. On Windows both '/' and '\' separate. **On Linux a
// backslash is an ORDINARY FILENAME CHARACTER**, so a directory really can be named
// `weird\` and stripping the backslash would name a different directory - or
// nothing. Caught by the Linux gate, which is exactly what a platform-split helper
// needs one for.
inline bool isSep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

inline std::string normaliseRoot(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && isSep(s.back())) {
#ifdef _WIN32
        if (s.size() == 3 && s[1] == ':') break;    // "C:\" - keep it
#endif
        s.pop_back();
    }
    return s;
}

// Is `path` inside `root`, or equal to it?
//
// THE BOUNDARY CHECK IS THE WHOLE POINT. A plain prefix compare makes "C:\Music" a
// parent of "C:\Musicology", which would reject a legitimate second root as a
// duplicate and - worse, once carry-forward uses this - hand one root's records to
// another. So the prefix must end exactly at a separator.
inline bool isPathUnder(const std::string& path, const std::string& root) {
    const std::string p = foldPathKey(normaliseRoot(path));
    const std::string r = foldPathKey(normaliseRoot(root));
    if (r.empty() || p.size() < r.size()) return false;
    if (p.compare(0, r.size(), r) != 0) return false;
    if (p.size() == r.size()) return true;                  // equal
    // A root that already ends in a separator ("C:\", "/") needs no extra one.
    if (isSep(r.back())) return true;
    return isSep(p[r.size()]);
}

} // namespace detail

// ── The grouping seam ───────────────────────────────────────────────────────
// SLICE 7 OWNS THE RULE; SLICE 1 OWNS THE SEAM. Every hierarchy query below
// routes through this one function, so compilation handling — Various Artists
// recognition, albums whose tracks disagree on artist — changes here and
// nowhere else. Shipping no rule at all was considered and rejected: it would
// not defer the decision, it would scatter it into slices 3-4 as ad-hoc UI
// logic that slice 7 would then have to hunt down.
//
// The simplest defensible rule: album-artist when the tag carries one, else
// artist. That already keeps a properly-tagged compilation together.
//
// SLICE 8 KEPT THIS FORM and added the index-aware one below. This one is what a
// caller with no index in hand uses - the scanner, building records one file at a
// time - and its behaviour is unchanged.
inline const std::string& groupingArtist(const LibraryTrack& t) {
    return t.album_artist.empty() ? t.artist : t.album_artist;
}

// ── Compilations (slice 8) ──────────────────────────────────────────────────
//
// The name a compilation groups under. A real string rather than a sentinel, because
// it is drawn as an artist row, sorted with the others, and searched like any other
// text - it is not a special case anywhere above this file.
inline constexpr const char* kVariousArtists = "Various Artists";

// Is this album-artist tag one of the "not really an artist" markers?
//
// Matched through foldAscii, so case and accents do not matter. The list is what the
// real collection and the common taggers actually write - MEASURED: of ten genuine
// compilations in the reference collection, EIGHT carry no album-artist at all, and
// the two that do say "Various" and "Soundtrack" rather than "Various Artists". So
// this test exists for the minority; the empty case below carries most of the weight.
inline bool isVariousish(const std::string& album_artist) {
    if (album_artist.empty()) return true;
    const std::string f = detail::foldAscii(album_artist);
    return f == "various" || f == "various artists" || f == "va" ||
           f == "compilation" || f == "compilations" ||
           f == "soundtrack" || f == "ost" || f == "original soundtrack" ||
           f == "original motion picture soundtrack";
}

// The minimum number of distinct track artists before an album with no usable
// album-artist is called a compilation.
//
// MEASURED, not tuned: on the reference collection the flagged set is IDENTICAL for
// every threshold from 2 to 5, because each genuine compilation has at least 9 distinct
// artists and no empty-album-artist album has 2. So the album-artist test is the
// discriminator and this is a backstop. 3 is the safe middle - low enough to catch a
// genuine three-way split release, high enough that two unrelated albums sharing a name
// and both missing album-artist cannot collide into one.
inline constexpr std::size_t kCompilationMinArtists = 3;

// Rebuild the derived set of compilation album names.
//
// DERIVED, NEVER STORED: computed from fields already on disk, so there is NO INDEX
// FORMAT CHANGE and no shipped library.idx is invalidated. Called after a parse and
// after a scan, which are the only two ways an index comes into existence.
//
// THE RULE, and the false positives it is built to reject:
//   1. every non-empty album-artist on the album is various-ish (or there are none), AND
//   2. at least kCompilationMinArtists distinct track artists.
//
// Test 1 is what saves the guest-artist album. The reference collection contains both
// shapes this is measured against: "Plastic Beach" (16 tracks, SIX credited artists,
// album-artist "Gorillaz") and "The Ultimate Collection" (21 tracks, three artists,
// album-artist "Jackson 5, The"). Neither is a compilation, and an artist-count
// threshold on its own would call both one.
//
// ACCEPTED FALSE NEGATIVE, stated rather than hidden: a genuine compilation whose
// album-artist names one of its contributors is not detected. There is no signal left -
// the tags say it is that artist's album - and inventing one would need an online
// lookup, which is a campaign non-goal.
inline void rebuildCompilations(LibraryIndex& idx) {
    idx.compilations.clear();
    // Album name -> (distinct artists, every album-artist various-ish so far).
    struct Acc { std::vector<std::string> artists; bool aa_ok = true; };
    std::unordered_map<std::string, Acc> by_album;
    by_album.reserve(idx.tracks.size() / 8 + 16);

    for (const LibraryTrack& t : idx.tracks) {
        if (t.album.empty()) continue;              // no album, nothing to group
        Acc& a = by_album[detail::foldAscii(t.album)];
        if (!isVariousish(t.album_artist)) a.aa_ok = false;
        if (!t.artist.empty()) {
            const std::string f = detail::foldAscii(t.artist);
            bool seen = false;
            for (const std::string& s : a.artists) if (s == f) { seen = true; break; }
            if (!seen) a.artists.push_back(f);
        }
    }
    for (const LibraryTrack& t : idx.tracks) {
        if (t.album.empty()) continue;
        const Acc& a = by_album[detail::foldAscii(t.album)];
        if (a.aa_ok && a.artists.size() >= kCompilationMinArtists)
            idx.compilations.insert(detail::foldAscii(t.album));
    }
}

// Is this track on a compilation?
inline bool isCompilation(const LibraryIndex& idx, const LibraryTrack& t) {
    return !t.album.empty() &&
           idx.compilations.find(detail::foldAscii(t.album)) != idx.compilations.end();
}

// THE SEAM, index-aware. Every hierarchy query routes through this, which is why one
// change here makes artists, albums, tracks AND search compilation-aware at once and
// makes it impossible for them to disagree.
inline const std::string& groupingArtist(const LibraryIndex& idx, const LibraryTrack& t) {
    if (isCompilation(idx, t)) {
        static const std::string various = kVariousArtists;
        return various;
    }
    return groupingArtist(t);
}

// ── Serialise ───────────────────────────────────────────────────────────────
inline std::string serialiseIndex(const LibraryIndex& idx) {
    std::string out;
    // ~160 bytes/record is a deliberate over-estimate: one reserve beats the
    // handful of reallocations a 100k-record index would otherwise take.
    out.reserve(64 + idx.tracks.size() * 160);

    out += kMagic;
    out += '\t';
    out += std::to_string(kFormatVersion);
    out += '\n';
    // One `root` line per root. ALWAYS AT LEAST ONE, even when there are no roots
    // configured: line 1 is structural - parseIndex rejects a file without it - and
    // an empty value is exactly what a single unconfigured root wrote before slice 11,
    // so a no-roots index is byte-identical to the file that shipped.
    if (idx.roots.empty()) {
        out += "root\t";
        out += '\n';
    } else {
        for (const std::string& r : idx.roots) {
            out += "root\t";
            out += detail::escape(r);
            out += '\n';
        }
    }

    for (const auto& t : idx.tracks) {
        out += detail::escape(t.path);         out += '\t';
        out += detail::escape(t.artist);       out += '\t';
        out += detail::escape(t.album);        out += '\t';
        out += detail::escape(t.album_artist); out += '\t';
        out += detail::escape(t.title);        out += '\t';
        out += detail::escape(t.genre);        out += '\t';
        out += std::to_string(t.track_no);     out += '\t';
        out += std::to_string(t.disc_no);      out += '\t';
        out += std::to_string(t.year);         out += '\t';
        out += std::to_string(t.duration_sec); out += '\t';
        out += std::to_string(t.mtime);        out += '\t';
        out += std::to_string(t.size);         out += '\n';
    }
    return out;
}

// ── Parse ───────────────────────────────────────────────────────────────────
// ok == false means the header was not understood (empty text, wrong magic,
// unsupported version, missing root line) and the index is empty. A true return
// with skipped_records > 0 means the file was readable but partly corrupt —
// which is still usable, and is reported rather than hidden.
//
// No record cap. A library legitimately has six figures of tracks, and the one
// expensive thing here (allocation) is bounded by the input the caller already
// holds in memory.
inline ParseResult parseIndex(const std::string& text) {
    ParseResult res;

    // Split into lines, tolerating CRLF. Slice 2 writes this file on Windows; if
    // any path ever opens it in text mode, a stray '\r' must not end up glued to
    // the last field of every record.
    std::vector<std::string> lines;
    lines.reserve(text.size() / 96 + 4);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::size_t end = i;
            if (end > start && text[end - 1] == '\r') --end;
            if (!(i == text.size() && end == start)) // ignore a trailing newline
                lines.emplace_back(text, start, end - start);
            start = i + 1;
        }
    }
    if (lines.size() < 2) return res;   // header needs two lines; ok stays false

    // Header line: magic + version.
    {
        const auto f = detail::splitTabs(lines[0]);
        if (f.size() != 2 || f[0] != kMagic) return res;
        int32_t ver = 0;
        if (!detail::parseI32(f[1], ver) || ver != kFormatVersion) return res;
    }
    // Root lines: the RUN of them starting at line 1. Line 1 is required (a file
    // without it is not this format); further ones are optional and are what makes
    // the index multi-root. See the format note at the top of this file - a `root`
    // line has 2 fields and a record has 12, so the two can never be confused, which
    // is why this needed no version bump.
    std::size_t li = 1;
    {
        const auto f = detail::splitTabs(lines[1]);
        if (f.size() != 2 || f[0] != "root") return res;
        // An EMPTY value means "no root configured" and yields no entry, so an
        // unconfigured index parses to an empty list rather than a list of one
        // empty string that would later be treated as a real root.
        std::string r = detail::unescape(f[1]);
        if (!r.empty()) res.index.roots.push_back(std::move(r));
        ++li;
        for (; li < lines.size(); ++li) {
            const auto g = detail::splitTabs(lines[li]);
            if (g.size() != 2 || g[0] != "root") break;
            std::string extra = detail::unescape(g[1]);
            if (!extra.empty()) res.index.roots.push_back(std::move(extra));
        }
    }

    res.ok = true;
    res.index.tracks.reserve(lines.size() > li ? lines.size() - li : 0);

    for (; li < lines.size(); ++li) {
        if (lines[li].empty()) continue;              // blank line: not a record
        const auto f = detail::splitTabs(lines[li]);
        if (f.size() != kFieldCount) { ++res.skipped_records; continue; }

        LibraryTrack t;
        if (!detail::parseI32(f[6],  t.track_no)     ||
            !detail::parseI32(f[7],  t.disc_no)      ||
            !detail::parseI32(f[8],  t.year)         ||
            !detail::parseI32(f[9],  t.duration_sec) ||
            !detail::parseI64(f[10], t.mtime)        ||
            !detail::parseU64(f[11], t.size)) {
            ++res.skipped_records;
            continue;
        }
        t.path         = detail::unescape(f[0]);
        // A record with no path names nothing and can never be played; it is
        // corruption, not a track.
        if (t.path.empty()) { ++res.skipped_records; continue; }
        t.artist       = detail::unescape(f[1]);
        t.album        = detail::unescape(f[2]);
        t.album_artist = detail::unescape(f[3]);
        t.title        = detail::unescape(f[4]);
        t.genre        = detail::unescape(f[5]);
        res.index.tracks.push_back(std::move(t));
    }
    // Slice 8: the compilation set is derived, so it is built HERE rather than left to
    // callers - parsing is one of only two ways an index comes into existence, and a
    // caller that forgot would get a browse tree silently missing its compilations.
    rebuildCompilations(res.index);
    return res;
}

// ── Hierarchy queries ───────────────────────────────────────────────────────
// Every one returns a deterministically sorted result, because the pane draws
// them directly and a list that reorders between launches reads as a bug.
//
// An empty grouping artist (a file with neither tag) is kept as its own empty
// group rather than dropped — losing tracks silently would be worse. What that
// row is LABELLED is the UI's call in slices 3-4.

// ── Surviving a rebuild ─────────────────────────────────────────────────────
// THE STALENESS ANSWER, and the reason no generation counter is needed: the UI
// remembers the IDENTITY it had selected - an artist name here, a path for a
// track row later - and never an index into anything. Query results are consumed
// inside one populate call and never survive a frame, so a rebuild underneath a
// live selection cannot leave a stale subscript behind; there is nothing to
// invalidate. After repopulating, the caller asks this where the cursor goes.
//
// Returns the row matching `remembered` (case-insensitively, the same fold the
// lists are built with), or the nearest valid row when it has gone: 0 for a
// non-empty list, and -1 only for an empty one, which the caller renders as its
// own empty state rather than a selection.
inline int restoreCursor(const std::string& remembered,
                         const std::vector<std::string>& rows) {
    if (rows.empty()) return -1;
    if (!remembered.empty()) {
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (detail::icmp(rows[i], remembered) == 0) return static_cast<int>(i);
    }
    return 0;
}

// DEDUP BEFORE SORTING (slice 8). The old form pushed one string per TRACK and then
// sorted the lot: at 100,000 records that is sorting 100,000 strings down to 900, and
// it MEASURED at 56.6 ms. A hash set first, sorting only the distinct values, is
// 1.4 ms - the same output, byte for byte, asserted in the test.
//
// It matters more than the raw figure suggests, because this query is on the path INTO
// the section and runs again on every repopulate after a rescan. It is the one query
// whose cost the user waits on rather than asks for.
inline std::vector<std::string> artists(const LibraryIndex& idx) {
    std::unordered_set<std::string> seen;
    seen.reserve(idx.tracks.size() / 8 + 16);
    std::vector<std::string> out;
    for (const LibraryTrack& t : idx.tracks) {
        const std::string& a = groupingArtist(idx, t);
        if (seen.insert(a).second) out.push_back(a);
    }
    // Still sortUniqueCI, not a plain sort: the hash set dedups by BYTES, and two
    // artists differing only in case must still collapse to one row.
    detail::sortUniqueCI(out);
    return out;
}

// Every distinct genre in the collection, split and folded (slice 10).
//
// Dedup-first from birth rather than shipping the shape slice 8 had to fix: this
// does strictly more work per record than artists() - a split per record, not one
// push - so it is the one query where sorting one entry per TRACK would hurt most.
//
// An untagged track contributes nothing and gets NO synthetic "(no genre)" row.
// 679 of 2,157 records here are untagged: a bucket holding a third of the
// collection, under a name that is not a genre, would be the largest row in the
// list. Those tracks stay reachable by artist, album and search, which is where
// someone looks for them.
// Slice 16: grouped on detail::genreKey, and the row is LABELLED with the variant
// that the most records actually use - `Post Punk` (5) rather than `Post-Punk` (2).
//
// THE TIE RULE IS NOT DECORATION. Two variants with equal counts have to resolve
// deterministically or the genre list reorders between runs on an unchanged
// collection, because the winner would be whichever the hash map happened to yield
// first. The tie goes to whichever sorts first under detail::icmp - the comparison
// sortUniqueCI already uses, so it is not a new rule either.
//
// Counting is PER TRACK per distinct variant: a tag reading `Rock / Rock` is one
// record's worth of Rock, not two.
inline std::vector<std::string> genres(const LibraryIndex& idx) {
    struct Variant { std::string label; std::size_t count = 0; };
    std::unordered_map<std::string, std::vector<Variant>> groups;   // key -> its variants
    std::unordered_set<std::string> per_track;

    for (const LibraryTrack& t : idx.tracks) {
        if (t.genre.empty()) continue;
        per_track.clear();
        for (const std::string& g : detail::splitGenres(t.genre)) {
            if (!per_track.insert(g).second) continue;      // same variant twice on one tag
            std::vector<Variant>& vs = groups[detail::genreKey(g)];
            bool found = false;
            for (Variant& v : vs)
                if (v.label == g) { ++v.count; found = true; break; }
            if (!found) vs.push_back({ g, 1 });
        }
    }

    std::vector<std::string> out;
    out.reserve(groups.size());
    for (auto& kv : groups) {
        const Variant* best = nullptr;
        for (const Variant& v : kv.second) {
            if (!best || v.count > best->count
                || (v.count == best->count && detail::icmp(v.label, best->label) < 0))
                best = &v;
        }
        if (best) out.push_back(best->label);
    }
    // sortUniqueCI, not a plain sort: one row per key means the labels are already
    // distinct, but the list still has to come out in a stable display order.
    detail::sortUniqueCI(out);
    return out;
}

// The artist list RESTRICTED to one genre (slice 10).
//
// A genre is one new LEVEL plus this filter, rather than a parallel hierarchy: the
// album and track levels beneath inherit the filter through libnav::State, so
// `Genres -> Artists -> Albums -> Tracks` reuses three levels that already exist.
//
// Still routed through groupingArtist, so a compilation stays ONE `Various Artists`
// row inside a genre exactly as it does outside one. That is the slice-1 seam doing
// its job for the fourth time.
inline std::vector<std::string> artists(const LibraryIndex& idx,
                                        const std::string& genre) {
    if (genre.empty()) return artists(idx);
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    for (const LibraryTrack& t : idx.tracks) {
        if (!detail::trackHasGenre(t.genre, genre)) continue;
        const std::string& a = groupingArtist(idx, t);
        if (seen.insert(a).second) out.push_back(a);
    }
    detail::sortUniqueCI(out);
    return out;
}

inline std::vector<std::string> albumsForArtist(const LibraryIndex& idx,
                                                const std::string& artist) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    for (const LibraryTrack& t : idx.tracks)
        if (detail::icmp(groupingArtist(idx, t), artist) == 0 && seen.insert(t.album).second)
            out.push_back(t.album);
    detail::sortUniqueCI(out);
    return out;
}

// The album list restricted to a genre (slice 10). An album is shown when AT LEAST
// ONE of its tracks carries the genre - not when all of them do, because a single
// untagged track would otherwise hide the album the user came looking for.
//
// THE FILTER STOPS HERE. Level 3 shows the WHOLE album, unfiltered, and that is
// deliberate on two grounds. An album is the unit a listener thinks in, so showing
// eleven of fourteen tracks because three are tagged differently is worse than
// showing three tracks that are not strictly the genre. And libnav::albumTracks is
// the ONE ordering function, shared with the album append since LIB-AA - filtering
// it here would either make the drawn order diverge from the appended order or
// force a second filtered path, which is the two-lists defect again.
inline std::vector<std::string> albumsForArtist(const LibraryIndex& idx,
                                                const std::string& artist,
                                                const std::string& genre) {
    if (genre.empty()) return albumsForArtist(idx, artist);
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    for (const LibraryTrack& t : idx.tracks)
        if (detail::icmp(groupingArtist(idx, t), artist) == 0
            && detail::trackHasGenre(t.genre, genre)
            && seen.insert(t.album).second)
            out.push_back(t.album);
    detail::sortUniqueCI(out);
    return out;
}

// Returns INDICES into idx.tracks so the caller reads path and display fields
// without copying strings.
//
// LIFETIME — carried into slice 3's design note: these indices are valid only
// against the index instance that produced them. Once slice 2 can rebuild the
// index while the UI holds a selection, the UI must either re-query after a
// rebuild or hold the path (the stable identity) rather than the index. This is
// a staleness question to answer at the UI layer, deliberately not papered over
// here with a generation counter this slice cannot test.
inline std::vector<std::size_t> tracksForAlbum(const LibraryIndex& idx,
                                               const std::string& artist,
                                               const std::string& album) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < idx.tracks.size(); ++i) {
        const auto& t = idx.tracks[i];
        if (detail::icmp(groupingArtist(idx, t), artist) == 0 &&
            detail::icmp(t.album, album) == 0)
            out.push_back(i);
    }
    // Disc, then track number, then title, then path. The last two are the
    // tie-breakers that make the order total for untagged rips, where every
    // track_no is 0 and only the filename distinguishes them.
    std::sort(out.begin(), out.end(), [&idx](std::size_t a, std::size_t b) {
        const auto& x = idx.tracks[a];
        const auto& y = idx.tracks[b];
        if (x.disc_no  != y.disc_no)  return x.disc_no  < y.disc_no;
        if (x.track_no != y.track_no) return x.track_no < y.track_no;
        const int c = detail::icmp(x.title, y.title);
        if (c != 0) return c < 0;
        return x.path < y.path;
    });
    return out;
}

// ── Whole-collection search (slice 7) ────────────────────────────────────────
//
// The first addition to this query surface since slice 1, and the reason the library
// exists: finding a track without knowing where it lives. The three queries above all
// require knowing the artist; this one requires knowing nothing.
//
// RETURNS RECORDS BY VALUE. Six slices have held the rule that the UI never holds a
// subscript into the index, and a results list is the strongest pull yet toward
// breaking it, so the conversion happens here and nothing else ever sees an index.
// libnav::albumTracks is the precedent.
//
// NO FORMAT CHANGE: this reads fields every shipped index already carries, so no
// existing library.idx is invalidated.
//
// MATCHING. The query is split on spaces and every term must be found in at least one
// field - AND across terms, OR across fields - which is what makes "beastie sabotage"
// work, and is how people search. Both sides go through detail::foldAscii, so matching
// is case- and accent-insensitive for Latin (see that function for the stated limit).
//
// FIELDS: title, artist, album, album-artist, genre, and the FILENAME STEM. The stem is
// included because an untagged rip has nothing else to match on, and those are precisely
// the tracks that browsing cannot find. Only the stem, never the whole path, so a query
// cannot match a directory name and drag in every track beneath it.
//
// CAP. `limit` bounds the returned rows; `total_out`, when given, receives the TRUE
// number of matches. The count is taken before the cap, so a capped list can say "500 of
// 2045" honestly rather than implying it is complete - which matters because a
// one-character query matches almost everything (measured: 2045 of 2156).
//
// ORDER: artist, album, disc, track, path. Deterministic, and it puts the several format
// copies of one track ADJACENT rather than scattered - measured on the real collection at
// 84 records for a 12-track album.
inline std::vector<LibraryTrack> search(const LibraryIndex& idx,
                                       const std::string& query,
                                       std::size_t limit = 500,
                                       std::size_t* total_out = nullptr) {
    std::vector<LibraryTrack> out;
    if (total_out) *total_out = 0;

    // Fold the query once, then split it. Empty or whitespace-only matches nothing
    // rather than everything: an empty search box should not dump the collection.
    std::vector<std::string> terms;
    {
        const std::string f = detail::foldAscii(query);
        std::string cur;
        for (char c : f) {
            if (c == ' ' || c == '\t') { if (!cur.empty()) { terms.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) terms.push_back(cur);
    }
    if (terms.empty()) return out;

    std::vector<std::size_t> hits;
    std::string fold;              // one buffer, reused for every field of every record
    for (std::size_t i = 0; i < idx.tracks.size(); ++i) {
        const LibraryTrack& t = idx.tracks[i];
        bool all = true;
        for (const std::string& q : terms) {
            const std::string* const fields[5] =
                { &t.title, &t.artist, &t.album, &t.album_artist, &t.genre };
            bool any = false;
            for (const std::string* s : fields) {
                detail::foldAsciiInto(*s, fold);
                if (fold.find(q) != std::string::npos) { any = true; break; }
            }
            if (!any) {                                  // last resort: the filename
                detail::foldAsciiInto(detail::pathStemOf(t.path), fold);
                any = fold.find(q) != std::string::npos;
            }
            if (!any) { all = false; break; }
        }
        if (all) hits.push_back(i);
    }

    if (total_out) *total_out = hits.size();

    const auto by_place = [&idx](std::size_t a, std::size_t b) {
        const LibraryTrack& x = idx.tracks[a];
        const LibraryTrack& y = idx.tracks[b];
        int c = detail::icmp(groupingArtist(idx, x), groupingArtist(idx, y));
        if (c != 0) return c < 0;
        c = detail::icmp(x.album, y.album);
        if (c != 0) return c < 0;
        if (x.disc_no  != y.disc_no)  return x.disc_no  < y.disc_no;
        if (x.track_no != y.track_no) return x.track_no < y.track_no;
        c = detail::icmp(x.title, y.title);
        if (c != 0) return c < 0;
        return x.path < y.path;
    };

    const std::size_t n = (hits.size() < limit) ? hits.size() : limit;

    // PARTIAL sort when the cap bites, because only the first `n` are ever returned.
    // This is not a micro-optimisation: the comparator does case-insensitive compares on
    // artist and album, so a full sort of a near-total match set dominates everything
    // else. Measured on 100k records where every record matched, full sort against
    // partial: it is the difference between half a second and tens of milliseconds, and
    // half a second per keystroke is not a live search.
    if (n < hits.size()) std::partial_sort(hits.begin(), hits.begin() + (std::ptrdiff_t)n,
                                           hits.end(), by_place);
    else                 std::sort(hits.begin(), hits.end(), by_place);

    out.reserve(n);
    for (std::size_t k = 0; k < n; ++k) out.push_back(idx.tracks[hits[k]]);
    return out;
}

// ── Play statistics (slice 10) ──────────────────────────────────────────────
//
// The two stat views join the index against Config::track_stats AT QUERY TIME.
// Nothing is copied into the index and no play data is stored here: Config stays
// the one source of what has been played, the index stays the one description of
// what exists. That was slice 1's decision and it holds.
//
// The join goes through a FOLDED map the caller builds once, rather than a lookup
// per record against the raw one, for two reasons that are the same reason: see
// detail::foldPathKey. A byte-exact join finds 20 of 295 real entries, and 17 real
// files hold TWO entries differing only in case whose counts are split between them
// (`One of Us` is 73 + 22 = 95). Building the folded map SUMS those, so the count a
// view ranks on is the count the file actually has.

// A track's play record, aggregated. Deliberately not Config's TrackStats: this
// header is pure and links into a test with nothing else, so it cannot include
// Config.h. buildPlayStats is a template for exactly that reason.
struct PlayStat {
    int64_t play_count  = 0;
    int64_t last_played = 0;
};

// Fold Config's track_stats into a join-ready map, SUMMING entries whose paths
// differ only in case (Windows) and keeping the most recent last_played.
//
// Templated on the source map so this file gains no dependency on Config. Any map
// of path -> { .play_count, .last_played } works.
template <class StatMap>
inline std::unordered_map<std::string, PlayStat> buildPlayStats(const StatMap& src) {
    std::unordered_map<std::string, PlayStat> out;
    out.reserve(src.size());
    for (const auto& kv : src) {
        PlayStat& p = out[detail::foldPathKey(kv.first)];
        p.play_count += (int64_t)kv.second.play_count;         // AGGREGATE, not overwrite
        const int64_t lp = (int64_t)kv.second.last_played;
        if (lp > p.last_played) p.last_played = lp;
    }
    return out;
}

// One track's aggregated play record, or a zeroed one when it has never played.
// The info pane reads this too, so the number it prints and the number the
// most-played view ranks on cannot disagree.
inline PlayStat lookupPlayStat(const std::unordered_map<std::string, PlayStat>& ps,
                               const std::string& path) {
    const auto it = ps.find(detail::foldPathKey(path));
    return (it == ps.end()) ? PlayStat{} : it->second;
}

// Most played, count descending. `total_out` receives the TRUE count before the cap,
// the LIB-S7 idiom, so a capped view can say "500 of 1921" rather than understate.
//
// The order is TOTAL - count, then last_played, then path - so a repopulate cannot
// shuffle rows that tie. Records BY VALUE: no subscript escapes, as since slice 1.
inline std::vector<LibraryTrack> mostPlayed(const LibraryIndex& idx,
                                            const std::unordered_map<std::string, PlayStat>& ps,
                                            std::size_t limit,
                                            std::size_t* total_out = nullptr) {
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < idx.tracks.size(); ++i)
        if (lookupPlayStat(ps, idx.tracks[i].path).play_count > 0) hits.push_back(i);

    if (total_out) *total_out = hits.size();

    const auto by_plays = [&](std::size_t a, std::size_t b) {
        const PlayStat pa = lookupPlayStat(ps, idx.tracks[a].path);
        const PlayStat pb = lookupPlayStat(ps, idx.tracks[b].path);
        if (pa.play_count  != pb.play_count)  return pa.play_count  > pb.play_count;
        if (pa.last_played != pb.last_played) return pa.last_played > pb.last_played;
        return idx.tracks[a].path < idx.tracks[b].path;
    };
    const std::size_t n = (limit && hits.size() > limit) ? limit : hits.size();
    // partial_sort for the same reason search() uses it: only the first n are ever
    // returned, and sorting the rest is work whose result is discarded.
    if (n < hits.size())
        std::partial_sort(hits.begin(), hits.begin() + (std::ptrdiff_t)n, hits.end(), by_plays);
    else
        std::sort(hits.begin(), hits.end(), by_plays);

    std::vector<LibraryTrack> out;
    out.reserve(n);
    for (std::size_t k = 0; k < n; ++k) out.push_back(idx.tracks[hits[k]]);
    return out;
}

// Never played: indexed tracks with no stat entry, or one whose count is zero.
//
// ~89% of the reference collection, which is why it takes the LIB-S7 cap rather
// than building 1,921 rows nobody scrolls. Index order, which is already total.
inline std::vector<LibraryTrack> neverPlayed(const LibraryIndex& idx,
                                             const std::unordered_map<std::string, PlayStat>& ps,
                                             std::size_t limit,
                                             std::size_t* total_out = nullptr) {
    std::size_t total = 0;
    std::vector<LibraryTrack> out;
    for (const LibraryTrack& t : idx.tracks) {
        if (lookupPlayStat(ps, t.path).play_count > 0) continue;
        ++total;
        if (!limit || out.size() < limit) out.push_back(t);
    }
    if (total_out) *total_out = total;
    return out;
}

} // namespace libidx
