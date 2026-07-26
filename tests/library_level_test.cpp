// library_level_test - proves LibraryNav.h headless and device-free: no curses, no
// filesystem, no threads, no UIManager. Library slice 4 is depth, and depth is
// three levels times two directions, which is where an off-by-one lives. Written
// inside a key handler none of it would be reachable from a test; the point of the
// helper existing at all is this file.
//
// The obligations, in order of what would actually hurt:
//
//   1. THE STALENESS RULE. `libidx::tracksForAlbum` returns INDICES into the
//      index, valid only against the instance that produced them. Every identity
//      this slice stores must be a STRING, and a level-3 identity must be a path
//      that is really in the index. A stored index is the bug three slices of work
//      avoided, so it is asserted rather than trusted.
//   2. THE REMEMBERED CURSORS. Three of them, because one cannot survive descent.
//      Descending into a DIFFERENT album must not restore a track cursor from the
//      previous one.
//   3. LATIN-1 TAG TEXT. Levels 1-2 identities come from tags and may be invalid
//      UTF-8. They are stored and compared as bytes here and must never be
//      touched by anything that could throw.
//
// Header-only, both matrix jobs.

#include "LibraryNav.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

using namespace libnav;

// ── A small synthetic index ─────────────────────────────────────────────────
// Two artists, one of them with two albums, and an album title shared across both
// artists so the collision case is real rather than described.
static libidx::LibraryTrack mk(const std::string& path, const std::string& artist,
                               const std::string& album, const std::string& title,
                               int track_no = 0, int dur = 0) {
    libidx::LibraryTrack t;
    t.path = path; t.artist = artist; t.album = album; t.title = title;
    t.track_no = track_no; t.duration_sec = dur;
    return t;
}

static libidx::LibraryIndex makeIndex() {
    libidx::LibraryIndex idx;
    idx.root = "/music";
    idx.tracks = {
        mk("/music/Muse/Absolution/01 Apocalypse Please.flac", "Muse", "Absolution", "Apocalypse Please", 1, 201),
        mk("/music/Muse/Absolution/02 Time Is Running Out.flac", "Muse", "Absolution", "Time Is Running Out", 2, 237),
        mk("/music/Muse/Origin/01 Sunburn.flac",               "Muse", "Origin of Symmetry", "Sunburn", 1, 214),
        mk("/music/VA/Greatest Hits/01 A.flac",                "Queen", "Greatest Hits", "Bohemian Rhapsody", 1, 355),
        mk("/music/Muse/GH/01 B.flac",                         "Muse", "Greatest Hits", "Starlight", 1, 240),
    };
    return idx;
}

// ── 1. The six transitions ──────────────────────────────────────────────────
static void test_descend_and_ascend() {
    State s;
    CHECK(s.level == Level::Artists, "starts at level 1");
    CHECK(s.artist.empty() && s.album.empty(), "starts with no path taken");

    CHECK(descend(s, "Muse") == Action::Repopulate, "descend artist repopulates");
    CHECK(s.level == Level::Albums, "artist -> albums");
    CHECK(s.artist == "Muse", "artist recorded");
    CHECK(s.album.empty(), "album not set yet");

    CHECK(descend(s, "Absolution") == Action::Repopulate, "descend album repopulates");
    CHECK(s.level == Level::Tracks, "albums -> tracks");
    CHECK(s.artist == "Muse" && s.album == "Absolution", "full path taken recorded");

    // Enter at level 3 does NOT navigate in slice 4 - playback is slice 5.
    CHECK(descend(s, "/music/Muse/Absolution/01 Apocalypse Please.flac") == Action::None,
          "descend at tracks is a no-op action");
    CHECK(s.level == Level::Tracks, "and does not change level");

    CHECK(ascend(s) == Action::Repopulate, "ascend from tracks repopulates");
    CHECK(s.level == Level::Albums, "tracks -> albums");
    CHECK(s.album.empty(), "album cleared on the way up - never stale at the level that reads it");
    CHECK(s.artist == "Muse", "artist still held");

    CHECK(ascend(s) == Action::Repopulate, "ascend from albums repopulates");
    CHECK(s.level == Level::Artists, "albums -> artists");
    CHECK(s.artist.empty() && s.album.empty(), "both cleared");

    CHECK(ascend(s) == Action::LeaveSection, "ascend from artists leaves the section");
    CHECK(s.level == Level::Artists, "and stays at level 1");
}

// ── 2. The remembered cursors ───────────────────────────────────────────────
static void test_remembered_cursor_roundtrip() {
    State s;
    descend(s, "Muse");
    descend(s, "Absolution");
    descend(s, "/music/Muse/Absolution/02 Time Is Running Out.flac");

    CHECK(s.sel_artist == "Muse", "artist remembered");
    CHECK(s.sel_album == "Absolution", "album remembered");
    CHECK(s.sel_track == "/music/Muse/Absolution/02 Time Is Running Out.flac", "track remembered");

    ascend(s);   // -> Albums
    CHECK(s.sel_album == "Absolution", "ascending keeps the album cursor to land on");
    CHECK(s.sel_track.find("Time Is Running Out") != std::string::npos,
          "and keeps the track cursor for a re-descent into the SAME album");

    // Re-descend into the same album: the track cursor must survive.
    descend(s, "Absolution");
    CHECK(s.sel_track.find("Time Is Running Out") != std::string::npos,
          "same album re-descended -> track cursor restored, not thrown away");

    ascend(s); ascend(s);   // -> Artists
    CHECK(s.sel_artist == "Muse", "artist cursor survives to land on");
    descend(s, "Muse");
    CHECK(s.sel_album == "Absolution", "same artist re-descended -> album cursor survives");
}

static void test_different_selection_discards_deeper_cursor() {
    State s;
    descend(s, "Muse");
    descend(s, "Absolution");
    descend(s, "/music/Muse/Absolution/01 Apocalypse Please.flac");
    ascend(s);   // -> Albums, sel_track still set

    // THE CASE THAT WOULD LOOK LIKE A BUG: a different album must not inherit the
    // previous album's track cursor.
    descend(s, "Origin of Symmetry");
    CHECK(s.sel_track.empty(), "different album -> track cursor discarded");
    CHECK(s.sel_album == "Origin of Symmetry", "and the album cursor updated");

    // Same, one level up: a different artist discards BOTH deeper cursors.
    State t;
    descend(t, "Muse");
    descend(t, "Absolution");
    descend(t, "/music/Muse/Absolution/01 Apocalypse Please.flac");
    ascend(t); ascend(t);   // -> Artists
    descend(t, "Queen");
    CHECK(t.sel_album.empty(), "different artist -> album cursor discarded");
    CHECK(t.sel_track.empty(), "different artist -> track cursor discarded");
    CHECK(t.sel_artist == "Queen", "and the artist cursor updated");
}

static void test_selection_compare_is_case_insensitive() {
    // The compare must be the same one restoreCursor uses, or a row could match
    // for cursor restore and not for level identity - a cursor that jumps.
    State s;
    descend(s, "Muse");
    descend(s, "Absolution");
    descend(s, "/music/Muse/Absolution/01 Apocalypse Please.flac");
    ascend(s);
    descend(s, "ABSOLUTION");     // same album, different case
    CHECK(!s.sel_track.empty(), "case-different album is the SAME album - cursor kept");
    CHECK(sameName("Muse", "muse"), "sameName is case-insensitive");
    CHECK(!sameName("Muse", "Mus"), "and not a prefix match");
}

// ── 3. The staleness rule, asserted rather than trusted ─────────────────────
static void test_no_index_escapes_as_identity() {
    const libidx::LibraryIndex idx = makeIndex();

    // What showLibraryTracks does: query, then convert to path identities inside
    // the one call, and let the index vector die.
    const std::vector<std::size_t> hits = libidx::tracksForAlbum(idx, "Muse", "Absolution");
    CHECK(hits.size() == 2, "two tracks on that album, got %zu", hits.size());

    std::vector<std::string> identities;
    for (std::size_t i : hits) identities.push_back(idx.tracks[i].path);

    // EVERY identity must be a path that is really in the index. An index that
    // leaked through as a number would fail this by not being a path at all.
    for (const auto& id : identities) {
        bool found = false;
        for (const auto& t : idx.tracks) if (t.path == id) { found = true; break; }
        CHECK(found, "identity is a real indexed path: %s", id.c_str());
        CHECK(id.find('/') != std::string::npos || id.find('\\') != std::string::npos,
              "identity looks like a path, not a subscript: %s", id.c_str());
    }

    // And the re-query key is the pair of strings the UI holds, so a rebuild needs
    // no invalidation: the same query from the same two strings is stable.
    const std::vector<std::size_t> again = libidx::tracksForAlbum(idx, "Muse", "Absolution");
    CHECK(again == hits, "re-query from held strings is stable");
}

static void test_album_shared_across_artists_does_not_collide() {
    const libidx::LibraryIndex idx = makeIndex();
    const auto q = libidx::tracksForAlbum(idx, "Queen", "Greatest Hits");
    const auto m = libidx::tracksForAlbum(idx, "Muse",  "Greatest Hits");
    CHECK(q.size() == 1 && m.size() == 1, "one track each, got %zu and %zu", q.size(), m.size());
    CHECK(q[0] != m[0], "same album name under two artists is two disjoint listings");
    CHECK(idx.tracks[q[0]].title == "Bohemian Rhapsody", "Queen's row");
    CHECK(idx.tracks[m[0]].title == "Starlight", "Muse's row");
}

// ── 4. Latin-1 tag text survives as bytes ───────────────────────────────────
static void test_latin1_identity_survives() {
    // A raw CP1252 right single quote (0x92) and an e-acute (0xE9): invalid UTF-8,
    // the exact bytes that throw out of fs::path on Windows.
    const std::string album  = "Don\x92t Look Back";
    const std::string artist = "Bj\xE9rk";

    State s;
    descend(s, artist);
    CHECK(s.artist == artist, "Latin-1 artist stored byte-exact");
    descend(s, album);
    CHECK(s.album == album, "Latin-1 album stored byte-exact");
    CHECK(s.sel_artist == artist && s.sel_album == album, "and remembered byte-exact");

    ascend(s);
    descend(s, album);
    CHECK(s.album == album, "and compares equal to itself, so the cursor is kept");

    // Bytes above 0x7F must not be case-folded into each other by the compare.
    CHECK(!sameName("Bj\xE9rk", "Bj\xF9rk"), "distinct high bytes stay distinct");
}

// ── 5. The level-3 row label, whose degenerate cases are the normal cases ───
static void test_track_row_label() {
    libidx::LibraryTrack t = mk("/music/A/B/03 Song.flac", "A", "B", "Song Title", 3, 201);
    CHECK(trackRowLabel(t, "3:21") == "03. Song Title  (3:21)", "full row: [%s]",
          trackRowLabel(t, "3:21").c_str());

    t.track_no = 0;
    CHECK(trackRowLabel(t, "3:21") == "Song Title  (3:21)", "no track number, and never \"0. \": [%s]",
          trackRowLabel(t, "3:21").c_str());

    t.track_no = 12;
    CHECK(trackRowLabel(t, "3:21") == "12. Song Title  (3:21)", "two-digit number not zero-padded further");

    t.title.clear();
    CHECK(trackRowLabel(t, "3:21") == "12. 03 Song  (3:21)", "empty title falls back to the stem: [%s]",
          trackRowLabel(t, "3:21").c_str());

    CHECK(trackRowLabel(t, "") == "12. 03 Song", "zero duration omits the parenthetical");

    // The whole row missing everything but a filename - an untagged rip.
    libidx::LibraryTrack u = mk("/music/unknown/track07.mp3", "", "", "");
    CHECK(trackRowLabel(u, "") == "track07", "untagged: stem only, no crash: [%s]",
          trackRowLabel(u, "").c_str());

    // Slice 8: on a COMPILATION the per-track artist goes on the row, because there it
    // differs per track and a list of bare titles cannot say who is performing.
    libidx::LibraryTrack c = mk("/m/p/05 Take On Me.flac", "A-ha", "Pure 80s", "Take On Me", 5, 225);
    CHECK(trackRowLabel(c, "3:45", false) == "05. Take On Me  (3:45)",
          "ordinary album: no artist, as before: [%s]", trackRowLabel(c, "3:45", false).c_str());
    CHECK(trackRowLabel(c, "3:45", true) == "05. A-ha - Take On Me  (3:45)",
          "compilation: artist between the number and the title: [%s]",
          trackRowLabel(c, "3:45", true).c_str());
    // Default is off, so every existing caller is unchanged.
    CHECK(trackRowLabel(c, "3:45") == trackRowLabel(c, "3:45", false), "default is off");
    // A compilation track with no artist tag must not produce a dangling separator.
    libidx::LibraryTrack n = mk("/m/p/06 Unknown.flac", "", "Pure 80s", "Unknown", 6, 100);
    CHECK(trackRowLabel(n, "", true) == "06. Unknown",
          "no artist -> no \" - \" left behind: [%s]", trackRowLabel(n, "", true).c_str());
}

static void test_path_stem() {
    CHECK(pathStem("/a/b/c.flac") == "c", "posix stem");
    CHECK(pathStem("C:\\a\\b\\c.flac") == "c", "windows stem");
    CHECK(pathStem("noslash.mp3") == "noslash", "bare filename");
    CHECK(pathStem("/a/b/noext") == "noext", "no extension");
    CHECK(pathStem("/a/b/.hidden") == ".hidden", "leading dot is not an extension");
    CHECK(pathStem("/a/b/two.dots.ogg") == "two.dots", "only the last dot");
    CHECK(pathStem("").empty(), "empty in, empty out, no crash");
    CHECK(pathStem("/a/b/") .empty(), "trailing separator, no crash");
}

// ── 5b. albumTracks - THE one ordering function (LIB-AA) ────────────────────
// Both the level-3 display and the album append read this, so these assertions are
// what makes "it appends in the order you see" true rather than hoped for.
static void test_album_tracks_order() {
    libidx::LibraryIndex idx;
    idx.root = "/music";
    // Deliberately shuffled input across two discs, so ordering is being proved and
    // not merely inherited from insertion order.
    idx.tracks = {
        mk("/m/d2t02.flac", "A", "Boxset", "D2T2", 2, 100),
        mk("/m/d1t03.flac", "A", "Boxset", "D1T3", 3, 100),
        mk("/m/d2t01.flac", "A", "Boxset", "D2T1", 1, 100),
        mk("/m/d1t01.flac", "A", "Boxset", "D1T1", 1, 100),
        mk("/m/d1t02.flac", "A", "Boxset", "D1T2", 2, 100),
    };
    idx.tracks[0].disc_no = 2; idx.tracks[2].disc_no = 2;
    idx.tracks[1].disc_no = 1; idx.tracks[3].disc_no = 1; idx.tracks[4].disc_no = 1;

    const auto rows = albumTracks(idx, "A", "Boxset");
    CHECK(rows.size() == 5, "all five tracks, got %zu", rows.size());
    // DISC then track - not track then disc. A multi-disc set must not interleave.
    CHECK(rows[0].title == "D1T1" && rows[1].title == "D1T2" && rows[2].title == "D1T3",
          "disc 1 in track order first: [%s %s %s]",
          rows[0].title.c_str(), rows[1].title.c_str(), rows[2].title.c_str());
    CHECK(rows[3].title == "D2T1" && rows[4].title == "D2T2",
          "then disc 2 in track order: [%s %s]",
          rows[3].title.c_str(), rows[4].title.c_str());

    // NO SUBSCRIPT ESCAPES: every returned record must correspond to a real indexed
    // path. An index leaking out as a number could not satisfy this.
    for (const auto& r : rows) {
        bool found = false;
        for (const auto& t : idx.tracks) if (t.path == r.path) { found = true; break; }
        CHECK(found, "record carries a real indexed path: %s", r.path.c_str());
    }
}

static void test_album_tracks_untagged_and_duplicate_numbers() {
    libidx::LibraryIndex idx;
    // Every track_no 0 - the untagged rip. Order must fall to title then path and be
    // total, so the list is stable between two calls.
    idx.tracks = {
        mk("/m/03 c.flac", "A", "Al", ""),
        mk("/m/01 a.flac", "A", "Al", ""),
        mk("/m/02 b.flac", "A", "Al", ""),
    };
    const auto a = albumTracks(idx, "A", "Al");
    const auto b = albumTracks(idx, "A", "Al");
    CHECK(a.size() == 3, "three untagged tracks");
    CHECK(a[0].path < a[1].path && a[1].path < a[2].path,
          "untagged falls to filename order: [%s %s %s]",
          a[0].path.c_str(), a[1].path.c_str(), a[2].path.c_str());
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) same = (a[i].path == b[i].path);
    CHECK(same, "and the order is stable across calls");

    // Duplicated track numbers must still produce a total order, losing and repeating
    // nothing.
    libidx::LibraryIndex dup;
    dup.tracks = {
        mk("/m/y.flac", "A", "Al", "Y", 1),
        mk("/m/x.flac", "A", "Al", "X", 1),
    };
    const auto d = albumTracks(dup, "A", "Al");
    CHECK(d.size() == 2, "both rows kept despite the same track number");
    CHECK(d[0].title == "X" && d[1].title == "Y", "tie broken by title: [%s %s]",
          d[0].title.c_str(), d[1].title.c_str());
}

static void test_album_tracks_edge_cases() {
    const libidx::LibraryIndex idx = makeIndex();
    CHECK(albumTracks(idx, "Nobody", "Nothing").empty(), "unknown artist+album -> empty");
    CHECK(albumTracks(idx, "Muse", "Nothing").empty(), "known artist, unknown album -> empty");
    CHECK(albumTracks(libidx::LibraryIndex{}, "A", "B").empty(), "empty index -> empty");

    // Shared album name stays disjoint at the record level too.
    CHECK(albumTracks(idx, "Queen", "Greatest Hits").size() == 1, "Queen's one row");
    CHECK(albumTracks(idx, "Muse", "Greatest Hits").size() == 1, "Muse's one row");
    CHECK(albumTracks(idx, "Queen", "Greatest Hits")[0].path !=
          albumTracks(idx, "Muse", "Greatest Hits")[0].path, "and they are different rows");

    // A Latin-1 album NAME selects correctly and is never turned into a path.
    libidx::LibraryIndex l1;
    const std::string bad = "Don\x92t Look Back";
    l1.tracks = { mk("/m/one.flac", "A", bad, "T", 1) };
    const auto r = albumTracks(l1, "A", bad);
    CHECK(r.size() == 1, "invalid-UTF-8 album name matches by bytes");
    CHECK(r[0].album == bad, "and round-trips byte-exact");
}

// ── 6. rowIsPath - slice 5's entire safety, in one predicate ────────────────
static void test_row_is_path() {
    // Only level 3 identities are filesystem paths. Levels 1-2 are tag text, which
    // may be invalid UTF-8, and handing one to fs::path throws on Windows. This
    // predicate is what browserEntryPath consults, so a flipped polarity here is a
    // crash and not a cosmetic bug - which is the reason it is a named function
    // rather than an open-coded comparison inside a curses handler.
    CHECK(!rowIsPath(Level::Artists), "an artist row is NOT a path");
    CHECK(!rowIsPath(Level::Albums),  "an album row is NOT a path");
    CHECK(rowIsPath(Level::Tracks),   "a track row IS a path");

    // And it agrees with where descend actually leaves you, so the two cannot drift.
    State s;
    CHECK(!rowIsPath(s.level), "fresh state is not actionable");
    descend(s, "Muse");
    CHECK(!rowIsPath(s.level), "after descending to albums, still not actionable");
    descend(s, "Absolution");
    CHECK(rowIsPath(s.level), "after descending to tracks, actionable");
    ascend(s);
    CHECK(!rowIsPath(s.level), "ascending back out of tracks stops being actionable");
}

// ── 6b. Search results as a fourth level (slice 7) ──────────────────────────
static void test_search_level() {
    // rowIsPath must include Results, because a result row's identity is a real path -
    // that one predicate is what turns on play/append/queue/mark/convert for results.
    CHECK(rowIsPath(Level::Results), "a result row IS a path");
    CHECK(!rowIsPath(Level::Artists) && !rowIsPath(Level::Albums),
          "levels 1-2 are still tag text, still excluded");

    // return_to is captured at entry, so ascending lands where the search started.
    State s;
    descend(s, "Muse");                       // -> Albums
    beginSearch(s, "sabotage");
    CHECK(s.level == Level::Results, "search enters Results");
    CHECK(s.return_to == Level::Albums, "and remembers where it came from");
    CHECK(s.query == "sabotage", "query held as a STRING, so results re-derive");
    CHECK(ascend(s) == Action::Repopulate, "ascending from Results repopulates");
    CHECK(s.level == Level::Albums, "and returns to the level the search started at");
    CHECK(s.query.empty(), "leaving a search drops the query");

    // From level 3.
    State t;
    descend(t, "Muse"); descend(t, "Absolution");
    CHECK(t.level == Level::Tracks, "at tracks");
    beginSearch(t, "x");
    CHECK(t.return_to == Level::Tracks, "returns to tracks");
    ascend(t);
    CHECK(t.level == Level::Tracks, "landed back at tracks");

    // Re-searching from inside results must not overwrite return_to with Results, or
    // ascending would loop back into the search it was leaving.
    State u;
    beginSearch(u, "one");
    beginSearch(u, "two");
    CHECK(u.return_to == Level::Artists, "a second search keeps the original return level");
    CHECK(u.query == "two", "and takes the new query");

    // Enter on a result is a leaf action, not a descent.
    State v;
    beginSearch(v, "q");
    CHECK(descend(v, "/m/a.flac") == Action::None, "Enter at Results does not descend");
    CHECK(v.level == Level::Results, "and stays put");
    CHECK(v.sel_track == "/m/a.flac", "but remembers the row for cursor restore");
}

// ── 7. The reset trap ───────────────────────────────────────────────────────
static void test_reset() {
    State s;
    descend(s, "Muse");
    descend(s, "Absolution");
    descend(s, "/music/Muse/Absolution/01 Apocalypse Please.flac");

    reset(s);
    CHECK(s.level == Level::Artists, "reset returns to level 1");
    CHECK(s.artist.empty(), "reset clears the artist");
    CHECK(s.album.empty(), "reset clears the album");
    CHECK(s.sel_album.empty(), "reset clears the album cursor");
    CHECK(s.sel_track.empty(), "reset clears the track cursor");
    // Deliberate: sel_artist is a section-level memory and matches shipped
    // slice-3 behaviour, where refreshDir never cleared lib_selected_.
    CHECK(s.sel_artist == "Muse", "reset deliberately KEEPS the artist cursor");

    // Slice 7: a live search must not survive a reset either, or a refresh would leave
    // the section holding a query it is no longer showing results for.
    State q;
    beginSearch(q, "something");
    reset(q);
    CHECK(q.query.empty(), "reset clears the query");
    CHECK(q.return_to == Level::Artists, "and the return level");
    CHECK(q.level == Level::Artists, "and lands at level 1");
}

int main() {
    test_descend_and_ascend();
    test_remembered_cursor_roundtrip();
    test_different_selection_discards_deeper_cursor();
    test_selection_compare_is_case_insensitive();
    test_no_index_escapes_as_identity();
    test_album_shared_across_artists_does_not_collide();
    test_latin1_identity_survives();
    test_track_row_label();
    test_path_stem();
    test_album_tracks_order();
    test_album_tracks_untagged_and_duplicate_numbers();
    test_album_tracks_edge_cases();
    test_row_is_path();
    test_search_level();
    test_reset();

    std::printf("library_level_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
