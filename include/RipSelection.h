#pragma once

// ─── Which tracks to rip, and which track each one IS on the disc ────────────
//
// CD-S1. A rip has always taken the whole disc, so ONE number said both "how
// many tracks are on this disc" and "how many are we ripping", and ONE index
// said both "position in the TOC" and "position in this rip". They were only
// ever correct because they coincided.
//
// THE FIVE THINGS THAT READ THE DISC MEANING, and would silently take the
// selection meaning if a shorter vector were simply handed in:
//
//   1. the AccurateRip disc ID       - summed and POSITION-WEIGHTED over the
//                                      vector, so a subset yields a different
//                                      ID for an unchanged disc
//   2. the AR response chunk filter  - `tc != ntracks` rejects every chunk
//   3. AR result indexing            - out_v1[t] is keyed by vector position
//   4. the multi-disc pick           - identifies the medium BY TRACK COUNT
//   5. is_first / is_last            - **these choose the AR CRC skip window**
//
// (5) IS THE DANGEROUS ONE. AccurateRip's flat-disc scheme excludes AR_SKIP
// samples at the very start of the disc's first track and the very end of its
// last. Those flags were computed as `i==0` and `i==total-1` - LOOP position.
// Rip track 5 alone and `i==0` is true, so AR_SKIP samples are cut from track
// 5's checksum: a number that can never match AccurateRip, wrong for a reason
// no log line would reveal. The other four fail to verify; this one verifies
// against nothing while looking like it worked.
//
// So: is_first and is_last are DISC facts and are computed HERE from toc_index,
// never from a loop counter. That is the whole of (5)'s fix.
//
// PURE: no CD, no device, no network, no filesystem. The test links nothing.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ripsel {

// One track to extract, carrying its DISC identity next to its selection
// identity. `toc_index` is the position in the FULL table of contents and is
// what every disc-meaning consumer must use - the AR verdict slot above all.
struct Item {
    int  toc_index = 0;
    bool is_first  = false;   // the DISC's first track, not the plan's
    bool is_last   = false;   // the DISC's last track, not the plan's
};

inline bool operator==(const Item& a, const Item& b) {
    return a.toc_index == b.toc_index && a.is_first == b.is_first
        && a.is_last == b.is_last;
}

// Build the extraction plan from TOC indices.
//
// Defensive, because the caller is a UI in a later slice: out-of-range indices
// are dropped, duplicates are dropped, and the result is SORTED INTO TOC ORDER.
// That last property is deliberate and matches the contract rip_sel_ already
// keeps for formats - the rip must not depend on the order the user happened to
// toggle things in.
inline std::vector<Item> plan(int disc_total, const std::vector<int>& selected) {
    std::vector<Item> out;
    if (disc_total <= 0) return out;
    std::vector<int> idx;
    idx.reserve(selected.size());
    for (int i : selected)
        if (i >= 0 && i < disc_total) idx.push_back(i);
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    out.reserve(idx.size());
    for (int i : idx)
        out.push_back(Item{ i, i == 0, i == disc_total - 1 });
    return out;
}

// The default, and the whole of this slice's "nothing changed" claim: every
// track in TOC order. Substituted into the call sites this reproduces exactly
// the arguments the worker passes today - toc_index == i, is_first == (i==0),
// is_last == (i==total-1).
inline std::vector<Item> planAll(int disc_total) {
    std::vector<Item> out;
    if (disc_total <= 0) return out;
    out.reserve((std::size_t)disc_total);
    for (int i = 0; i < disc_total; ++i)
        out.push_back(Item{ i, i == 0, i == disc_total - 1 });
    return out;
}

// Is this plan the whole disc? The side-product rulings in CD-S2 turn on it
// (a cue sheet, album gain and CTDB describe a DISC), and the modal's summary
// line in CD-S3 shows nothing when this is true - so the default path draws
// exactly as it does today.
inline bool isWholeDisc(int disc_total, const std::vector<Item>& p) {
    return disc_total > 0 && (int)p.size() == disc_total;
}

} // namespace ripsel
