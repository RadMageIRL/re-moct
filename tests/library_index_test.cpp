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
// ── Slice 7: the ASCII fold ─────────────────────────────────────────────────
// Case AND diacritics in one pass, which is what a typed query needs: folding case
// alone turns "BJÖRK" into "björk", which a typed "bjork" still misses.
static void test_fold_ascii() {
    using libidx::detail::foldAscii;
    CHECK(foldAscii("BJÖRK") == "bjork", "BJORK-with-umlaut folds to bjork: [%s]",
          foldAscii("BJÖRK").c_str());
    CHECK(foldAscii("bjork") == "bjork", "and the typed form folds to the same thing");
    CHECK(foldAscii("Mötley Crüe") == "motley crue", "[%s]",
          foldAscii("Mötley Crüe").c_str());
    CHECK(foldAscii("Sigur Rós") == "sigur ros", "acute");
    CHECK(foldAscii("Beyoncé") == "beyonce", "e-acute");
    CHECK(foldAscii("Niño") == "nino", "tilde n");
    CHECK(foldAscii("Æon") == "aeon", "AE expands to two letters");
    CHECK(foldAscii("straße") == "strasse", "sharp s expands to ss");
    CHECK(foldAscii("Łódź") == "lodz", "Latin Extended-A: [%s]",
          foldAscii("Łódź").c_str());
    CHECK(foldAscii("Dvořák") == "dvorak", "caron");
    CHECK(foldAscii("PLAIN Ascii 123") == "plain ascii 123", "ASCII lowercased, digits kept");

    // The STATED LIMIT, pinned so it is documented rather than discovered: this is a
    // Latin fold, not a Unicode collation. Anything outside Latin-1/Latin Ext-A passes
    // through byte-exact and matches only itself.
    CHECK(foldAscii("你好") == "你好", "CJK passes through unchanged");
    CHECK(foldAscii("Ж") == "Ж", "Cyrillic passes through unchanged");

    // Hostile input must not throw or hang - the index's standing contract.
    CHECK(foldAscii("") == "", "empty");
    CHECK(foldAscii("\xff\xfe") == "\xff\xfe", "invalid lead bytes are kept verbatim");
    CHECK(foldAscii("caf\xc3") == "caf\xc3", "truncated sequence kept, no read past end");
}

// ── Slice 7: what matches ───────────────────────────────────────────────────
static libidx::LibraryIndex searchIndex() {
    libidx::LibraryIndex idx;
    idx.tracks = {
        mk("/m/a/01 Sabotage.flac",  "Beastie Boys", "Ill Communication", "Sabotage"),
        mk("/m/a/02 Sure Shot.flac", "Beastie Boys", "Ill Communication", "Sure Shot"),
        mk("/m/b/01 Army.flac",      "Ben Folds Five", "Whatever and Ever", "Army"),
        mk("/m/c/track07.flac",      "", "", ""),                 // untagged: filename only
        mk("/m/d/01 Joga.flac",      "Björk", "Homogenic", "Jóga"),
    };
    idx.tracks[2].genre = "Piano Rock";
    idx.tracks[4].album_artist = "Björk";
    return idx;
}

static void test_search_matching() {
    const libidx::LibraryIndex idx = searchIndex();

    CHECK(libidx::search(idx, "sabotage").size() == 1, "title match");
    CHECK(libidx::search(idx, "beastie").size() == 2, "artist match hits both tracks");
    CHECK(libidx::search(idx, "communication").size() == 2, "album match");
    CHECK(libidx::search(idx, "piano").size() == 1, "genre match");
    CHECK(libidx::search(idx, "track07").size() == 1, "FILENAME match - the untagged rip");

    // AND across terms, OR across fields: both terms present, in different fields.
    CHECK(libidx::search(idx, "beastie sabotage").size() == 1, "two terms, two fields");
    CHECK(libidx::search(idx, "beastie army").empty(), "one term absent -> no match");

    // Case and accents, both directions.
    CHECK(libidx::search(idx, "BJORK").size() == 1, "typed ASCII finds the accented tag");
    CHECK(libidx::search(idx, "björk").size() == 1, "typed accented finds it too");
    CHECK(libidx::search(idx, "joga").size() == 1, "accented TITLE found by plain typing");

    // Degenerate queries.
    CHECK(libidx::search(idx, "").empty(), "empty query matches NOTHING, not everything");
    CHECK(libidx::search(idx, "   ").empty(), "whitespace-only likewise");
    CHECK(libidx::search(idx, "zzzznope").empty(), "no match is empty, not a crash");
    CHECK(libidx::search(idx, std::string(500, 'x')).empty(), "query longer than any field");

    // A path must match only by its STEM, or a query would drag in whole directories.
    CHECK(libidx::search(idx, "/m/a/").empty(), "directory text does not match");
}

static void test_search_cap_order_and_identity() {
    libidx::LibraryIndex idx;
    // Deliberately shuffled, two artists, one album each, plus format duplicates of one
    // track - the shape LIB-AA measured (84 records for a 12-track album).
    idx.tracks = {
        mk("/m/z2.flac", "Zed", "Later",  "Song"),
        mk("/m/a1.mp3",  "Abe", "Early",  "Tune"),
        mk("/m/a1.flac", "Abe", "Early",  "Tune"),
        mk("/m/z1.flac", "Zed", "Later",  "Song"),
    };
    idx.tracks[0].track_no = 2;   // Zed, later track
    idx.tracks[1].track_no = 1;
    idx.tracks[2].track_no = 1;
    idx.tracks[3].track_no = 1;   // Zed, first track
    std::size_t total = 0;
    // "a" appears in every row (Abe / Early / Later), so all four match.
    const auto all = libidx::search(idx, "a", 500, &total);
    CHECK(total == 4, "total counts every match, got %zu", total);
    CHECK(all.size() == 4, "and all are returned when under the cap");
    CHECK(all[0].artist == "Abe" && all[3].artist == "Zed", "ordered by artist: [%s..%s]",
          all[0].artist.c_str(), all[3].artist.c_str());
    // Format duplicates land ADJACENT, which is the point of the ordering.
    CHECK(all[0].title == all[1].title, "the two copies of one track are adjacent");
    CHECK(all[0].path != all[1].path, "and are distinct rows");
    // Disc/track ordering within an album.
    CHECK(all[2].track_no == 1 && all[3].track_no == 2, "track order within the album");

    // THE CAP, and that the total still tells the truth.
    std::size_t t2 = 0;
    const auto capped = libidx::search(idx, "a", 2, &t2);
    CHECK(capped.size() == 2, "cap honoured, got %zu", capped.size());
    CHECK(t2 == 4, "total is counted BEFORE the cap, so it cannot understate: %zu", t2);

    // NO SUBSCRIPT ESCAPES: every returned record is a real indexed path.
    for (const auto& r : all) {
        bool found = false;
        for (const auto& t : idx.tracks) if (t.path == r.path) { found = true; break; }
        CHECK(found, "result carries a real indexed path: %s", r.path.c_str());
    }

    // A planted invalid-UTF-8 tag field must be searchable and returnable, not a throw.
    libidx::LibraryIndex l1;
    l1.tracks = { mk("/m/x.flac", "Bj\x92rk", "Al\x92um", "T\x92tle") };
    CHECK(libidx::search(l1, "rk").size() == 1, "raw Latin-1 tag text still matches");
    CHECK(libidx::search(l1, "rk")[0].artist == "Bj\x92rk", "and round-trips byte-exact");
}

static void test_search_scale() {
    libidx::LibraryIndex big;
    big.tracks.reserve(100000);
    for (std::size_t i = 0; i < 100000; ++i)
        big.tracks.push_back(mk("/m/" + std::to_string(i) + "/love song.flac",
                                "Artist " + std::to_string(i % 900),
                                "Album " + std::to_string(i % 300), "Love Song"));
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t total = 0;
    const auto rows = libidx::search(big, "love", 500, &total);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(total == 100000, "all match, got %zu", total);
    CHECK(rows.size() == 500, "capped to 500, got %zu", rows.size());
    // Deliberately generous - this is a REGRESSION guard against someone making the
    // search quadratic, not a benchmark. Measured ~25 ms here; a loaded CI runner gets
    // an order of magnitude of headroom before this flakes.
    CHECK(ms < 1500.0, "100k search stayed linear: %.1f ms", ms);
    std::printf("  [search scale] 100k records, 100k matches, capped 500: %.1f ms\n", ms);
}

// ── Slice 8: compilations ───────────────────────────────────────────────────
// The fixture reproduces all four shapes MEASURED in the real collection, which is
// what makes this a regression test rather than a description of an idea.
static libidx::LibraryIndex compIndex() {
    libidx::LibraryIndex idx;
    auto add = [&](const char* path, const char* artist, const char* album, const char* aa) {
        libidx::LibraryTrack t = mk(path, artist, album, "T");
        t.album_artist = aa;
        idx.tracks.push_back(t);
    };
    // 1. The COMMON case: a real compilation with NO album-artist at all. Eight of the
    //    ten in the reference collection look exactly like this.
    add("/m/p1.flac", "Toto",     "Pure 80s", "");
    add("/m/p2.flac", "A-ha",     "Pure 80s", "");
    add("/m/p3.flac", "Berlin",   "Pure 80s", "");
    // 2. A compilation whose album-artist is a VARIANT marker, not "Various Artists".
    add("/m/r1.flac", "Chuck Berry", "Rock Era", "Various");
    add("/m/r2.flac", "Fats Domino", "Rock Era", "Various");
    add("/m/r3.flac", "Little Richard", "Rock Era", "Various");
    // 3. THE GUEST-ARTIST ALBUM - "Plastic Beach", six credited artists under one real
    //    album-artist. A distinct-artist threshold ALONE calls this a compilation.
    add("/m/g1.flac", "Gorillaz",              "Plastic Beach", "Gorillaz");
    add("/m/g2.flac", "Gorillaz feat. Snoop",  "Plastic Beach", "Gorillaz");
    add("/m/g3.flac", "Gorillaz feat. De La",  "Plastic Beach", "Gorillaz");
    add("/m/g4.flac", "Gorillaz feat. Mos Def","Plastic Beach", "Gorillaz");
    // 4. ARTIST-TAG VARIANCE within one artist's own record.
    add("/m/u1.flac", "Jackson 5",     "Ultimate", "Jackson 5, The");
    add("/m/u2.flac", "Jackson 5, The","Ultimate", "Jackson 5, The");
    add("/m/u3.flac", "Michael Jackson","Ultimate","Jackson 5, The");
    // 5. An ordinary single-artist album - must be untouched.
    add("/m/n1.flac", "Muse", "Absolution", "Muse");
    add("/m/n2.flac", "Muse", "Absolution", "Muse");
    libidx::rebuildCompilations(idx);
    return idx;
}

static void test_compilation_detection() {
    const libidx::LibraryIndex idx = compIndex();

    CHECK(idx.compilations.size() == 2, "exactly two albums flagged, got %zu",
          idx.compilations.size());
    CHECK(libidx::isCompilation(idx, idx.tracks[0]), "no-album-artist multi-artist IS one");
    CHECK(libidx::isCompilation(idx, idx.tracks[3]), "album-artist \"Various\" IS one");
    // The two that matter most - the false positives an artist-count rule would catch.
    CHECK(!libidx::isCompilation(idx, idx.tracks[6]),
          "GUEST-ARTIST album is NOT a compilation (four artists, real album-artist)");
    CHECK(!libidx::isCompilation(idx, idx.tracks[10]),
          "ARTIST-VARIANCE album is NOT a compilation");
    CHECK(!libidx::isCompilation(idx, idx.tracks[13]), "ordinary album is not one");

    // The seam: this is what makes artists/albums/tracks/search agree.
    CHECK(libidx::groupingArtist(idx, idx.tracks[0]) == libidx::kVariousArtists,
          "a compilation track groups under Various Artists");
    CHECK(libidx::groupingArtist(idx, idx.tracks[6]) == "Gorillaz",
          "the guest-artist album still groups under its real album-artist");
    CHECK(libidx::groupingArtist(idx, idx.tracks[13]) == "Muse", "ordinary album unchanged");
    // The one-argument form is untouched, for callers with no index (the scanner).
    CHECK(libidx::groupingArtist(idx.tracks[0]) == "Toto",
          "the index-free form is unchanged and still returns the track's own artist");

    // ...and the whole browse tree follows from the seam.
    const auto as = libidx::artists(idx);
    int va = 0;
    for (const auto& a : as) if (a == libidx::kVariousArtists) ++va;
    CHECK(va == 1, "ONE Various Artists row, not one per contributing artist");
    for (const auto& a : as)
        CHECK(a != "Toto" && a != "A-ha" && a != "Berlin",
              "a compilation's contributors do not appear as top-level artists: %s", a.c_str());
    CHECK(libidx::albumsForArtist(idx, libidx::kVariousArtists).size() == 2,
          "both compilations sit under it");
    CHECK(libidx::tracksForAlbum(idx, libidx::kVariousArtists, "Pure 80s").size() == 3,
          "and the album keeps all three of its tracks");
}

static void test_compilation_variants_and_threshold() {
    // Variant matching is fold-based, so case and accents do not matter.
    CHECK(libidx::isVariousish(""), "empty album-artist is 'no usable album-artist'");
    CHECK(libidx::isVariousish("Various"), "Various");
    CHECK(libidx::isVariousish("VARIOUS ARTISTS"), "case-insensitive");
    CHECK(libidx::isVariousish("Soundtrack"), "Soundtrack");
    CHECK(libidx::isVariousish("Original Motion Picture Soundtrack"), "long OST form");
    CHECK(libidx::isVariousish("VA"), "VA");
    CHECK(!libidx::isVariousish("Gorillaz"), "a real artist is not various-ish");
    CHECK(!libidx::isVariousish("Various Cruelties"), "a BAND whose name starts with Various");

    // The threshold boundary: 2 distinct artists is not enough, 3 is.
    auto build = [](int n_artists) {
        libidx::LibraryIndex i;
        for (int k = 0; k < n_artists; ++k) {
            libidx::LibraryTrack t = mk("/m/" + std::to_string(k) + ".flac",
                                        "Artist " + std::to_string(k), "Split", "");
            i.tracks.push_back(t);
        }
        libidx::rebuildCompilations(i);
        return i;
    };
    CHECK(build(2).compilations.empty(), "two artists is not a compilation");
    CHECK(build(3).compilations.size() == 1, "three is");

    // A compilation with a non-ASCII album name must round-trip through the fold.
    libidx::LibraryIndex a;
    for (const char* who : {"Sigur Rós", "Björk", "Múm"}) {
        libidx::LibraryTrack t = mk(std::string("/m/") + who + ".flac", who, "Íslensk Tónlist", "");
        a.tracks.push_back(t);
    }
    libidx::rebuildCompilations(a);
    CHECK(a.compilations.size() == 1, "non-ASCII album name detected");
    CHECK(libidx::groupingArtist(a, a.tracks[0]) == libidx::kVariousArtists, "and groups");

    // Hostile: an album with no name must never be grouped as a compilation.
    libidx::LibraryIndex e;
    for (int k = 0; k < 5; ++k)
        e.tracks.push_back(mk("/m/e" + std::to_string(k) + ".flac",
                              "A" + std::to_string(k), "", ""));
    libidx::rebuildCompilations(e);
    CHECK(e.compilations.empty(), "no album name -> never a compilation");
}

// The dedup-first rewrite must be OUTPUT-IDENTICAL, not merely faster.
static void test_artists_dedup_identical() {
    libidx::LibraryIndex idx;
    const char* names[] = {"Muse", "muse", "MUSE", "Björk", "bjork", "", "Zed", "Abe"};
    int k = 0;
    for (const char* n : names)
        for (int r = 0; r < 3; ++r)                       // duplicates, as a real index has
            idx.tracks.push_back(mk("/m/" + std::to_string(k++) + ".flac", n, "Al", "T"));
    libidx::rebuildCompilations(idx);

    // The pre-slice-8 shape: one push per track, then sortUniqueCI.
    std::vector<std::string> old;
    old.reserve(idx.tracks.size());
    for (const auto& t : idx.tracks) old.push_back(libidx::groupingArtist(idx, t));
    libidx::detail::sortUniqueCI(old);

    const auto now = libidx::artists(idx);
    CHECK(now == old, "dedup-first output is byte-identical to push-everything-then-sort");
    CHECK(now.size() < 8, "case variants still collapse to one row each: %zu rows", now.size());
}

static void test_compilation_scale() {
    libidx::LibraryIndex big;
    big.tracks.reserve(100000);
    for (std::size_t i = 0; i < 100000; ++i) {
        libidx::LibraryTrack t = mk("/m/" + std::to_string(i) + ".flac",
                                    "Artist " + std::to_string(i % 900),
                                    "Album " + std::to_string(i % 300), "T");
        // album_artist SET, so this is 300 ordinary albums rather than 300 accidental
        // compilations. Without it every album has ~333 artists and no album-artist, the
        // rule correctly flags all of them, and artists() returns ONE row - which would
        // time the wrong thing and read as a bug in the numbers.
        t.album_artist = "Artist " + std::to_string(i % 900);
        big.tracks.push_back(std::move(t));
    }
    auto t0 = std::chrono::steady_clock::now();
    libidx::rebuildCompilations(big);
    const double build_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    const auto as = libidx::artists(big);
    const double art_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    // Regression guards, deliberately generous - measured at ~13 ms and ~3 ms. The one
    // that matters is artists(): before slice 8 it was 59 ms at this size, and it is on
    // the path INTO the section.
    CHECK(build_ms < 900.0, "compilation build stayed linear: %.1f ms", build_ms);
    CHECK(art_ms   < 400.0, "artists() stayed fast: %.1f ms", art_ms);
    std::printf("  [comp scale] 100k: rebuildCompilations %.1f ms, artists() %.1f ms (%zu rows)\n",
                build_ms, art_ms, as.size());
}

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
    test_compilation_detection();
    test_compilation_variants_and_threshold();
    test_artists_dedup_identical();
    test_compilation_scale();
    test_fold_ascii();
    test_search_matching();
    test_search_cap_order_and_identity();
    test_search_scale();
    test_scale(2773,   "real");       // the measured collection: 2155 + 618
    test_scale(100000, "headroom");   // synthetic, an order of magnitude beyond

    std::printf("library_index_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
