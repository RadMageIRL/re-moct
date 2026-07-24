#pragma once

// ─── Podcasting 2.0 chapters JSON reader ─────────────────────────────────────
//
// A standalone, PURE parser: the raw text of a feed-referenced chapters document
// in, a chapter list out. No I/O, no globals, no throw. Feeds carry these as
//   <podcast:chapters url="…" type="application/json"/>
// per <item>, and the document is a v1.1.0 object with a "chapters" array of
// {startTime, title, …}. That maps almost 1:1 onto the chapter model the player
// already has, so this returns Mp4Chapter (start_sec + title) and the Chapters
// pane, the seek, and the cursor logic never learn a second type.
//
// TRUST BOUNDARY. These documents come off the open internet and are cached to
// disk, so this parser is the single place either source is trusted — the fetch
// path and the sidecar-read path both land here, and a cached document is
// re-validated on every read rather than once when it was written. Everything is
// bounded: nlohmann runs in no-exceptions mode (malformed input, including
// invalid UTF-8, becomes a discarded value and then an empty list), the shape is
// checked before it is walked, per-entry values are range-checked, titles are
// sanitized for the terminal, and the result is capped. One pass and one sort,
// nothing quadratic — the same "linear or reject" rule the MP4 box reader lives
// under.
//
// DEFENSIVE CONTRACT: never crash, never throw, never hang. Anything unexpected
// degrades to fewer chapters — ultimately to none, which the caller reports as an
// honest "no chapters" rather than an error.

#include "Mp4Chapters.h"   // Mp4Chapter — the one chapter model
#include "StringUtils.h"   // sanitizeForDisplay

#include "json.hpp"        // nlohmann single-header (vendored; already in product TUs)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace pc_detail {

// A UI list is a UI list: a document with a hundred thousand entries is not a
// chapter list, and building one would be the only expensive thing here. Applied
// AFTER the sort so a capped document keeps its earliest chapters, not whichever
// ones happened to be written first.
inline constexpr std::size_t kMaxChapters = 512;

// Titles are one row of a pane, not prose. Bytes, not codepoints — the truncation
// backs off to a UTF-8 boundary so a multi-byte character is never split in half.
inline constexpr std::size_t kMaxTitleBytes = 256;

// Trim, and collapse internal whitespace runs to single spaces. A local copy
// rather than a reach into the feed parser's helpers: this unit reads a JSON
// document and has no business depending on the XML one.
inline std::string collapseWs(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pending = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            pending = true;
        } else {
            if (pending && !out.empty()) out += ' ';
            pending = false;
            out += c;
        }
    }
    return out;
}

// Chapter titles are arbitrary publisher text on their way to curses. Two things
// have to go: ASCII control bytes, which sanitizeForDisplay passes through
// VERBATIM (it folds Unicode punctuation and rejects bad UTF-8, but its ASCII
// fast path copies any byte under 0x80, escape and newline included), and the
// Unicode punctuation sanitizeForDisplay exists to fold. Control bytes become
// spaces first so words either side of one do not fuse, then the display fold
// runs, then whitespace collapses and the result is trimmed.
inline std::string cleanChapterTitle(const std::string& raw) {
    std::string stripped;
    stripped.reserve(raw.size());
    for (unsigned char c : raw)
        stripped += (c < 0x20 || c == 0x7F) ? ' ' : (char)c;

    std::string t = collapseWs(sanitizeForDisplay(stripped));
    if (t.size() > kMaxTitleBytes) {
        std::size_t n = kMaxTitleBytes;
        while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) --n;   // don't split a sequence
        t.resize(n);
        t = collapseWs(t);          // a trailing partial word may leave a space
    }
    return t;
}

}  // namespace pc_detail

// Parse a chapters document. Returns chapters sorted by start time, deduplicated
// on exact-equal starts (keep-first), and capped. Empty for anything that is not
// a readable v1.1.0-shaped document — which the caller reports as no chapters.
inline std::vector<Mp4Chapter> parsePodcastChaptersJson(const std::string& doc) {
    using namespace pc_detail;
    std::vector<Mp4Chapter> out;
    if (doc.empty()) return out;

    // No-exceptions mode: a parse failure yields a discarded value instead of
    // throwing, so nothing here can escape into the UI thread.
    nlohmann::json j = nlohmann::json::parse(doc, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return out;

    // The array IS the contract. "version" is read when present because the format
    // records it, but an absent or unfamiliar version does not reject a document
    // whose chapters we can plainly read.
    auto it = j.find("chapters");
    if (it == j.end() || !it->is_array()) return out;

    // Walking the array is linear and the array is already in memory: whoever
    // handed us this string bounded it (the fetch caps the body, the sidecar read
    // caps the file), so there is no unbounded work to guard against here.
    out.reserve(std::min<std::size_t>(it->size(), kMaxChapters));
    for (const auto& e : *it) {
        if (!e.is_object()) continue;
        auto st = e.find("startTime");
        if (st == e.end() || !st->is_number()) continue;      // absent / string / null -> skip
        double start = st->get<double>();
        if (!std::isfinite(start) || start < 0.0) continue;   // NaN, inf, negative -> skip

        Mp4Chapter ch;
        ch.start_sec = start;
        auto ti = e.find("title");
        if (ti != e.end() && ti->is_string())
            ch.title = cleanChapterTitle(ti->get<std::string>());
        // A missing, non-string, or empty-after-cleaning title is equally useless;
        // all three are numbered below, once the final order is known.
        // Per-chapter img / url / toc extras are deliberately ignored: the model
        // is a start point and a name.
        out.push_back(std::move(ch));
    }

    // Out-of-order documents exist in the wild. Stable, so "keep the first of a
    // duplicate pair" below means the first as PUBLISHED.
    std::stable_sort(out.begin(), out.end(),
                     [](const Mp4Chapter& a, const Mp4Chapter& b) {
                         return a.start_sec < b.start_sec;
                     });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Mp4Chapter& a, const Mp4Chapter& b) {
                              return a.start_sec == b.start_sec;
                          }),
              out.end());
    if (out.size() > kMaxChapters) out.resize(kMaxChapters);

    // Name the unnamed by their place in the finished list, so the pane always
    // reads top to bottom even when a document titles only some of its chapters.
    // A start time past the end of the file is KEPT: seekTo already clamps to the
    // track duration, and dropping it would silently lose a real chapter over a
    // duration we may not know yet.
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i].title.empty()) out[i].title = "Chapter " + std::to_string(i + 1);

    return out;
}
