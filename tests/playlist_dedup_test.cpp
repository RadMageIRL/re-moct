// playlist_dedup_test.cpp - LIB-S15: what "already in the playlist" means.
//
// PlaylistManager answers that question in exactly one place, indexOfPath, which
// folds through libidx::detail::foldPathKey. This file pins both halves of that
// rule, and the second half is the important one:
//
//   FOLDED:     case and separator, ON WINDOWS ONLY. NTFS is case-insensitive, so
//               `C:\A\b.flac` and `c:\a\B.flac` name one file and adding the
//               second must not make a second row. On Linux they may be two
//               genuinely different files, foldPathKey is the identity, and both
//               must be addable. Every case/separator block below therefore
//               asserts a DIFFERENT count per platform rather than being compiled
//               out on one of them - "Linux still adds both" is the constraint,
//               and a constraint nobody tests is a wish.
//
//   NOT FOLDED: THE EXTENSION, ever. `song.flac`, `song.opus` and `song.mp3` are
//               three distinct files and all three belong in the playlist. This
//               is not a happy accident of a byte-folding helper - it is measured:
//               349 groups of same-directory, same-stem, multi-format files exist
//               in the real collection, 12.6% of a 2,775-record index, and a rule
//               that merged them would eat hundreds of files. Blocks 7 and 8 exist
//               so that anyone who later "improves" indexOfPath into a stem
//               compare is stopped by a failing test on both platforms.
//
// Device-free and filesystem-free: entry paths need not exist on disk, because
// addTrack's TagLib probe fails soft and still appends the row. next_resolver_test
// relies on the same property and states it too.
#include "PlaylistManager.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond, why) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n    %s\n", \
                                         __FILE__, __LINE__, #cond, why); } \
} while (0)

// What the platform is expected to do with two paths that differ only in case or
// separator: one row on Windows, two on Linux.
#ifdef _WIN32
static const std::size_t kFoldedRows = 1;
static const bool        kFolds      = true;
#else
static const std::size_t kFoldedRows = 2;
static const bool        kFolds      = false;
#endif

int main() {
    // ── 1. byte-identical re-add: the behaviour LIB-AA's count depends on ─────
    {
        PlaylistManager pl;
        const std::string A = "C:\\m\\Artist\\01 - Song.flac";
        const std::size_t first = pl.addTrack(A);
        const std::size_t again = pl.addTrack(A);
        CHECK(pl.size() == 1, "an identical re-add must not append a second row");
        CHECK(first == again, "a denied add must return the EXISTING row's index");
        CHECK(first == 0, "the first add lands at row 0");
    }

    // ── 2. case variant - the measured defect ────────────────────────────────
    //
    // Five of the fifty-nine saved entries in the live config were exactly this:
    // one file under `C:\Users\...` and again under `c:\users\...`, identical
    // length, identical extension, differing at two bytes.
    {
        PlaylistManager pl;
        pl.addTrack("C:\\Users\\david\\Music\\A\\b.flac");
        const std::size_t at = pl.addTrack("c:\\users\\david\\Music\\A\\b.flac");
        CHECK(pl.size() == kFoldedRows,
              "Windows: one file, one row. Linux: two case-differing files, two rows");
        if (kFolds)
            CHECK(at == 0, "on Windows the case variant resolves to the existing row");
        else
            CHECK(at == 1, "on Linux the case variant is a new row of its own");
    }

    // ── 3. separator variant ─────────────────────────────────────────────────
    //
    // On Linux a backslash is an ordinary filename character, so `C:/A/b.flac`
    // and `C:\A\b.flac` really are two different names there. LIB-S11's
    // normaliseRoot was caught by the Linux gate for exactly this.
    {
        PlaylistManager pl;
        pl.addTrack("C:\\A\\b.flac");
        pl.addTrack("C:/A/b.flac");
        CHECK(pl.size() == kFoldedRows,
              "Windows: one file reached two ways. Linux: two distinct names");
    }

    // ── 4. album append accounting - LIB-AA's honest count ───────────────────
    //
    // Seed three of eighteen, then append the whole album. Fifteen rows appear,
    // not eighteen and not three. This is the property UIManager measures as
    // size()-before against size()-after, pinned here where no curses is needed.
    {
        PlaylistManager pl;
        auto trackN = [](int n) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "C:\\m\\Album\\%02d - t.flac", n);
            return std::string(buf);
        };
        pl.addIndexedTrack(trackN(2),  "Artist", "Two",   100);
        pl.addIndexedTrack(trackN(7),  "Artist", "Seven", 100);
        pl.addIndexedTrack(trackN(13), "Artist", "Thirteen", 100);
        const std::size_t before = pl.size();
        CHECK(before == 3, "three seeded rows");
        for (int n = 1; n <= 18; ++n)
            pl.addIndexedTrack(trackN(n), "Artist", "T", 100);
        CHECK(pl.size() - before == 15,
              "appending an 18-track album owning 3 of them adds exactly 15");
        CHECK(pl.size() == 18, "and the album is present exactly once over");
    }

    // ── 5. URLs are NOT path-folded ──────────────────────────────────────────
    //
    // A URL's path component is case-SENSITIVE, and foldPathKey would also turn
    // its slashes into backslashes. addTrack short-circuits http(s) to addStream
    // before the membership test, so no URL can ever reach the fold. Two stations
    // differing only in path case stay two stations, on both platforms.
    {
        PlaylistManager pl;
        pl.addTrack("https://example.com/Live/HLS.m3u8");
        pl.addTrack("https://example.com/live/hls.m3u8");
        CHECK(pl.size() == 2,
              "URLs are not paths - a case difference in a URL is a different stream");
        // ...and an identical URL still dedups, via addStream's own compare.
        pl.addTrack("https://example.com/live/hls.m3u8");
        CHECK(pl.size() == 2, "an identical URL still resolves to its existing row");
    }

    // ── 6. the directory-add paths obey the same rule ────────────────────────
    //
    // addDirectoryAsync's prefilter and drainPending each used to carry their own
    // byte compare. drainPending is the reachable one without a filesystem: it
    // appends whatever the worker produced, subject to the membership test.
    {
        PlaylistManager pl;
        pl.addTrack("C:\\Users\\david\\Music\\A\\b.flac");
        pl.addDirectoryAsync("C:\\this\\directory\\does\\not\\exist");
        while (pl.isLoading() || pl.pendingCount() > 0) pl.drainPending();
        pl.drainPending();
        CHECK(pl.size() == 1,
              "a directory that does not exist adds nothing and removes nothing");
    }

    // ── 7. FORMAT VARIANTS ARE THREE FILES - both platforms ──────────────────
    //
    // The real trio from the collection: Ace of Base "The Sign" exists on disk as
    // .flac, .mp3 and .opus in one directory. All three are separate files and
    // all three belong in the playlist.
    {
        const std::string dir = "C:\\Users\\david\\Music\\re-moct\\"
                                "Ace of Base - The Sign (1993)\\04 - The Sign";
        PlaylistManager pl;
        pl.addTrack(dir + ".flac");
        pl.addTrack(dir + ".mp3");
        pl.addTrack(dir + ".opus");
        CHECK(pl.size() == 3,
              "flac, mp3 and opus of one track are THREE files - never merge formats");

        // The same through the library route, which is how an album listing that
        // holds several formats of one track reaches the playlist.
        PlaylistManager pi;
        pi.addIndexedTrack(dir + ".flac", "Ace of Base", "The Sign", 190);
        pi.addIndexedTrack(dir + ".mp3",  "Ace of Base", "The Sign", 190);
        pi.addIndexedTrack(dir + ".opus", "Ace of Base", "The Sign", 190);
        CHECK(pi.size() == 3,
              "addIndexedTrack must not merge formats either - same rule, same helper");

        // A four-format set, the .m4a case included (Da Doo Ron Ron is .flac,
        // .m4a and .opus on disk).
        PlaylistManager pm;
        const std::string d2 = "C:\\Users\\david\\Music\\Crystals, The\\"
                               "Playlist; The Very Best Of The Crystals\\"
                               "01 Crystals, The - Da Doo Ron Ron";
        pm.addTrack(d2 + ".flac");
        pm.addTrack(d2 + ".m4a");
        pm.addTrack(d2 + ".opus");
        CHECK(pm.size() == 3, "flac, m4a and opus are three files too");
    }

    // ── 8. THE TRAP: case AND extension differing at once ────────────────────
    //
    // Taken verbatim from the live config, where these two lines coexist. They
    // differ in the drive-letter case AND in the extension. A stem compare - the
    // shape someone reaches for when "the same track keeps showing up" - merges
    // them and silently deletes a file Dos deliberately keeps.
    //
    // Two rows. On EVERY platform. This is the block that must never be relaxed.
    {
        PlaylistManager pl;
        pl.addTrack("C:\\Users\\david\\Music\\Joan Osborne\\Relish\\"
                    "06 Joan Osborne - One of Us.mp3");
        pl.addTrack("c:\\users\\david\\Music\\Joan Osborne\\Relish\\"
                    "06 Joan Osborne - One of Us.flac");
        CHECK(pl.size() == 2,
              "differing in case AND extension = two files, on Windows and Linux alike");

        // ...and the second one, also live: same directory, same stem, .flac and
        // .opus, identical case throughout. Nothing to fold, and still two rows.
        PlaylistManager pj;
        const std::string js = "c:\\users\\david\\Music\\re-moct\\"
                               "Jermaine Stewart - Frantic Romantic (1986)\\"
                               "01 - We Don't Have to Take Our Clothes Off";
        pj.addTrack(js + ".flac");
        pj.addTrack(js + ".opus");
        CHECK(pj.size() == 2, "same stem, two formats, two rows");
    }

    // ── 9. a denied add does not disturb the current row ─────────────────────
    //
    // Enter on a library track already present calls selectAt(returned index),
    // so the returned index has to be the row that is really there.
    {
        PlaylistManager pl;
        pl.addTrack("C:\\m\\one.flac");
        pl.addTrack("C:\\m\\two.flac");
        pl.addTrack("C:\\m\\three.flac");
        pl.selectAt(2);
        const std::size_t at = pl.addTrack("c:\\m\\ONE.flac");
        if (kFolds) {
            CHECK(pl.size() == 3, "no row appended");
            CHECK(at == 0, "the caller is handed row 0, the file it asked for");
        } else {
            CHECK(pl.size() == 4, "Linux: a fourth, genuinely different file");
            CHECK(at == 3, "appended at the end");
        }
        CHECK(pl.current() == 2, "the playing row is not moved by an add");
    }

    if (g_fail == 0) std::printf("playlist_dedup_test: all checks passed\n");
    else             std::printf("playlist_dedup_test: %d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
