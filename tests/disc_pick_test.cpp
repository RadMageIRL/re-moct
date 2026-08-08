// disc_pick_test - P3 Part 1: the silent tie, and the three places that now
// break the silence.
//
// THE DEFECT. pickDiscForTrackCount identifies the medium in the drive by
// matching its track count against each medium of the release, and falls back to
// disc 1 when zero or several fit. That fallback was documented and correct as a
// choice - and completely silent. Mellon Collie is the case that cost a real rip:
// both media hold the same number of tracks, so ripping disc 2 matched nothing
// uniquely, disc 1 was assumed, and every title was written from disc 1's
// tracklist with nothing said in the log, the sidecar, or on screen.
//
// The fixtures are the two real discs behind this work:
//   * FINAL FANTASY XI PREMIUM BOX - 7 media, 21/30/19/24/21/18/10 tracks.
//     19 is unique (disc 3, determined) and 21 is not (a tie between media 1
//     and 5, assumed). One release exercises both outcomes.
//   * Mellon Collie - 2 media of equal length, the motivating case.
//
// WHAT IS ASSERTED. Not just the predicate: the EXACT TEXT of all three
// reporting surfaces, because the text is this slice's deliverable. A pick that
// knows it is ambiguous and a log line that does not say so would still be the
// bug. discLogLine / discSourceLabel / discAmbiguityNote are the functions the
// ripper and the UI actually call, so what is pinned here is what ships.
//
// Header-only: MBLookup.h's free functions have no dependencies beyond std, so
// this links nothing.

#include "MBLookup.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& got = "") {
    std::printf("  [%s] %-58s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                got.empty() ? "" : "  got: ", got.c_str());
    if (!ok) ++g_fail;
}

// Build a release whose media hold the given track counts, numbered within each
// medium exactly as MBLookup::parseJson tags them.
static MBRelease releaseWithMedia(const std::vector<int>& counts) {
    MBRelease r;
    r.title = "fixture";
    int disc = 0;
    for (int n : counts) {
        ++disc;
        for (int i = 1; i <= n; ++i) {
            MBTrack t;
            t.number = i;
            t.disc   = disc;
            t.title  = "d" + std::to_string(disc) + "t" + std::to_string(i);
            r.tracks.push_back(t);
        }
    }
    return r;
}

int main() {
    // ── 1. The determined case: FFXI disc 3 is the only 19-track medium ──────
    std::printf("\n-- FFXI PREMIUM BOX (21/30/19/24/21/18/10), disc of 19 tracks --\n");
    {
        const MBRelease ffxi = releaseWithMedia({21, 30, 19, 24, 21, 18, 10});
        const DiscPick  p    = pickDisc(ffxi, 19);

        check(p.disc == 3,     "picks disc 3",            std::to_string(p.disc));
        check(p.total == 7,    "reports 7 media",         std::to_string(p.total));
        check(p.matches == 1,  "exactly one medium fits", std::to_string(p.matches));
        check(!p.ambiguous(),  "not ambiguous");
        check(std::string(discSourceLabel(p)) == "unique_track_count",
              "disc_source = unique_track_count", discSourceLabel(p));
        check(discLogLine(p, 19) == "3 of 7 (matched by track count)",
              "log line names the match", discLogLine(p, 19));
        check(discAmbiguityNote(p, 19).empty(),
              "no on-screen note when determined", discAmbiguityNote(p, 19));

        // The contract the rest of the tree depends on is untouched.
        check(pickDiscForTrackCount(ffxi, 19) == 3,
              "pickDiscForTrackCount unchanged (3)",
              std::to_string(pickDiscForTrackCount(ffxi, 19)));
    }

    // ── 2. The tie, inside that same release: 21 tracks fits media 1 AND 5 ───
    std::printf("\n-- same box, a disc of 21 tracks (media 1 and 5 both fit) --\n");
    {
        const MBRelease ffxi = releaseWithMedia({21, 30, 19, 24, 21, 18, 10});
        const DiscPick  p    = pickDisc(ffxi, 21);

        check(p.matches == 2,  "two media fit",           std::to_string(p.matches));
        check(p.disc == 1,     "falls back to disc 1",    std::to_string(p.disc));
        check(p.ambiguous(),   "IS ambiguous");
        check(std::string(discSourceLabel(p)) == "ambiguous_fallback",
              "disc_source = ambiguous_fallback", discSourceLabel(p));
        check(discLogLine(p, 21) ==
              "1 of 7  ** AMBIGUOUS - 2 media have 21 tracks; disc 1 assumed, "
              "titles may be wrong **",
              "log line says the disc was ASSUMED", discLogLine(p, 21));
        check(discAmbiguityNote(p, 21) ==
              "Disc ambiguous - 2 discs have 21 tracks; assumed disc 1, "
              "titles may be wrong",
              "on-screen note is raised", discAmbiguityNote(p, 21));
        // The colour path in drawStatus() keys on this word.
        check(discAmbiguityNote(p, 21).find("ambiguous") != std::string::npos,
              "note carries the marker drawStatus() colours on");
    }

    // ── 3. Mellon Collie: the disc this whole slice exists for ──────────────
    std::printf("\n-- Mellon Collie: two media, same track count --\n");
    {
        const MBRelease mc = releaseWithMedia({14, 14});
        const DiscPick  p  = pickDisc(mc, 14);

        check(p.total == 2 && p.matches == 2, "both media fit 14 tracks",
              std::to_string(p.matches) + " of " + std::to_string(p.total));
        check(p.disc == 1,   "disc 1 assumed - ripping disc 2 gets disc 1's titles");
        check(p.ambiguous(), "IS ambiguous - and now says so");
        check(discLogLine(p, 14) ==
              "1 of 2  ** AMBIGUOUS - 2 media have 14 tracks; disc 1 assumed, "
              "titles may be wrong **",
              "the line that was missing from every rip log", discLogLine(p, 14));
    }

    // ── 4. No medium fits at all - the other ambiguous shape ────────────────
    std::printf("\n-- a disc matching no medium (usually the wrong release) --\n");
    {
        const MBRelease r = releaseWithMedia({12, 15, 9});
        const DiscPick  p = pickDisc(r, 20);

        check(p.matches == 0, "no medium fits",        std::to_string(p.matches));
        check(p.disc == 1,    "disc 1 assumed");
        check(p.ambiguous(),  "IS ambiguous");
        check(std::string(discSourceLabel(p)) == "ambiguous_fallback",
              "disc_source = ambiguous_fallback", discSourceLabel(p));
        check(discLogLine(p, 20) ==
              "1 of 3  ** AMBIGUOUS - no medium has 20 tracks; disc 1 assumed, "
              "titles may be wrong **",
              "log line distinguishes NO match from a tie", discLogLine(p, 20));
        check(discAmbiguityNote(p, 20) ==
              "Disc ambiguous - no disc has 20 tracks; assumed disc 1, "
              "titles may be wrong",
              "note distinguishes it too", discAmbiguityNote(p, 20));
    }

    // ── 5. Single-medium releases: nothing to be ambiguous about ────────────
    // Including the count-disagrees case, which is a wrong-release problem and
    // deliberately NOT reported as an ambiguous medium.
    std::printf("\n-- single-medium releases, and no release at all --\n");
    {
        const MBRelease one = releaseWithMedia({19});
        const DiscPick  p   = pickDisc(one, 19);
        check(p.disc == 1 && p.total == 1, "1 of 1");
        check(!p.ambiguous(), "not ambiguous");
        check(discLogLine(p, 19) == "1 of 1",
              "log line is plain, no match parenthetical", discLogLine(p, 19));
        check(discAmbiguityNote(p, 19).empty(), "no note");

        const DiscPick q = pickDisc(one, 25);   // count disagrees
        check(!q.ambiguous(),
              "single medium with a WRONG count is still not 'ambiguous disc'");
        check(q.disc == 1, "still disc 1", std::to_string(q.disc));

        // No MusicBrainz lookup at all - rel is empty.
        const DiscPick e = pickDisc(MBRelease{}, 19);
        check(e.disc == 1 && e.total == 1 && !e.ambiguous(),
              "empty release: 1 of 1, not ambiguous");
        check(discLogLine(e, 19) == "1 of 1", "empty release logs 1 of 1",
              discLogLine(e, 19));
    }

    // ── 6. A person chooses the medium: the picker's whole output ───────────
    // withUserDisc is the ONE way "chosen" is expressed. It clears ambiguous(),
    // because an answer given by the user is determined whatever the counts say
    // - and that single flag is what turns the log line, the sidecar value and
    // the on-screen note all around at once.
    std::printf("\n-- the user chooses disc 2 of Mellon Collie --\n");
    {
        const DiscPick before = pickDisc(releaseWithMedia({14, 14}), 14);
        check(before.ambiguous() && !before.user, "before: ambiguous, not user-set");
        check(std::string(discSourceLabel(before)) == "ambiguous_fallback",
              "before: ambiguous_fallback", discSourceLabel(before));

        const DiscPick after = withUserDisc(before, 2);
        check(after.disc == 2 && after.user, "after: disc 2, user-set",
              std::to_string(after.disc));
        check(!after.ambiguous(), "after: NOT ambiguous - a person decided");
        check(std::string(discSourceLabel(after)) == "user",
              "after: disc_source = user", discSourceLabel(after));
        check(discLogLine(after, 14) == "2 of 2 (chosen)",
              "log line says chosen, not ASSUMED", discLogLine(after, 14));
        check(discAmbiguityNote(after, 14).empty(),
              "no warning once chosen", discAmbiguityNote(after, 14));

        // A disc outside the set is refused rather than trusted.
        const DiscPick bad = withUserDisc(before, 9);
        check(!bad.user && bad.disc == 1, "a disc outside the set is ignored",
              std::to_string(bad.disc));
    }

    // ── 7. The disc column: what a row says BEFORE it is chosen ─────────────
    // Shapes taken from the MEASURED Mellon Collie candidate list. All four
    // releases are ambiguous for a 14-track disc, so every row must show "?" and
    // none may claim a disc it cannot resolve.
    std::printf("\n-- disc column, on the measured Mellon Collie candidates --\n");
    {
        struct Row { std::vector<int> media; const char* want; };
        const Row rows[] = {
            { {14,14,21,20,23,15}, "?/6" },   // 2012 box - what releases[0] is today
            { {14,14},             "?/2" },   // the plain 2-disc
            { {14,14,8,6},         "?/4" },   // 30th anniversary
            { {14,14,8,6},         "?/4" },   // and its near-twin
        };
        for (const auto& r : rows) {
            const std::string got = discColumn(pickDisc(releaseWithMedia(r.media), 14));
            check(got == r.want,
                  std::to_string(r.media.size()) + " media -> " + r.want, got);
        }
        const MBRelease ffxi = releaseWithMedia({21,30,19,24,21,18,10});
        check(discColumn(pickDisc(ffxi, 19)) == "3/7", "FFXI 19-track disc -> 3/7",
              discColumn(pickDisc(ffxi, 19)));
        check(discColumn(pickDisc(releaseWithMedia({19}), 19)).empty(),
              "a single-medium release has no disc column");
        const DiscPick chosen = withUserDisc(pickDisc(releaseWithMedia({14,14}), 14), 2);
        check(discColumn(chosen) == "2/2", "once chosen the column stops saying ?",
              discColumn(chosen));
    }

    // ── 8. The shared row formatter ────────────────────────────────────────
    // Both candidate lists go through this, so the two lists cannot drift. The
    // disambiguation is load-bearing: without it the two 30th-anniversary rows
    // above are identical on screen.
    std::printf("\n-- shared row formatter --\n");
    {
        CandidateRow r;
        r.artist = "The Smashing Pumpkins";
        r.title  = "Mellon Collie and the Infinite Sadness";
        r.year   = "2025"; r.country = "US"; r.right = "?/4";
        const std::string plain = formatCandidateRow(0, r, 72);
        r.disambig = "30th anniversary edition, remastered";
        const std::string disam = formatCandidateRow(1, r, 72);

        check(plain.size() == 72 && disam.size() == 72, "rows are exactly the width",
              std::to_string(plain.size()) + "/" + std::to_string(disam.size()));
        check(plain.rfind(" 1. ", 0) == 0 && disam.rfind(" 2. ", 0) == 0,
              "1-based index, padded to two columns");
        check(plain != disam, "the disambiguation actually changes the row");
        check(disam.find("30th") != std::string::npos,
              "and it is visible, not the first thing truncated", disam);
        check(plain.find("?/4") != std::string::npos
              && plain.find("2025") != std::string::npos
              && plain.find("US") != std::string::npos,
              "year, country and disc column all present", plain);

        // A Ctrl+F row: a track count instead of a disc, plus the Discogs marker.
        CandidateRow f;
        f.artist = "Bjork"; f.title = "Post"; f.year = "1995";
        f.right  = "11t";   f.from_discogs = true;
        const std::string fr = formatCandidateRow(9, f, 72);
        check(fr.find("11t") != std::string::npos && fr.find("[D]") != std::string::npos,
              "Ctrl+F row carries the track count and the Discogs marker", fr);
        check(fr.rfind("10. ", 0) == 0, "index 10 needs no leading pad", fr.substr(0, 4));

        // A narrow terminal must not overflow the box.
        for (int wdt : {40, 50, 60, 72}) {
            const std::string n = formatCandidateRow(3, r, wdt);
            check((int)n.size() == wdt, "width " + std::to_string(wdt) + " respected",
                  std::to_string(n.size()));
        }
    }

    // ── 9. The note fits the box it is drawn in ────────────────────────────
    // discAmbiguityNote is written for the command line, where the whole
    // terminal is available. Reused verbatim inside the 76-column medium modal
    // it ran off the right edge and ended "titles may be w" - confirmed on
    // hardware. Wrapping keeps ONE string for both surfaces; shortening it for
    // the modal would have meant two to keep in step.
    std::printf("\n-- the ambiguity note fits a 76-column modal --\n");
    {
        const DiscPick p = pickDisc(releaseWithMedia({14, 14}), 14);
        const std::string note = discAmbiguityNote(p, 14);
        const int ROW_W = 76 - 4;                 // drawMBPick's usable width

        check((int)note.size() > ROW_W,
              "the note really is wider than the modal", std::to_string(note.size()));

        const std::vector<std::string> lines = wrapToWidth(note, ROW_W);
        bool fits = !lines.empty();
        for (const auto& l : lines) if ((int)l.size() > ROW_W) fits = false;
        check(fits, "every wrapped line fits", std::to_string(lines.size()) + " lines");
        check(lines.size() == 2, "and it takes two rows, not more",
              std::to_string(lines.size()));

        // Nothing lost and no word split: rejoining reproduces the original.
        std::string joined;
        for (std::size_t i = 0; i < lines.size(); ++i)
            joined += (i ? " " : "") + lines[i];
        check(joined == note, "wrapping loses no text", joined);
        check(lines.back().find("titles may be wrong") != std::string::npos,
              "the actionable half survives", lines.back());

        // A wide terminal keeps it on one line - the cmdline case, unchanged.
        check(wrapToWidth(note, 100).size() == 1, "one line when there is room");
        // Degenerate widths must not hang or throw.
        check(wrapToWidth(note, 0).empty(), "width 0 yields nothing");
        for (int wdt : {1, 5, 12, 40}) {
            const auto ls = wrapToWidth(note, wdt);
            bool ok = !ls.empty();
            for (const auto& l : ls) if ((int)l.size() > wdt) ok = false;
            check(ok, "width " + std::to_string(wdt) + " respected",
                  std::to_string(ls.size()) + " lines");
        }
    }


    std::printf("\n%s (%d failure%s)\n",
                g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
