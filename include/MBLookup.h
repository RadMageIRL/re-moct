#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>

// ─── MusicBrainz DiscID + metadata lookup ────────────────────────────────────
// Non-blocking CD metadata retrieval via WinINet.
// Spawns a single worker thread per lookup. Rate-limited to 1 req/sec.
// Uses the MusicBrainz DiscID algorithm (SHA-1 + custom Base64).

struct MBTrack {
    int         number;        // 1-based position WITHIN its disc
    std::string title;
    std::string artist;
    int         disc = 1;      // 1-based medium/disc number (for multi-disc sets)
};

struct MBRelease {
    std::string mb_id;   // MusicBrainz release ID (for Cover Art Archive)
    std::string title;
    std::string artist;
    std::string date;
    // MusicBrainz's own "these two releases are not the same thing" note, e.g.
    // "30th anniversary edition, remastered". MEASURED NECESSITY, not a nicety:
    // Mellon Collie's disc 2 resolves to four releases, all titled "Mellon
    // Collie and the Infinite Sadness", two of which share a date, a country and
    // a media shape and differ only in a track-title spelling. Without this the
    // picker cannot tell those two rows apart at all.
    std::string disambiguation;
    std::vector<MBTrack> tracks;

    // The hidden track ahead of track 1, when the release names one. Kept OUT
    // of `tracks` deliberately and on pain of breaking disc identification:
    // MBTrack::number is a 1-based position within its disc, and
    // pickDiscForTrackCount matches a medium BY COUNTING those entries, so an
    // extra element would make a 13-track disc look like a 14-track one and
    // pick the wrong medium on a multi-disc release. It is not a track; the TOC
    // does not describe it; it does not belong in a list of tracks.
    //
    // Both sources carry it, in different shapes, and both are read:
    //   MusicBrainz - `media[].pregap`, a key SEPARATE from `tracks`, with
    //                 position 0. Factory Showroom gives "Token Back to
    //                 Brooklyn", 61000 ms, matching the 4575-sector pregap.
    //   Discogs     - an ordinary tracklist row whose `position` is "0".
    //
    // Empty means the release named no title, which is not the same as the disc
    // having no hidden track - the TOC decides that, and only the TOC.
    std::string pregap_title;
};

// ─── Which medium is in the drive, and did we actually determine it ──────────
//
// The fallback to disc 1 was always documented and always SILENT, and that cost
// a real rip: Mellon Collie's two media have the same track count, so the tie
// fires, disc 1 is assumed, and every title on disc 2 is written from disc 1's
// tracklist with nothing said anywhere. `matches` is the number this type exists
// to carry - pickDiscForTrackCount computed it and threw it away at the return.
//
// `disc` is bit-for-bit what pickDiscForTrackCount has always returned. Nothing
// about the CHOICE changes in this slice; only whether the program admits how it
// was made.
struct DiscPick {
    int  disc    = 1;   // 1-based medium chosen (1 when undetermined)
    int  total   = 1;   // media on the release (1 when the release names none)
    int  matches = 0;   // media whose track count equals the physical count
    // A PERSON chose this disc, in the medium stage of the picker. Track counts
    // did not decide it and cannot contradict it, which is why it clears
    // `ambiguous()` below rather than sitting beside it: an answer given by the
    // user is determined, whatever the counts say.
    bool user    = false;

    // AMBIGUOUS = the number was not determined and disc 1 was assumed. Both
    // failing shapes land here: no medium fits (`matches == 0`, usually the
    // wrong release) and several fit (`matches >= 2`, the Mellon Collie tie).
    // Gated on total > 1 because a release with one medium has nothing to be
    // ambiguous about - its disc is 1 by construction, not by assumption.
    bool ambiguous() const { return !user && total > 1 && matches != 1; }
};

// Apply a user's medium choice to a pick. Kept as a function rather than left to
// each caller so "chosen" always means the same two field writes.
inline DiscPick withUserDisc(DiscPick p, int disc) {
    if (disc >= 1 && disc <= p.total) { p.disc = disc; p.user = true; }
    return p;
}

// For multi-disc releases, rel.tracks holds every disc's tracks tagged with
// MBTrack::disc. Given the physical disc's audio track count, identify the
// medium by matching that count. Unambiguous single match wins; otherwise (no
// match, or two discs sharing a count) fall back to disc 1. Single-disc releases
// always give 1. Lookups should scope on (number == tnum && disc == pick.disc).
inline DiscPick pickDisc(const MBRelease& rel, int n_physical) {
    DiscPick p;
    for (const auto& t : rel.tracks) if (t.disc > p.total) p.total = t.disc;
    int found = 1;
    for (int d = 1; d <= p.total; ++d) {
        int c = 0;
        for (const auto& t : rel.tracks) if (t.disc == d) ++c;
        if (c == n_physical) { found = d; ++p.matches; }
    }
    // Single-medium releases short-circuit to 1 exactly as before, WITHOUT
    // consulting the count - a release whose one medium disagrees with the disc
    // is a wrong-release problem, not a which-medium problem, and this function
    // has never claimed to detect it.
    p.disc = (p.total == 1) ? 1 : (p.matches == 1 ? found : 1);
    return p;
}

// The original entry point, unchanged in contract and in every value it returns.
// Kept so no existing caller has to care that the richer form now exists.
inline int pickDiscForTrackCount(const MBRelease& rel, int n_physical) {
    return pickDisc(rel, n_physical).disc;
}

// ─── Saying it out loud: the three reporting surfaces ────────────────────────
// Pure functions over DiscPick so the log line, the sidecar value and the
// on-screen note cannot drift from the predicate or from each other, and so
// disc_pick_test can assert the exact text a user will see.

// `disc_source` for disc.json, schema 4. A DISCRIMINATED ENCODING, not a
// convenience label: the number alone cannot say how it was arrived at, and
// "1" from a confident match and "1" from a coin-toss are different facts.
//
// "user" now HAS a producer: the picker's medium stage sets DiscPick::user, and
// that is the single source - there is no second way to say "a person chose it".
inline const char* discSourceLabel(const DiscPick& p) {
    if (p.user)         return "user";
    return p.ambiguous() ? "ambiguous_fallback" : "unique_track_count";
}

// The rip log's `Disc :` line. Written on EVERY rip, single-disc included -
// absence would otherwise mean both "one disc" and "older version".
inline std::string discLogLine(const DiscPick& p, int n_physical) {
    std::string s = std::to_string(p.disc) + " of " + std::to_string(p.total);
    if (p.user) return s + " (chosen)";
    if (!p.ambiguous())
        return p.total > 1 ? s + " (matched by track count)" : s;
    s += "  ** AMBIGUOUS - ";
    s += (p.matches == 0)
        ? "no medium has " + std::to_string(n_physical) + " tracks"
        : std::to_string(p.matches) + " media have " + std::to_string(n_physical) + " tracks";
    s += "; disc 1 assumed, titles may be wrong **";
    return s;
}

// The cmdline note, shown when titles are applied under an assumed disc. Empty
// when the disc was determined - there is nothing to say then, and a message on
// every lookup would train the user to ignore this one. Deliberately shorter
// than the log line: this competes for one terminal row.
//
// Contains the word "ambiguous", which drawStatus() matches to colour it as a
// warning rather than as an OK status.
inline std::string discAmbiguityNote(const DiscPick& p, int n_physical) {
    if (!p.ambiguous()) return {};
    std::string s = "Disc ambiguous - ";
    s += (p.matches == 0)
        ? "no disc has " + std::to_string(n_physical) + " tracks"
        : std::to_string(p.matches) + " discs have " + std::to_string(n_physical) + " tracks";
    s += "; assumed disc 1, titles may be wrong";
    return s;
}

// ─── The candidate row, shared by both lists ─────────────────────────────────
// ^F (text search) and ^R (disc-ID picker) draw candidate lists that MUST look
// identical - Dos already reads the ^F rows fluently, and two formatters would
// drift the first time one of them was adjusted. They carry different types
// (MBSearchResult vs MBRelease), so what is shared is the LAYOUT, below, and
// each caller fills the fields.
//
// Strings arrive ALREADY FOLDED. The fold belongs at the draw site (1.6.1), and
// keeping it out of here means this header needs nothing from StringUtils and
// the layout can be asserted with plain ASCII.

// The right-hand column. On a single-medium release it is the track count,
// which is what ^F has always effectively shown; on a set it is the disc this
// row would resolve to, and "?" when the row cannot resolve one. Being able to
// see "?/6" BEFORE choosing is the point - it says this row will still need the
// medium stage.
inline std::string discColumn(const DiscPick& p) {
    if (p.total <= 1) return {};
    if (p.ambiguous()) return "?/" + std::to_string(p.total);
    return std::to_string(p.disc) + "/" + std::to_string(p.total);
}

struct CandidateRow {
    std::string artist;
    std::string title;
    std::string disambig;    // "" when the release names none
    std::string year;        // 4 chars or ""
    std::string country;     // 2 chars or ""
    std::string right;       // discColumn(), or "19t" for a ^F track count
    bool        from_discogs = false;
};

// One row, truncated to `width` columns. ASCII by contract (see above), so byte
// length is column count here and substr cannot split a character.
inline std::string formatCandidateRow(int idx, const CandidateRow& r, int width) {
    auto cut = [](const std::string& s, std::size_t n) {
        if (n == 0) return std::string();
        return s.size() > n ? s.substr(0, n - 1) + ">" : s;
    };

    std::string s = (idx < 9 ? " " : "") + std::to_string(idx + 1) + ". ";
    std::string tail;
    if (!r.year.empty())    tail += "  " + r.year;
    if (!r.country.empty()) tail += "  " + r.country;
    if (!r.right.empty())   tail += "  " + r.right;
    if (r.from_discogs)     tail += " [D]";

    const int fixed = (int)s.size() + (int)tail.size();
    int room = width - fixed;
    if (room < 8) room = 8;
    // Artist gets a third, title the rest - a title plus its disambiguation is
    // what distinguishes rows; an artist repeated down every row is not.
    const std::size_t a_w = (std::size_t)(room / 3);
    const std::size_t t_w = (std::size_t)room - a_w - 2;

    // THE DISAMBIGUATION GETS ITS SPACE FIRST, and the title is truncated into
    // what is left - not the other way round. Appending it and letting the field
    // truncate normally put it at the END of the string, so it was the first
    // thing to disappear: exactly the failure it exists to prevent. Two releases
    // that need it agree on their titles by definition, so the title is the
    // redundant half of the pair and the right one to cut.
    std::string ttl;
    if (r.disambig.empty()) {
        ttl = cut(r.title, t_w);
    } else {
        std::size_t d_w = r.disambig.size() < t_w / 2 ? r.disambig.size() : t_w / 2;
        std::size_t base = t_w > d_w + 3 ? t_w - d_w - 3 : 0;
        ttl = cut(r.title, base) + " (" + cut(r.disambig, d_w) + ")";
        if (ttl.size() > t_w) ttl.resize(t_w);
    }

    std::string art = cut(r.artist, a_w);
    art.resize(a_w, ' ');
    s += art + "  " + ttl;
    if ((int)s.size() < width - (int)tail.size())
        s.resize((std::size_t)(width - (int)tail.size()), ' ');
    s += tail;
    if ((int)s.size() > width) s.resize((std::size_t)width);
    return s;
}

// Callback fired on completion (success or failure), always on worker thread.
// Caller must sync before touching UI state.
using MBCallback = std::function<void(bool ok, const MBRelease& result, const std::string& err)>;

// The DISC-ID lookup's callback, and the only one that returns more than one
// release. A disc ID identifies a piece of plastic, and the same plastic is
// routinely sold as several releases - Mellon Collie's disc 2 resolves to four,
// FFXI's disc 3 to four. The parser used to keep releases[0] and drop the rest
// before anything could see them, which is not a matching failure but a choice
// made silently on the user's behalf.
//
// `note` carries a truncation notice on success ("" when nothing was dropped)
// and the error text on failure - the same double duty MBSearchCallback's third
// parameter already does.
using MBDiscIdCallback =
    std::function<void(bool ok, std::vector<MBRelease> results, const std::string& note)>;

// How many candidates the disc-ID lookup will keep. Matches parseSearchJson's
// cap so both lists behave alike. Truncation is REPORTED, never silent.
inline constexpr std::size_t kMaxDiscIdCandidates = 12;

// ─── Text search result (lightweight — just enough for the picker UI) ─────────
struct MBSearchResult {
    std::string mbid;
    std::string title;
    std::string artist;
    std::string date;
    std::string country;
    std::string label;
    std::string disambiguation;        // see MBRelease::disambiguation
    int         track_count  = 0;
    bool        from_discogs = false;  // true if result came from Discogs fallback
};

using MBSearchCallback = std::function<void(bool ok,
                                            std::vector<MBSearchResult> results,
                                            const std::string& err)>;

class MBLookup {
public:
    MBLookup() = default;
    ~MBLookup() { cancel(); }

    // Start async DiscID lookup. Returns false if already in progress.
    // toc_offsets: sector offsets for each track (1-based), plus lead-out as last entry.
    // Returns EVERY release the disc ID resolves to (capped, see
    // kMaxDiscIdCandidates), not just the first. The caller decides between
    // them; this seam no longer decides on its behalf.
    bool lookup(int first_track, int last_track,
                const std::vector<uint32_t>& toc_offsets,
                MBDiscIdCallback cb);

    // Search MusicBrainz by artist + album text.
    // Falls back to Discogs if MB returns zero results.
    // Returns false if a lookup/search is already in progress.
    bool search(const std::string& artist, const std::string& album,
                MBSearchCallback cb);

    // Fetch full release metadata by MBID (feeds the existing MBCallback pipeline).
    // Returns false if already in progress.
    bool lookupByMbid(const std::string& mbid, MBCallback cb);

    // Fetch full release metadata from Discogs by numeric release ID.
    // The Discogs /database/search endpoint returns NO tracklist; this hits the
    // release-detail endpoint (/releases/{id}) and feeds the same MBCallback
    // pipeline so a Discogs pick populates tracks like an MB pick.
    // Returns false if already in progress.
    bool lookupDiscogsRelease(const std::string& discogs_id, MBCallback cb);

    bool isActive() const { return active_.load(); }
    void cancel();

    // Compute DiscID from TOC data (public for testing)
    static std::string computeDiscId(int first_track, int last_track,
                                     const std::vector<uint32_t>& offsets);

    // URL-encode a UTF-8 string for use in query parameters
    static std::string urlEncode(const std::string& s);

    // SHA-1 over a byte buffer -> 20-byte digest. Pure/stateless (no member or static
    // state); public so other request-signing paths (e.g. Podcast Index auth) reuse it
    // instead of vendoring a second hash. Used internally by the DiscID computation.
    static void sha1(const uint8_t* data, size_t len, uint8_t out[20]);

private:
    std::atomic<bool>   active_  { false };
    std::atomic<bool>   cancel_  { false };
    std::thread         thread_;

    static std::chrono::steady_clock::time_point last_request_;
    static std::mutex                            rate_mutex_;

    // DiscID worker (existing)
    void worker(int first_track, int last_track,
                std::vector<uint32_t> offsets, MBDiscIdCallback cb);

    // Text search worker
    void searchWorker(std::string artist, std::string album, MBSearchCallback cb);

    // MBID direct fetch worker
    void mbidWorker(std::string mbid, MBCallback cb);

    // Discogs release-detail fetch worker (parses tracklist)
    void discogsReleaseWorker(std::string discogs_id, MBCallback cb);

    static std::string httpGet(const std::string& url);
    // Every release in the disc-ID response, in the order the server returned
    // them. `truncated_out` receives the number dropped by the cap, so the
    // caller can say so rather than showing a silently shortened list.
    static std::vector<MBRelease> parseDiscIdJson(const std::string& json,
                                                  std::size_t* truncated_out = nullptr);

    // Parse the /ws/2/release?query=... response into a list of lightweight results
    static std::vector<MBSearchResult> parseSearchJson(const std::string& json);

    // Parse Discogs /database/search response as fallback
    static std::vector<MBSearchResult> parseDiscogsJson(const std::string& json);

    // DiscID internals (sha1 is declared public above for cross-path reuse)
    static std::string mb_base64(const uint8_t* data, size_t len);
};

