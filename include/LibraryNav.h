#pragma once

// ─── [Library] navigation: the level state machine, pure ─────────────────────
//
// Three levels - artists, that artist's albums, that album's tracks - and two
// directions. Six transitions, and the off-by-one lives in exactly the kind of
// code that is unreachable from a test once it is written inside a curses
// handler. So it lives here instead: no curses, no filesystem, no UIManager, and
// the test links nothing.
//
// WHAT THIS IS NOT. It is not a tree, and it holds no rows. The pane draws a flat
// list produced by an index query, and the hierarchy lives in the index. This
// type holds only WHERE the user is and WHAT THEY CAME THROUGH.
//
// THE STALENESS RULE, which this type is built to keep: every member is an
// identity STRING and never an index. `libidx::tracksForAlbum` returns indices
// into the index, and those are valid only against the instance that produced
// them; they are converted to paths inside the one populate call that asked for
// them and never stored. Because the path back to any level is a pair of strings,
// a scan completing underneath a live listing needs no invalidation - the query
// simply re-runs from `artist` and `album`. That is why there is no generation
// counter here and no need of one.

#include <string>
#include <vector>

#include "LibraryIndex.h"   // detail::icmp - deliberately the SAME compare that
                            // libidx::restoreCursor uses to re-seat the cursor.
                            // If these two ever disagreed, a row could match for
                            // cursor restore and not for level identity, which is
                            // a bug that would present as a cursor that jumps.

namespace libnav {

// Artists, that artist's albums, that album's tracks - and Results, which is not a
// depth but a whole-collection search landing (slice 7). It is a Level rather than a
// separate mode for a concrete reason: as a level its rows are dir_entries_ and
// dir_display_ in lockstep like every other library level, so every operation slices 5
// and AA wired - play, append, queue, favourite, mark, convert, chapters - works on a
// result row by construction rather than by being wired a second time.
enum class Level { Artists, Albums, Tracks, Results };

// Does a row at this level carry a FILESYSTEM PATH as its identity?
//
// This is the whole of slice 5's safety in one predicate, and it is here rather
// than open-coded in browserEntryPath so the product and the test read the same
// comparison instead of two copies of it. A flipped polarity in that comparison
// is not a cosmetic bug: it would hand artist and album strings - tag text, which
// may be invalid UTF-8 - to code that builds fs::path, which is the crash the six
// slice-3 guards exist to prevent.
// Tracks AND Results: a search result is a track, and its identity is the same
// absolute OS-origin path. This one line is what turns on every level-3 operation for
// result rows, which is why slice 7 needed almost no new wiring.
inline bool rowIsPath(Level l) { return l == Level::Tracks || l == Level::Results; }

struct State {
    Level level = Level::Artists;

    // The path taken to the current level. Each is read only by the level that
    // needs it, and cleared on the way back up so it is never stale while the
    // level that reads it is current.
    std::string artist;      // set at Albums and Tracks
    std::string album;       // set at Tracks

    // The remembered cursor per level, so ascending lands where the user left
    // rather than on row 0. THREE members, not one: a single shared one cannot
    // work, because descending from artist "Muse" into album "Absolution" would
    // overwrite the artist memory and ascending would land on row 0 instead of
    // back on Muse.
    //
    // sel_track is a PATH (a level-3 row's identity); the other two are tag text.
    // Nothing here is ever used to build a filesystem path - see the note on
    // descend() below.
    std::string sel_artist;
    std::string sel_album;
    std::string sel_track;

    // ── Whole-collection search (slice 7) ────────────────────────────────────
    // The live query. A STRING, which is why results need no invalidation: a rescan, or
    // anything else that repopulates, simply re-runs the search from this. Same property
    // the artist and album levels have had since slice 4.
    std::string query;
    // Where ascending out of Results goes. Set at entry, so search from level 2 returns
    // to level 2 rather than dumping the user at the top of the section. Entering from
    // the folder browser enters the section first, so this is Artists and a second Left
    // leaves - predictable, and no state beyond one enum.
    Level       return_to = Level::Artists;
};

// What the caller must do after a transition. Repopulate means re-run the query
// for the now-current level; LeaveSection means the user has ascended out of the
// section entirely.
enum class Action { None, Repopulate, LeaveSection };

// The one compare, so level identity and cursor restore can never disagree.
inline bool sameName(const std::string& a, const std::string& b) {
    return libidx::detail::icmp(a, b) == 0;
}

// Descend on the identity of the row under the cursor.
//
// `entry` is TAG TEXT at Artists and Albums, and may be raw Latin-1 - it is
// stored and compared as bytes and must never reach fs::path. At Tracks it is an
// absolute path, but this function does not care: it only ever stores.
//
// The remembered cursor of a DEEPER level is discarded when the selection at this
// level changes, so descending into a different album cannot restore a track
// cursor from the previous one. The comparison is against the remembered
// selection rather than against `artist`/`album`, because those are cleared on
// ascent - comparing with them would make every re-descent look like a change and
// throw the memory away exactly when it is wanted.
inline Action descend(State& s, const std::string& entry) {
    switch (s.level) {
        case Level::Artists:
            if (!sameName(entry, s.sel_artist)) { s.sel_album.clear(); s.sel_track.clear(); }
            s.sel_artist = entry;
            s.artist     = entry;
            s.album.clear();
            s.level      = Level::Albums;
            return Action::Repopulate;

        case Level::Albums:
            if (!sameName(entry, s.sel_album)) s.sel_track.clear();
            s.sel_album = entry;
            s.album     = entry;
            s.level     = Level::Tracks;
            return Action::Repopulate;

        case Level::Tracks:
        case Level::Results:
            // Both are leaves: Enter PLAYS (slice 5) rather than descending, so there is
            // no deeper level to go to. The selection is still remembered, so the cursor
            // survives a repopulate either way.
            s.sel_track = entry;
            return Action::None;
    }
    return Action::None;
}

// Ascend one level. At Artists there is nothing above, so the caller leaves the
// section. [Back] and Left both route here - that is what makes them identical at
// every level rather than identical by coincidence.
inline Action ascend(State& s) {
    switch (s.level) {
        case Level::Results:
            // Back to wherever the search was opened from, not to a fixed level. The
            // query is dropped: leaving a result list is finishing with that search, and
            // keeping it would make the next repopulate silently re-run it.
            s.query.clear();
            s.level = s.return_to;
            return Action::Repopulate;

        case Level::Tracks:
            s.album.clear();
            s.level = Level::Albums;
            return Action::Repopulate;

        case Level::Albums:
            s.artist.clear();
            s.album.clear();
            s.level = Level::Artists;
            return Action::Repopulate;

        case Level::Artists:
            return Action::LeaveSection;
    }
    return Action::LeaveSection;
}

// The reset-trap contract, called from BOTH refreshDir() and enterDriveList().
// Clears the navigation POSITION and the two remembered cursors whose meaning
// depends on it.
//
// sel_artist deliberately SURVIVES: it is a section-level memory, it is what
// shipped slice-3 behaviour does (refreshDir never cleared lib_selected_), and it
// cannot go stale in a harmful way because it is only ever a restoreCursor hint
// matched by name against a freshly queried list. Leaving the section through
// [Back] or Left clears it separately, which is also slice-3 behaviour.
inline void reset(State& s) {
    s.level = Level::Artists;
    s.artist.clear();
    s.album.clear();
    s.sel_album.clear();
    s.sel_track.clear();
    s.query.clear();               // slice 7: a live search does not survive a reset
    s.return_to = Level::Artists;
}

// Open a search. `from` is the level to come back to, captured at entry so ascending
// returns where the user was rather than to the top of the section.
inline void beginSearch(State& s, const std::string& q) {
    if (s.level != Level::Results) s.return_to = s.level;
    s.query = q;
    s.level = Level::Results;
}

// ── One album's tracks, in the order they are shown ─────────────────────────
//
// THE ONE ORDERING FUNCTION. Both readers call this: showLibraryTracks to draw the
// level-3 list, and the album append to add rows to the playlist. So "it appends in
// the order you see" is true BY CONSTRUCTION rather than because two loops happen to
// agree - which is precisely the defect BrowserPins.h was written to end, caught here
// before it existed rather than after a gate failed.
//
// THE STALENESS RULE IS ENFORCED HERE. libidx::tracksForAlbum returns INDICES into
// the index, valid only against the instance that produced them; they are resolved
// inside this function and the vector holding them dies at the closing brace. What
// leaves is RECORDS BY VALUE - no subscripts, no references into the index, nothing
// that can go stale when a rescan replaces it.
//
// Records rather than bare paths, deliberately: the drawing caller needs the title,
// track number and duration as well, and a paths-only return would force it either to
// look each path back up (quadratic) or to run its own second query, which is the
// duplication this function exists to remove.
//
// Order is libidx::tracksForAlbum's: disc, then track number, then title, then path.
// The last two are what give an untagged rip, where every track_no is 0, a total and
// stable order from the filename.
inline std::vector<libidx::LibraryTrack> albumTracks(const libidx::LibraryIndex& idx,
                                                     const std::string& artist,
                                                     const std::string& album) {
    const std::vector<std::size_t> hits = libidx::tracksForAlbum(idx, artist, album);
    std::vector<libidx::LibraryTrack> out;
    out.reserve(hits.size());
    for (std::size_t i : hits) out.push_back(idx.tracks[i]);
    return out;
}

// ── Level-3 row label ───────────────────────────────────────────────────────
// Here rather than in the draw code so the degenerate cases - which are the
// normal cases for an untagged rip - are provable without a terminal.

// Filename stem, by BYTES, with no fs::path anywhere. A track path is OS-origin
// and would survive fs::path, but there is no reason to pay a throwing
// construction to find a separator, and keeping this header free of <filesystem>
// keeps it free of the whole invalid-UTF-8 hazard by construction.
//
// Delegates to libidx: slice 7's search needs the same stem to match untagged rips on
// their filename, and two copies of a rule this small is how they drift.
inline std::string pathStem(const std::string& p) { return libidx::detail::pathStemOf(p); }

// "NN. Title  (M:SS)", with every part optional because every part is missing on
// something in a real collection:
//   track_no == 0      -> no number and no separator, never "0. "
//   title empty        -> the filename stem, which is the only name left
//   dur empty          -> no parenthetical (caller passes "" for a zero duration)
// `dur` is pre-formatted by the caller so UIManager::formatTime stays the single
// duration formatter in the program and a library row reads exactly like a
// playlist row.
inline std::string trackRowLabel(const libidx::LibraryTrack& t, const std::string& dur) {
    std::string out;
    if (t.track_no > 0) {
        const std::string n = std::to_string(t.track_no);
        out += (n.size() < 2 ? "0" + n : n);
        out += ". ";
    }
    out += t.title.empty() ? pathStem(t.path) : t.title;
    if (!dur.empty()) { out += "  ("; out += dur; out += ")"; }
    return out;
}

// ── Result-row label (slice 7) ──────────────────────────────────────────────
// "Artist - Title  [Album] (ext)".
//
// A result row has to be actionable on its own, because unlike a level-3 row it carries
// no surrounding context: the pane is not "this album", it is "everything that matched".
// So artist and album are on the row.
//
// THE EXTENSION IS NEVER DROPPED, and that is not tidiness. The real collection holds
// seven format copies of some tracks (84 records for a 12-track album, measured in
// LIB-AA), and with artist, title and album all identical the extension is the ONLY
// thing distinguishing those rows from each other. A list of seven identical lines is
// not a list.
//
// Elision priority when the row will not fit: album first (it is context), then artist,
// and the title last, because the title is what was searched for.
inline std::string searchRowLabel(const libidx::LibraryTrack& t, int cols) {
    const std::string ext = [&] {
        const std::string base = libidx::detail::pathStemOf(t.path);
        // Extension = what pathStemOf removed; recover it rather than re-parsing.
        const std::size_t slash = t.path.find_last_of("/\\");
        const std::string file = (slash == std::string::npos) ? t.path : t.path.substr(slash + 1);
        return (file.size() > base.size() + 1) ? file.substr(base.size() + 1) : std::string();
    }();

    const std::string title  = t.title.empty() ? libidx::detail::pathStemOf(t.path) : t.title;
    const std::string artist = t.artist.empty() ? groupingArtist(t) : t.artist;
    const std::string tail   = ext.empty() ? std::string() : ("  (" + ext + ")");

    // Budget in BYTES here deliberately: the caller sanitises and column-clips for the
    // pane. This only decides what to include, and cols is a hint for that choice.
    const int budget = (cols > 0) ? cols : 200;
    std::string row = artist.empty() ? title : (artist + " - " + title);
    if (!t.album.empty() &&
        (int)(row.size() + t.album.size() + 4 + tail.size()) <= budget)
        row += "  [" + t.album + "]";
    else if (!t.album.empty() && (int)(title.size() + t.album.size() + 4 + tail.size()) <= budget)
        row = title + "  [" + t.album + "]";      // dropped the artist to keep the album
    return row + tail;
}

} // namespace libnav
