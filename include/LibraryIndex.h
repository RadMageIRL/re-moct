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
// a std::string THROWS in this application for any byte the ANSI codepage
// cannot map: RE-MOCT calls setlocale(LC_ALL, ""), so the narrow-to-wide
// conversion is CP1252, and even fs::exists(str, ec) throws because the
// conversion runs before the error-code applies. That took down the podcast
// list draw in slice 5, and roughly 5% of the paths in a real collection are
// non-ASCII. This unit never converts a path to anything. Where a later slice
// must actually open a file, port::fopenUtf8 (_wfopen over utf8_to_wide) is the
// one sanctioned route.
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
#include <vector>

namespace libidx {

// ── Format identity ─────────────────────────────────────────────────────────
// A version mismatch DISCARDS and rescans. There is no migration path and there
// is deliberately no plan for one: the index is a cache, entirely rebuildable
// from the files it describes, so carrying migration code would be pure cost.
inline constexpr const char* kMagic         = "remoct-library-index";
inline constexpr int         kFormatVersion = 1;

// Field count of one record line. A line that does not have exactly this many
// fields is skipped and counted, never guessed at.
inline constexpr std::size_t kFieldCount = 12;

// ── One track ───────────────────────────────────────────────────────────────
// Tag text is stored RAW. sanitizeForDisplay runs at draw time in slices 3-4,
// not here: folding on the way in would be lossy, and it would also be
// insufficient (sanitizeForDisplay passes ASCII control bytes straight through,
// as the chapters slice discovered).
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
    std::string               root;    // the music root this index was built from
    std::vector<LibraryTrack> tracks;
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
inline void sortUniqueCI(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end(), iless);
    v.erase(std::unique(v.begin(), v.end(),
                        [](const std::string& a, const std::string& b) {
                            return icmp(a, b) == 0;
                        }),
            v.end());
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
inline const std::string& groupingArtist(const LibraryTrack& t) {
    return t.album_artist.empty() ? t.artist : t.album_artist;
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
    out += "root\t";
    out += detail::escape(idx.root);
    out += '\n';

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
    // Root line.
    {
        const auto f = detail::splitTabs(lines[1]);
        if (f.size() != 2 || f[0] != "root") return res;
        res.index.root = detail::unescape(f[1]);
    }

    res.ok = true;
    res.index.tracks.reserve(lines.size() > 2 ? lines.size() - 2 : 0);

    for (std::size_t li = 2; li < lines.size(); ++li) {
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
    return res;
}

// ── Hierarchy queries ───────────────────────────────────────────────────────
// Every one returns a deterministically sorted result, because the pane draws
// them directly and a list that reorders between launches reads as a bug.
//
// An empty grouping artist (a file with neither tag) is kept as its own empty
// group rather than dropped — losing tracks silently would be worse. What that
// row is LABELLED is the UI's call in slices 3-4.

inline std::vector<std::string> artists(const LibraryIndex& idx) {
    std::vector<std::string> out;
    out.reserve(idx.tracks.size());
    for (const auto& t : idx.tracks) out.push_back(groupingArtist(t));
    detail::sortUniqueCI(out);
    return out;
}

inline std::vector<std::string> albumsForArtist(const LibraryIndex& idx,
                                                const std::string& artist) {
    std::vector<std::string> out;
    for (const auto& t : idx.tracks)
        if (detail::icmp(groupingArtist(t), artist) == 0) out.push_back(t.album);
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
        if (detail::icmp(groupingArtist(t), artist) == 0 &&
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

} // namespace libidx
