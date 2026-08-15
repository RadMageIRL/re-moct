#pragma once
#include <string>
#include <vector>

// ArtCandidates — the free-text cover-art candidate list, as ROWS.
//
// CoverArt::bytesByText scores ~10 albums each from iTunes and Deezer, keeps one
// URL and drops every loser inside the loop. This module keeps them, so the art
// picker can show a list on the discs where Cover Art Archive has nothing — which
// is exactly where the automatic pick is least trustworthy.
//
// SPLIT ON PURPOSE: everything here is pure and depends on nothing. The JSON
// field extraction lives in ArtCandidatesJson.h, which is included by CoverArt.cpp
// and by the test and NOT by UIManager — json.hpp is a large header and the
// picker only needs the struct.
//
// The normalisation and artist gate below are the SAME RULES CoverArt's text path
// applies (`norm` / `artist_ok` / `album_overlap` there). They are deliberately a
// second copy rather than a refactor of the automatic path: changing the scoring
// that picks the automatic default was a stated non-goal of the slice that added
// this. `art_candidates_test` pins the behaviour the two copies must share, so a
// drift is a test failure rather than a surprise in the rip.
namespace art {

// One album a free-text search offered. Field availability is NOT uniform across
// the two services and the empty values are load-bearing (see below).
struct Candidate {
    std::string source;      // "iT" or "dz" — shown as a row tag, never inferred
    std::string artist;
    std::string title;
    std::string year;        // 4 chars, or "" — DEEZER ALBUM SEARCH PUBLISHES NONE
    std::string country;     // 2-3 chars, or "" — likewise none from Deezer
    int         tracks = 0;  // 0 = unknown. The discriminating field: see below
    std::string thumb_url;   // 250px, for the preview pane
    std::string image_url;   // full size, fetched only when a row is chosen
    bool        automatic = false;  // what bytesByText would have taken
};

// ─── The shared normalisation (mirrors CoverArt's text path) ─────────────────
// Lowercase alphanumerics, single spaces, leading "the " dropped so
// "The Crystals" == "Crystals".
inline std::string norm(const std::string& s) {
    std::string o; bool sp = false;
    for (char c : s) {
        bool aln = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');
        if (aln) { o += (c>='A'&&c<='Z') ? (char)(c + 32) : c; sp = false; }
        else if (!sp && !o.empty()) { o += ' '; sp = true; }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.rfind("the ", 0) == 0) o.erase(0, 4);
    return o;
}

// A free-text music search will happily return a DIFFERENT ARTIST's album when
// the exact title is not in the catalogue. This gate is why the picker cannot
// list one: no art beats wrong-artist art, and a wrong-artist ROW is worse still
// because it looks like a choice.
inline bool artistOk(const std::string& want, const std::string& got) {
    std::string a = norm(want), b = norm(got);
    if (a.empty() || b.empty()) return false;
    return a == b || a.find(b) != std::string::npos
                  || b.find(a) != std::string::npos;
}

// How many tokens of the REQUESTED album appear in a candidate's title.
inline int albumOverlap(const std::string& want, const std::string& got) {
    auto toks = [](const std::string& n) {
        std::vector<std::string> t; std::string cur;
        for (char c : n) { if (c==' ') { if(!cur.empty()){t.push_back(cur);cur.clear();} }
                           else cur += c; }
        if (!cur.empty()) t.push_back(cur);
        return t;
    };
    auto ta = toks(norm(want)), tb = toks(norm(got));
    int shared = 0;
    for (auto& x : ta) for (auto& y : tb) if (x == y) { ++shared; break; }
    return shared;
}

// Mark the row the automatic path would have taken, so the picker opens on the
// honest question "here is what you were going to get, is it right".
//
// The rule is bytesByText's, replicated exactly: highest album_overlap wins,
// compared with a STRICT `>` so that on a TIE THE FIRST-LISTED CANDIDATE WINS,
// and iTunes is decided on its own — Deezer is consulted only when iTunes offered
// nothing at all.
//
// MEASURED, and it is the reason this feature exists: for album "Relish" both
// "Relish" and "Relish (Expanded Edition)" score 1, because the request has one
// token and both titles contain it. The tie hands the pick to whatever the
// service listed first, and iTunes lists the 20-track Expanded Edition — so a
// 12-track disc gets 20-track art, chosen by result ordering rather than by any
// judgement of ours.
//
// HONEST LIMIT: bytesByText also falls through to Deezer if the iTunes image
// fails to DOWNLOAD. That cannot be known without downloading, so this marks what
// would be chosen absent a download failure.
inline void markAutomatic(std::vector<Candidate>& v, const std::string& album) {
    for (auto& c : v) c.automatic = false;
    for (const char* src : { "iT", "dz" }) {
        int best = -1; std::size_t at = v.size();
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].source != src) continue;
            const int s = albumOverlap(album, v[i].title);
            if (s > best) { best = s; at = i; }     // strict: first wins a tie
        }
        if (at < v.size()) { v[at].automatic = true; return; }
    }
}

// The right-hand track-count cell, in formatCandidateRow's existing "19t" shape.
// "" when the service did not say, which renders as nothing rather than "0t".
inline std::string trackCell(int tracks) {
    return tracks > 0 ? std::to_string(tracks) + "t" : std::string();
}

}  // namespace art
