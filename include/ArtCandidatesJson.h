#pragma once
#include "ArtCandidates.h"
#include "json.hpp"
#include <string>
#include <vector>

// The JSON half of ArtCandidates: field extraction for the two free-text
// services. Separated from ArtCandidates.h so json.hpp reaches CoverArt.cpp and
// the test but NOT UIManager, which only needs the struct.
//
// Both parsers take a RESPONSE BODY and never touch the network, so the test
// drives them with captured real responses.
//
// FIELD AVAILABILITY IS NOT SYMMETRICAL, and pretending otherwise is what would
// make one source's rows look broken:
//
//   |            | iTunes                   | Deezer            |
//   | artist     | artistName               | artist.name       |
//   | title      | collectionName           | title             |
//   | year       | releaseDate (ISO)        | NONE              |
//   | country    | country                  | NONE              |
//   | tracks     | trackCount               | nb_tracks         |
//   | 250px      | artworkUrl100, resized   | cover_medium      |
//
// Measured against the live APIs, 2026-08-12.
namespace art {

// iTunes publishes ONE artwork URL (100x100) whose size is a path segment:
// ".../<hash>.rgb.jpg/100x100bb.jpg". Substituting the dimensions is how the
// existing automatic path already reaches 600x600, and 250x250 answers 200 with
// a ~26 KB image - the same size Cover Art Archive publishes as its thumbnail,
// so the preview pane gets identical treatment from all three sources.
inline std::string itunesResize(const std::string& url_100, const char* dims) {
    std::string u = url_100;
    const auto pos = u.rfind("100x100");
    if (pos != std::string::npos) u.replace(pos, 7, dims);
    return u;
}

// iTunes: `results` array. Candidates whose artist fails the gate, or which
// publish no artwork, are dropped - the same two filters bytesByText applies, so
// this list is exactly the set the automatic pick chose from.
inline std::vector<Candidate> parseItunes(const std::string& body,
                                          const std::string& want_artist) {
    std::vector<Candidate> out;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("results")) return out;
        for (auto& r : j["results"]) {
            const std::string a = r.value("artistName", "");
            if (!artistOk(want_artist, a)) continue;
            const std::string art100 = r.value("artworkUrl100", "");
            if (art100.empty()) continue;
            Candidate c;
            c.source    = "iT";
            c.artist    = a;
            c.title     = r.value("collectionName", "");
            c.country   = r.value("country", "");
            c.tracks    = r.value("trackCount", 0);
            // "1995-03-21T08:00:00Z" -> "1995". Anything shorter yields "",
            // which the row formatter omits rather than padding.
            const std::string rd = r.value("releaseDate", "");
            if (rd.size() >= 4) c.year = rd.substr(0, 4);
            c.thumb_url = itunesResize(art100, "250x250");
            c.image_url = itunesResize(art100, "600x600");
            out.push_back(std::move(c));
        }
    } catch (...) {}
    return out;
}

// Deezer: `data` array. Same two filters. `cover_medium` is the 250px variant and
// arrives in this very response - the existing automatic path reads only
// cover_xl/cover_big and leaves it on the floor.
inline std::vector<Candidate> parseDeezer(const std::string& body,
                                          const std::string& want_artist) {
    std::vector<Candidate> out;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("data")) return out;
        for (auto& r : j["data"]) {
            std::string a;
            if (r.contains("artist") && r["artist"].contains("name"))
                a = r["artist"]["name"].get<std::string>();
            if (!artistOk(want_artist, a)) continue;
            std::string full = r.value("cover_xl", "");
            if (full.empty()) full = r.value("cover_big", "");
            if (full.empty()) continue;
            Candidate c;
            c.source    = "dz";
            c.artist    = a;
            c.title     = r.value("title", "");
            c.tracks    = r.value("nb_tracks", 0);
            // No year and no country: Deezer's album search publishes neither.
            // Left empty ON PURPOSE - inventing them from the tracklist would be
            // another request, and guessing them would be a guess in a record.
            c.image_url = full;
            c.thumb_url = r.value("cover_medium", "");
            if (c.thumb_url.empty()) c.thumb_url = full;   // preview from full size
            out.push_back(std::move(c));
        }
    } catch (...) {}
    return out;
}

}  // namespace art
