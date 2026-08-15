// art_candidates_test - the free-text cover-art candidate rows.
//
// WHY THIS EXISTS. CoverArt::bytesByText scores ~10 albums each from iTunes and
// Deezer, keeps one URL and drops every loser inside the loop, so on the discs
// Cover Art Archive cannot cover there was one image and no way to change it.
// This module keeps the candidates so the picker can list them.
//
// THE FIXTURES ARE REAL RESPONSES, captured from the live APIs on 2026-08-12 for
// "Joan Osborne Relish" - the project's own CD gate album - and reduced to the
// fields the parsers read, values verbatim. That album is the case worth pinning
// because it is where the automatic pick is measurably WRONG:
//
//   * Both services return exactly two albums: "Relish" (12 tracks) and
//     "Relish (Expanded Edition)" (20 tracks). Same artist, same year, same
//     country - TRACK COUNT IS THE ONLY FIELD THAT SEPARATES THEM.
//   * album_overlap ties them at 1: the request "Relish" has one token and both
//     titles contain it. bytesByText compares with a strict `>`, so the tie is
//     broken by RESULT ORDER, and iTunes lists the Expanded Edition first.
//   * So a 12-track disc gets 20-track art, decided by iTunes' ordering.
//
// That tie is asserted below. It is the reason the picker exists, the reason the
// list keeps service order rather than sorting by a score that does not
// discriminate, and the behaviour art::markAutomatic must keep sharing with
// CoverArt's automatic path - the two are separate copies (changing the
// automatic pick was a non-goal), and this test is what makes a drift visible.
//
// Header-only: ArtCandidates.h is pure, ArtCandidatesJson.h adds only the
// vendored json.hpp, so this links nothing.

#include "ArtCandidatesJson.h"
#include "MBLookup.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string& what, const std::string& got = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                got.empty() ? "" : "  -> ", got.c_str());
    if (!ok) ++failures;
}

// ── Captured, reduced, values verbatim ──────────────────────────────────────
// NOTE THE `json(` DELIMITER, and do not simplify it back to a bare R"(...)":
// the titles contain `Edition)"`, which is exactly the sequence that closes a
// default-delimited raw string. It fails as a wall of unrelated syntax errors.
static const char* kItunes = R"json({"results":[
{"artistName":"Joan Osborne","collectionName":"Relish (Expanded Edition)","artworkUrl100":"https://is1-ssl.mzstatic.com/image/thumb/Music125/v4/b1/b8/8d/b1b88db0-d732-34f1-7ee0-e54581612a84/00602547532619.rgb.jpg/100x100bb.jpg","releaseDate":"1995-03-21T08:00:00Z","country":"USA","trackCount":20},
{"artistName":"Joan Osborne","collectionName":"Relish","artworkUrl100":"https://is1-ssl.mzstatic.com/image/thumb/Music125/v4/73/7b/e8/737be8fa-811b-a143-992a-948ba265f0a0/00731452669926.rgb.jpg/100x100bb.jpg","releaseDate":"1995-01-01T08:00:00Z","country":"USA","trackCount":12}]})json";

static const char* kDeezer = R"json({"data":[
{"title":"Relish","nb_tracks":12,"artist":{"name":"Joan Osborne"},"cover_xl":"https://cdn-images.dzcdn.net/images/cover/6fcf1bc7b0971b8718c51a73b33ca5e1/1000x1000-000000-80-0-0.jpg","cover_big":"https://cdn-images.dzcdn.net/images/cover/6fcf1bc7b0971b8718c51a73b33ca5e1/500x500-000000-80-0-0.jpg","cover_medium":"https://cdn-images.dzcdn.net/images/cover/6fcf1bc7b0971b8718c51a73b33ca5e1/250x250-000000-80-0-0.jpg"},
{"title":"Relish (Expanded Edition)","nb_tracks":20,"artist":{"name":"Joan Osborne"},"cover_xl":"https://cdn-images.dzcdn.net/images/cover/e9965f58a7e02748daf1a3aeae3f3468/1000x1000-000000-80-0-0.jpg","cover_big":"https://cdn-images.dzcdn.net/images/cover/e9965f58a7e02748daf1a3aeae3f3468/500x500-000000-80-0-0.jpg","cover_medium":"https://cdn-images.dzcdn.net/images/cover/e9965f58a7e02748daf1a3aeae3f3468/250x250-000000-80-0-0.jpg"}]})json";

static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    std::printf("art_candidates_test\n");

    // ── 1. iTunes: every field the row shows ────────────────────────────────
    std::printf("\n-- iTunes parse --\n");
    std::vector<art::Candidate> it = art::parseItunes(kItunes, "Joan Osborne");
    check(it.size() == 2, "two candidates survive the artist gate",
          std::to_string(it.size()));
    if (it.size() == 2) {
        check(it[0].title == "Relish (Expanded Edition)", "service order preserved", it[0].title);
        check(it[0].tracks == 20 && it[1].tracks == 12, "track counts read",
              std::to_string(it[0].tracks) + "/" + std::to_string(it[1].tracks));
        check(it[0].year == "1995", "year sliced from the ISO date", it[0].year);
        check(it[0].country == "USA", "country read", it[0].country);
        check(it[0].source == "iT", "source tag", it[0].source);
        // The one artwork URL is resized by path segment - the same trick the
        // automatic path already uses for 600x600. 250 is what CAA publishes,
        // so all three sources feed the preview pane the same size.
        check(has(it[0].thumb_url, "250x250") && !has(it[0].thumb_url, "100x100"),
              "thumb resized to 250x250", it[0].thumb_url.substr(it[0].thumb_url.size() - 18));
        check(has(it[0].image_url, "600x600"), "full image at 600x600",
              it[0].image_url.substr(it[0].image_url.size() - 18));
    }

    // ── 2. Deezer: the asymmetry is REAL and must not be papered over ───────
    std::printf("\n-- Deezer parse --\n");
    std::vector<art::Candidate> dz = art::parseDeezer(kDeezer, "Joan Osborne");
    check(dz.size() == 2, "two candidates", std::to_string(dz.size()));
    if (dz.size() == 2) {
        check(dz[0].title == "Relish" && dz[0].tracks == 12,
              "service order preserved - Deezer lists the 12-track first", dz[0].title);
        // Deezer's album search publishes NEITHER. Left empty on purpose: the
        // row formatter omits empty fields rather than padding them, so a Deezer
        // row comes out shorter instead of hole-punched.
        check(dz[0].year.empty(), "Deezer publishes no year - left empty, not invented");
        check(dz[0].country.empty(), "Deezer publishes no country - likewise");
        check(dz[0].source == "dz", "source tag", dz[0].source);
        // cover_medium arrives in the very response the automatic path already
        // parses, and it reads only cover_xl/cover_big - this is free.
        check(has(dz[0].thumb_url, "250x250"), "thumb is cover_medium (250px)",
              dz[0].thumb_url.substr(dz[0].thumb_url.size() - 26));
        check(has(dz[0].image_url, "1000x1000"), "full image is cover_xl",
              dz[0].image_url.substr(dz[0].image_url.size() - 26));
    }

    // ── 3. The artist gate - no art beats wrong-artist art ──────────────────
    std::printf("\n-- the artist gate --\n");
    check(art::parseItunes(kItunes, "The Doors").empty(),
          "a different artist's request keeps nothing");
    check(art::parseItunes(kItunes, "joan   osborne").size() == 2,
          "normalisation: case and spacing do not matter");
    check(art::artistOk("The Crystals", "Crystals"),
          "leading 'the' dropped, per CoverArt's own rule");

    // ── 4. THE TIE - the measured reason this feature exists ────────────────
    std::printf("\n-- the Relish tie, and who wins it --\n");
    check(art::albumOverlap("Relish", "Relish") == 1, "exact title scores 1");
    check(art::albumOverlap("Relish", "Relish (Expanded Edition)") == 1,
          "THE EXPANDED EDITION SCORES THE SAME - the score does not discriminate");
    {
        std::vector<art::Candidate> all = it;
        all.insert(all.end(), dz.begin(), dz.end());
        art::markAutomatic(all, "Relish");
        int marked = 0; std::string which;
        for (const auto& c : all) if (c.automatic) { ++marked; which = c.source + " " + c.title; }
        check(marked == 1, "exactly one row is marked automatic", std::to_string(marked));
        // This is the defect, pinned: iTunes wins because it is tried first, and
        // WITHIN iTunes the tie goes to whatever the service listed first.
        check(which == "iT Relish (Expanded Edition)",
              "the automatic pick is the 20-TRACK edition, on a 12-track disc", which);
    }
    {
        // iTunes offering nothing is the only way Deezer is consulted - the
        // precedence bytesByText uses.
        std::vector<art::Candidate> only_dz = dz;
        art::markAutomatic(only_dz, "Relish");
        check(only_dz[0].automatic && !only_dz[1].automatic,
              "with no iTunes rows, Deezer's first-listed wins");
    }
    {
        // A request whose tokens genuinely discriminate must still beat order.
        std::vector<art::Candidate> v = it;
        art::markAutomatic(v, "Relish Expanded Edition");
        check(v[0].automatic, "when the score DOES discriminate, it decides");
    }

    // ── 5. The row: the shared formatter, with a source tag ────────────────
    std::printf("\n-- the candidate row --\n");
    {
        CandidateRow r;
        r.artist = "Joan Osborne"; r.title = "Relish (Expanded Edition)";
        r.year = "1995"; r.country = "USA"; r.right = "20t"; r.source_tag = "iT";
        const std::string s = formatCandidateRow(0, r, 72);
        check(has(s, "20t") && has(s, "[iT]") && has(s, "1995"),
              "track count, source tag and year all present", s);
        check(!has(s, "[D]"), "the Discogs marker is NOT reused for a service tag");
        check((int)s.size() == 72, "width still exact", std::to_string(s.size()));

        // THE AUTOMATIC MARKER IS A STAR IN THE TAG, not an appended word, and
        // the reason is width. formatCandidateRow pads to exactly `width` and
        // right-aligns the tail, so anything appended is cut off by the draw;
        // reserving room for "(automatic)" instead costs 12 of the 50 columns
        // the picker actually has once the preview pane takes its 22, and that
        // reserve eats the TITLE - the one field distinguishing these rows.
        // At the picker's real width the two forms compare like this:
        //   reserve:  1. Joa>  Relish>          1995  USA  20t [iT]
        //   star:     1. Joan Os>  Relish (Expan>  1995  USA  20t [iT*]
        CandidateRow a = r;
        a.source_tag = "iT*";
        const std::string s50 = formatCandidateRow(0, a, 50);
        check(has(s50, "[iT*]"), "the automatic row is starred in the tag", s50);
        check(has(s50, "Relish (Expan"),
              "and the title survives at the picker's real width", s50);
        check((int)s50.size() == 50, "width exact at 50", std::to_string(s50.size()));
    }
    {
        // A Deezer row: two empty fields, and they must shorten the row rather
        // than punch holes in it.
        CandidateRow r;
        r.artist = "Joan Osborne"; r.title = "Relish";
        r.right = "12t"; r.source_tag = "dz";
        const std::string s = formatCandidateRow(1, r, 72);
        check(has(s, "12t") && has(s, "[dz]"), "Deezer row carries count and tag", s);
        check(!has(s, "  1995"), "no year column invented for Deezer");
        check((int)s.size() == 72, "width still exact", std::to_string(s.size()));
    }
    {
        // The existing release lists must be untouched by the new field.
        CandidateRow f;
        f.artist = "Bjork"; f.title = "Post"; f.year = "1995";
        f.right = "11t";    f.from_discogs = true;
        const std::string s = formatCandidateRow(9, f, 72);
        check(has(s, "[D]") && !has(s, "[]"),
              "an empty source_tag adds nothing to the old rows", s);
    }
    check(art::trackCell(0).empty(), "unknown track count renders as nothing, not '0t'");
    check(art::trackCell(12) == "12t", "known count in the existing shape", art::trackCell(12));

    // ── 6. Junk in, nothing out ────────────────────────────────────────────
    std::printf("\n-- malformed input --\n");
    check(art::parseItunes("", "Joan Osborne").empty(), "empty body");
    check(art::parseItunes("<html>404</html>", "Joan Osborne").empty(), "an HTML error page");
    check(art::parseItunes(R"({"results":[{"artistName":"Joan Osborne"}]})", "Joan Osborne").empty(),
          "a result with no artwork URL is dropped, as the automatic path drops it");
    check(art::parseDeezer(R"({"data":[{"title":"x","artist":{"name":"Joan Osborne"}}]})",
                           "Joan Osborne").empty(),
          "a Deezer entry with no cover is dropped");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
