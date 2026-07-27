#pragma once

// ─── Which marked playlist rows mean which TOC tracks ────────────────────────
//
// CD-S3. The playlist holds SYNTHETIC CD rows ("G:CD Track 05"). The rip engine
// takes 0-based TOC INDICES (CDRipper::start's `selected_toc`). This is the one
// place that converts between them, and it is pure so it can be proven with no
// drive, no disc and no device.
//
// THE TRAP, and the reason this is a named function rather than an inline loop:
// a track's NUMBER is not its TOC INDEX. `number == index + 1` happens to hold
// on an ordinary audio CD, so `num - 1` would pass every test written against
// one - and then quietly select the wrong track on a disc whose numbering does
// not start at 1 or is not contiguous, which is what a data track or a mixed-mode
// pressing produces. So the number is LOOKED UP in the TOC, never arithmetic.
// This is the same class as CD-S1's finding (5): two quantities that coincide on
// the common case and are silently different on the one that matters.
//
// DRIVE IS PART OF IDENTITY. A synthetic path is disc-INDEPENDENT - "G:CD Track
// 05" matches track 5 of whatever disc happens to be in G:. The UI clears the
// selection on media change for exactly that reason, and this function refuses a
// path whose drive spec is not the one being ripped as a second line of defence.
//
// THE DROP IS COUNTED, NEVER SILENT (CD-S3 addition B). `unresolved` is expected
// to be 0 on every rip today, which is precisely why it is returned rather than
// discarded: the day it is not zero, something a user marked is being thrown
// away. It is also where a future HTOA row would surface - HTOA has NO TOC entry,
// so it would land here as "unresolved" and be dropped. That is deliberate for
// now: HTOA cannot be expressed as a TOC index, so it needs a companion channel
// on start() rather than a sentinel value in this vector (a -1 here would be the
// one-value-two-meanings defect this campaign has already closed twice). This
// function's job is to make that arrival a visible change at one site instead of
// a behaviour that silently differs.

#include "StringUtils.h"   // isCDTrackPath, parseCDPath

#include <algorithm>
#include <string>
#include <vector>

namespace cdsel {

struct MapResult {
    // 0-based TOC indices, sorted into TOC order and deduplicated - the exact
    // shape CDRipper::start's `selected_toc` wants.
    std::vector<int> toc_indices;
    // Marked paths that named no track on this disc: a foreign drive, a stale
    // row, or a row kind with no TOC entry. Counted so a drop can never be silent.
    int unresolved = 0;
};

// `toc_numbers[i]` is the track NUMBER of TOC entry `i`. The caller passes it
// rather than a vector<CDTrack> so this header stays free of any CD/device
// dependency - the repo norm that keeps helpers testable (StringUtils.h).
inline MapResult toTocIndices(const std::vector<std::string>& marked_paths,
                              const std::string&              drive_spec,
                              const std::vector<int>&         toc_numbers) {
    MapResult out;
    out.toc_indices.reserve(marked_paths.size());

    for (const std::string& p : marked_paths) {
        std::string drv;
        int         num = 0;
        // Not a CD row at all, or a row belonging to a different drive.
        if (!parseCDPath(p, drv, num) || drv != drive_spec) { ++out.unresolved; continue; }

        // LOOK THE NUMBER UP. Never `num - 1`.
        int idx = -1;
        for (int i = 0; i < (int)toc_numbers.size(); ++i)
            if (toc_numbers[(size_t)i] == num) { idx = i; break; }
        if (idx < 0) { ++out.unresolved; continue; }

        out.toc_indices.push_back(idx);
    }

    std::sort(out.toc_indices.begin(), out.toc_indices.end());
    out.toc_indices.erase(std::unique(out.toc_indices.begin(), out.toc_indices.end()),
                          out.toc_indices.end());
    return out;
}

// Is this selection the whole disc? Empty means ALL (CDRipper::start's contract),
// so an empty set and a set naming every track are the same rip - and both must
// answer true, or the modal would show a "partial" summary line for a selection
// that is not partial.
//
// CD-S3 addition A: unmarking the last marked track returns to ripping EVERY
// track, not to ripping nothing. That is the only reading consistent with
// "empty means all", and there is deliberately no way to express "rip nothing" -
// start() already refuses an empty plan, and the way to not rip is to not rip.
inline bool isWholeDiscSelection(const MapResult& m, int disc_total) {
    return m.toc_indices.empty() || (int)m.toc_indices.size() >= disc_total;
}

} // namespace cdsel
