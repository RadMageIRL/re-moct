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

// ── 6. The reset trap ───────────────────────────────────────────────────────
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
    test_reset();

    std::printf("library_level_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
