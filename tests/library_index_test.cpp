// library_index_test - proves LibraryIndex.h headless and device-free: no
// filesystem, no tags, no curses, no threads. Library slice 1 is the index model,
// its on-disk FORMAT, and the hierarchy queries the [Library] pane will consume,
// and this is where all three are nailed down before any of them has a caller.
//
// The format's first field is a PATH - identity, not decoration - so the central
// obligation is EXACT round-trip: a path carrying a tab, a newline, a backslash
// or non-ASCII bytes must come back byte-identical, because a mangled path names
// a file that does not exist and fails on exactly the tracks nobody checks. The
// rest is the usual hostile-input contract: never throw, never hang, degrade to
// fewer records and say how many were dropped.
//
// Header-only, both matrix jobs.

#include "LibraryIndex.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

using namespace libidx;

static bool sameTrack(const LibraryTrack& a, const LibraryTrack& b) {
    return a.path == b.path && a.artist == b.artist && a.album == b.album &&
           a.album_artist == b.album_artist && a.title == b.title &&
           a.genre == b.genre && a.track_no == b.track_no &&
           a.disc_no == b.disc_no && a.year == b.year &&
           a.duration_sec == b.duration_sec && a.mtime == b.mtime &&
           a.size == b.size;
}

static LibraryTrack mk(const std::string& path, const std::string& artist,
                       const std::string& album, const std::string& title) {
    LibraryTrack t;
    t.path = path; t.artist = artist; t.album = album; t.title = title;
    t.track_no = 1; t.disc_no = 1; t.year = 2000; t.duration_sec = 180;
    t.mtime = 1700000000; t.size = 5000000;
    return t;
}

// ── Round-trip: a plain index survives serialise -> parse unchanged ──────────
static void test_roundtrip_basic() {
    LibraryIndex idx;
    idx.root = "C:\\Users\\david\\Music";
    idx.tracks.push_back(mk("C:\\Users\\david\\Music\\a.flac", "Artist", "Album", "Title"));
    idx.tracks.push_back(mk("/home/dos/Music/b.opus", "Other", "Record", "Song"));

    auto r = parseIndex(serialiseIndex(idx));
    CHECK(r.ok, "header understood");
    CHECK(r.skipped_records == 0, "skipped=%zu", r.skipped_records);
    CHECK(r.index.root == idx.root, "root [%s]", r.index.root.c_str());
    CHECK(r.index.tracks.size() == 2, "n=%zu", r.index.tracks.size());
    if (r.index.tracks.size() == 2) {
        CHECK(sameTrack(r.index.tracks[0], idx.tracks[0]), "track 0 exact");
        CHECK(sameTrack(r.index.tracks[1], idx.tracks[1]), "track 1 exact");
    }
}

// ── THE CENTRAL TEST: every framing character survives, in every string field ─
// Tabs and newlines are legal in POSIX filenames, and backslash is ubiquitous in
// Windows ones. Config.cpp's tab-fold would silently destroy all three.
static void test_roundtrip_hostile_fields() {
    const std::string nasty[] = {
        "tab\there",
        "newline\nhere",
        "carriage\rreturn",
        "back\\slash",
        "all\t\n\r\\four",
        "\\",                       // lone trailing backslash
        "\\t",                      // the literal two characters, not a tab
        "",                         // empty
        "caf\xC3\xA9 na\xC3\xAF ve",              // UTF-8 multi-byte
        "\xE2\x80\x9Csmart quotes\xE2\x80\x9D",   // the CP1252 landmine bytes
        std::string("nul\0inside", 10),           // embedded NUL
        std::string(171, 'x'),                    // the real longest path length
    };
    for (const auto& s : nasty) {
        LibraryIndex idx;
        idx.root = s;
        LibraryTrack t;
        t.path = s; t.artist = s; t.album = s; t.album_artist = s;
        t.title = s; t.genre = s;
        t.track_no = -1; t.disc_no = 0; t.year = 1999;
        t.duration_sec = 0; t.mtime = -1; t.size = 0;
        // A path must be non-empty to be a record at all; give the empty case a
        // real path so the other five fields still get their round-trip check.
        if (t.path.empty()) t.path = "/x";
        idx.tracks.push_back(t);

        auto r = parseIndex(serialiseIndex(idx));
        CHECK(r.ok && r.skipped_records == 0, "parsed [len=%zu]", s.size());
        CHECK(r.index.root == s, "root exact [len=%zu]", s.size());
        if (r.index.tracks.size() == 1)
            CHECK(sameTrack(r.index.tracks[0], t), "fields exact [len=%zu]", s.size());
        else
            CHECK(false, "lost the record [len=%zu]", s.size());
    }
}

// ── Numeric extremes round-trip, including the 64-bit ones ──────────────────
static void test_roundtrip_numeric_extremes() {
    LibraryIndex idx;
    LibraryTrack t = mk("/p", "a", "b", "c");
    t.track_no = INT32_MAX; t.disc_no = INT32_MIN; t.year = 0;
    t.duration_sec = INT32_MAX;
    t.mtime = INT64_MIN; t.size = UINT64_MAX;
    idx.tracks.push_back(t);

    auto r = parseIndex(serialiseIndex(idx));
    CHECK(r.ok && r.index.tracks.size() == 1, "parsed");
    if (r.index.tracks.size() == 1) CHECK(sameTrack(r.index.tracks[0], t), "extremes exact");
}

// ── Header contract: bad magic/version/root is rejected, not guessed at ─────
static void test_header_rejection() {
    auto bad = [](const std::string& s, const char* what) {
        auto r = parseIndex(s);
        CHECK(!r.ok, "rejected: %s", what);
        CHECK(r.index.tracks.empty(), "no tracks from: %s", what);
    };
    bad("", "empty");
    bad("\n", "one blank line");
    bad("remoct-library-index\t1", "header but no root line");
    bad("wrong-magic\t1\nroot\t/m\n", "wrong magic");
    bad("remoct-library-index\t2\nroot\t/m\n", "future version");
    bad("remoct-library-index\t0\nroot\t/m\n", "version 0");
    bad("remoct-library-index\tx\nroot\t/m\n", "non-numeric version");
    bad("remoct-library-index\t1\nnotroot\t/m\n", "missing root key");
    bad("remoct-library-index\t1\t9\nroot\t/m\n", "over-long header");

    // A valid header with zero records is legitimately an EMPTY library, not an
    // error - a scan that found nothing must be distinguishable from a corrupt file.
    auto r = parseIndex("remoct-library-index\t1\nroot\t/m\n");
    CHECK(r.ok && r.index.tracks.empty() && r.index.root == "/m", "empty library is ok");
}

// ── Malformed records are skipped and counted, never guessed at ─────────────
static void test_record_rejection() {
    const std::string head = "remoct-library-index\t1\nroot\t/m\n";
    const std::string good = "/p\ta\tb\tc\td\te\t1\t1\t2000\t180\t1700000000\t5000000\n";

    struct { const char* line; const char* what; } bad[] = {
        {"/p\ta\tb\tc\td\te\t1\t1\t2000\t180\t1700000000\n",           "too few fields"},
        {"/p\ta\tb\tc\td\te\t1\t1\t2000\t180\t1700000000\t500\t9\n",   "too many fields"},
        {"/p\ta\tb\tc\td\te\tX\t1\t2000\t180\t1700000000\t500\n",      "non-numeric track"},
        {"/p\ta\tb\tc\td\te\t1\t1\t2000\t180\tX\t500\n",               "non-numeric mtime"},
        {"/p\ta\tb\tc\td\te\t1\t1\t2000\t180\t170\t-5\n",              "negative size"},
        {"/p\ta\tb\tc\td\te\t1\t1\t2000\t180\t170\t99999999999999999999999\n", "size overflow"},
        {"\ta\tb\tc\td\te\t1\t1\t2000\t180\t170\t500\n",               "empty path"},
        {"garbage\n",                                                   "not a record at all"},
    };
    for (const auto& b : bad) {
        auto r = parseIndex(head + good + b.line + good);
        CHECK(r.ok, "still readable: %s", b.what);
        CHECK(r.index.tracks.size() == 2, "kept the 2 good (%s) got %zu", b.what, r.index.tracks.size());
        CHECK(r.skipped_records == 1, "counted 1 skip (%s) got %zu", b.what, r.skipped_records);
    }
}

// ── Truncation, CRLF, and a missing trailing newline all behave ─────────────
static void test_truncation_and_line_endings() {
    LibraryIndex idx;
    idx.root = "/m";
    idx.tracks.push_back(mk("/p1", "a", "b", "c"));
    idx.tracks.push_back(mk("/p2", "a", "b", "d"));
    const std::string full = serialiseIndex(idx);

    // No trailing newline on the last record.
    std::string no_nl = full;
    if (!no_nl.empty() && no_nl.back() == '\n') no_nl.pop_back();
    CHECK(parseIndex(no_nl).index.tracks.size() == 2, "missing trailing newline");

    // CRLF throughout - what a text-mode write on Windows would produce.
    std::string crlf;
    for (char c : full) { if (c == '\n') crlf += '\r'; crlf += c; }
    auto rc = parseIndex(crlf);
    CHECK(rc.ok && rc.index.tracks.size() == 2, "CRLF n=%zu", rc.index.tracks.size());
    if (rc.index.tracks.size() == 2)
        CHECK(sameTrack(rc.index.tracks[1], idx.tracks[1]), "CRLF last field not glued to \\r");

    // Truncated mid-record: the survivor is kept, the fragment is counted.
    auto rt = parseIndex(full.substr(0, full.size() - 20));
    CHECK(rt.ok, "truncated still readable");
    CHECK(rt.index.tracks.size() == 1 && rt.skipped_records == 1,
          "truncated n=%zu skipped=%zu", rt.index.tracks.size(), rt.skipped_records);

    // Every prefix of a valid file must parse without throwing or hanging.
    for (std::size_t n = 0; n <= full.size(); ++n) (void)parseIndex(full.substr(0, n));
    CHECK(true, "every prefix survived");
}

// ── The grouping seam: album-artist wins when present ───────────────────────
static void test_grouping_artist() {
    LibraryTrack t = mk("/p", "Track Artist", "Album", "Title");
    CHECK(groupingArtist(t) == "Track Artist", "falls back to artist");
    t.album_artist = "Album Artist";
    CHECK(groupingArtist(t) == "Album Artist", "album-artist wins");
    t.album_artist.clear(); t.artist.clear();
    CHECK(groupingArtist(t).empty(), "both empty is an empty group, not a crash");
}

// ── Hierarchy queries: grouping, dedupe, determinism ───────────────────────
static void test_queries() {
    LibraryIndex idx;
    idx.root = "/m";
    // A compilation: three artists, one album-artist. Must be ONE artist row.
    for (int i = 0; i < 3; ++i) {
        LibraryTrack t = mk("/c" + std::to_string(i), "Guest " + std::to_string(i),
                            "Comp", "Song " + std::to_string(i));
        t.album_artist = "Various Artists";
        t.track_no = 3 - i;                   // deliberately out of order
        idx.tracks.push_back(t);
    }
    // Case variants of one artist must collapse to one row.
    idx.tracks.push_back(mk("/b1", "The Beatles", "Revolver", "Taxman"));
    idx.tracks.push_back(mk("/b2", "the beatles", "Revolver", "Eleanor Rigby"));
    idx.tracks.push_back(mk("/b3", "The Beatles", "Abbey Road", "Come Together"));
    // A track with no artist at all is kept, not dropped.
    idx.tracks.push_back(mk("/u1", "", "Unknown Album", "Nameless"));

    auto a = artists(idx);
    CHECK(a.size() == 3, "artists=%zu (expect empty, Beatles, Various)", a.size());
    CHECK(!a.empty() && a[0].empty(), "the empty group sorts first and is kept");

    auto beatles = albumsForArtist(idx, "The Beatles");
    CHECK(beatles.size() == 2, "beatles albums=%zu", beatles.size());
    if (beatles.size() == 2)
        CHECK(beatles[0] == "Abbey Road" && beatles[1] == "Revolver", "albums sorted");
    CHECK(albumsForArtist(idx, "THE BEATLES").size() == 2, "artist lookup is case-insensitive");

    auto comp = tracksForAlbum(idx, "Various Artists", "Comp");
    CHECK(comp.size() == 3, "compilation held together: %zu", comp.size());
    if (comp.size() == 3) {
        CHECK(idx.tracks[comp[0]].track_no == 1 &&
              idx.tracks[comp[1]].track_no == 2 &&
              idx.tracks[comp[2]].track_no == 3, "sorted by track number");
    }
    CHECK(tracksForAlbum(idx, "Nobody", "Nothing").empty(), "unknown album is empty, not a crash");

    // Determinism: identical input, identical order, every time.
    for (int i = 0; i < 5; ++i) CHECK(artists(idx) == a, "artists deterministic");
}

// ── Untagged rips: every track_no is 0, so the tie-break must still be total ─
static void test_untagged_ordering() {
    LibraryIndex idx;
    for (int i = 5; i >= 1; --i) {
        LibraryTrack t = mk("/rip/" + std::to_string(i) + ".wav", "A", "B", "");
        t.track_no = 0; t.disc_no = 0;
        idx.tracks.push_back(t);
    }
    auto v = tracksForAlbum(idx, "A", "B");
    CHECK(v.size() == 5, "n=%zu", v.size());
    for (std::size_t i = 1; i < v.size(); ++i)
        CHECK(idx.tracks[v[i - 1]].path < idx.tracks[v[i]].path, "path tie-break orders %zu", i);
}

// ── Scale: measured, not promised ──────────────────────────────────────────
static void test_scale(std::size_t n, const char* label) {
    LibraryIndex idx;
    idx.root = "C:\\Users\\david\\Music";
    idx.tracks.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        LibraryTrack t = mk(idx.root + "\\Artist " + std::to_string(i % 400) +
                                "\\Album " + std::to_string(i % 1200) +
                                "\\" + std::to_string(i) + " caf\xC3\xA9.flac",
                            "Artist " + std::to_string(i % 400),
                            "Album "  + std::to_string(i % 1200),
                            "Track "  + std::to_string(i));
        t.track_no = static_cast<int32_t>(i % 20);
        idx.tracks.push_back(std::move(t));
    }
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    const std::string text = serialiseIndex(idx);
    auto t1 = clk::now();
    auto r = parseIndex(text);
    auto t2 = clk::now();
    auto arts = artists(r.index);
    auto t3 = clk::now();

    auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    CHECK(r.ok && r.index.tracks.size() == n && r.skipped_records == 0,
          "%s round-trip n=%zu", label, r.index.tracks.size());
    std::printf("  [scale %-9s] n=%6zu  bytes=%9zu  serialise=%7.1f ms  parse=%7.1f ms  artists()=%6.1f ms\n",
                label, n, text.size(), ms(t0, t1), ms(t1, t2), ms(t2, t3));
    (void)arts;
}

// ── restoreCursor: the staleness answer, proven without curses ──────────────
// The UI remembers the artist NAME it had selected, never a subscript, so a scan
// landing underneath a live selection re-seats on the same artist rather than on
// whatever row that index now happens to be. These are the cases that matters in.
static void test_restore_cursor() {
    const std::vector<std::string> rows = {"", "Beatles", "Cyndi Lauper", "UB40"};

    CHECK(restoreCursor("Beatles", rows) == 1,      "found -> its row");
    CHECK(restoreCursor("UB40", rows) == 3,         "found -> last row");
    CHECK(restoreCursor("BEATLES", rows) == 1,      "case-insensitive, same fold as the list");
    CHECK(restoreCursor("beatles", rows) == 1,      "duplicate-by-case resolves to one row");
    CHECK(restoreCursor("", rows) == 0,             "empty remembered -> first row, not the empty artist by luck");
    CHECK(restoreCursor("Gone Band", rows) == 0,    "artist deleted by a rescan -> clamp to a valid row");
    CHECK(restoreCursor("Beatles", {}) == -1,       "empty list -> -1, caller renders its own empty state");
    CHECK(restoreCursor("", {}) == -1,              "empty list, nothing remembered -> -1");

    // The whole point: a REBUILD must not move the selection off the artist.
    // "" and "New Band" appear/disappear around it, so "Beatles" slides 1 -> 0.
    std::vector<std::string> after = {"Beatles", "Cyndi Lauper", "New Band", "UB40"};
    const int before_row = restoreCursor("Beatles", rows);
    const int after_row  = restoreCursor("Beatles", after);
    CHECK(before_row == 1 && after_row == 0,
          "the subscript genuinely moved (%d -> %d)", before_row, after_row);
    CHECK(after[(size_t)after_row] == "Beatles", "but the SELECTION survived the rebuild");
    CHECK(rows[(size_t)before_row] == after[(size_t)after_row],
          "same artist under the cursor before and after");
}

// ── A Latin-1 artist tag must survive the index, not crash it ───────────────
// Planted rather than hoped for: this is the exact payload the [Library] section's
// exclusion chains exist to survive, and "the real collection probably has one"
// is not a gate. Tag text is display text and never becomes a path.
static void test_latin1_artist_survives() {
    LibraryIndex idx;
    idx.root = "/m";
    LibraryTrack t = mk("/m/a.flac", "Bj\xF6rk", "Post", "Army of Me");  // raw 0xF6, invalid UTF-8
    idx.tracks.push_back(t);
    LibraryTrack t2 = mk("/m/b.flac", "caf\xC3\xA9 tacvba", "Re", "El Ciclon");  // valid UTF-8
    idx.tracks.push_back(t2);

    auto r = parseIndex(serialiseIndex(idx));
    CHECK(r.ok && r.index.tracks.size() == 2, "both survive the format");
    if (r.index.tracks.size() == 2)
        CHECK(r.index.tracks[0].artist == "Bj\xF6rk", "raw Latin-1 artist round-trips byte-exact");

    auto a = artists(r.index);
    CHECK(a.size() == 2, "both artists listed (%zu)", a.size());
    // The query surface must not care that one of them is not valid UTF-8.
    CHECK(!albumsForArtist(r.index, "Bj\xF6rk").empty(), "albums resolve for a Latin-1 artist");
    CHECK(restoreCursor("Bj\xF6rk", a) >= 0, "cursor restores onto a Latin-1 artist");
}

int main() {
    test_roundtrip_basic();
    test_roundtrip_hostile_fields();
    test_roundtrip_numeric_extremes();
    test_header_rejection();
    test_record_rejection();
    test_truncation_and_line_endings();
    test_grouping_artist();
    test_queries();
    test_untagged_ordering();
    test_restore_cursor();
    test_latin1_artist_survives();
    test_scale(2773,   "real");       // the measured collection: 2155 + 618
    test_scale(100000, "headroom");   // synthetic, an order of magnitude beyond

    std::printf("library_index_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
