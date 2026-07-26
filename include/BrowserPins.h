#pragma once

// ─── The browser's pinned rows: ONE list, read by both readers ───────────────
//
// The directory pane puts its section pseudo-entries above the directory
// contents. That happens in two places - refreshDir() pushes them, and the sort
// comparator pins them - and before this header those were two hand-maintained
// lists that had to agree.
//
// They stopped agreeing. [Library] was added to the pushes and not to the
// comparator, so it fell through to the file/directory comparison, where
// fs::is_directory(current_dir_ / "[Library]") is false, sorted as an ordinary
// file, and rendered at the BOTTOM of the pane. It failed a hardware gate. The
// insert had been correct the whole time; the sort moved it.
//
// So the list lives here once. Adding a section means adding one line to kPins,
// and a section that is pushed but not pinned - or pinned but not pushed - is no
// longer something to remember, because there is only one list to add it to.
//
// PURE and dependency-free: no filesystem, no curses, so the order is asserted by
// a unit test rather than by reading the code and hoping.

#include <cstddef>
#include <string>

namespace browserpins {

// The rendered order, top to bottom. Everything not in this list sorts after all
// of it, in whatever order the active browser sort produces.
//
// ".." is last because it is navigation rather than a section, and it is the only
// entry pushed conditionally - a filesystem root has no parent to go up to.
inline const char* const kPins[] = {
    "[Drives]",
    "[Recent]",
    "[FAVs]",
    "[Radio]",
    "[Podcasts]",
    "[Books]",
    "[Library]",
    "..",
};
inline constexpr std::size_t kCount = sizeof(kPins) / sizeof(kPins[0]);

// Index of `e` in the pinned order, or -1 when it is not a pinned row.
inline int rank(const std::string& e) {
    for (std::size_t i = 0; i < kCount; ++i)
        if (e == kPins[i]) return static_cast<int>(i);
    return -1;
}

inline bool isPin(const std::string& e) { return rank(e) >= 0; }

// ── Which pins are actually SHOWN (library slice 6) ──────────────────────────
// [Library] is behind a config toggle, and "off" means the row is absent rather
// than present-and-inert. That filter has to apply to every reader or the section
// half-exists: before slice 6 there were THREE lists of these names - refreshDir's
// pushes, the sort comparator, and enterDriveList's own hand-written reverse-order
// inserts, which read none of this header. Slice 6 routed enterDriveList here too,
// so the toggle is one predicate consulted by all of them.
//
// Deliberately a plain parameter rather than a stored flag: this header stays pure
// and dependency-free, so the order AND the filtering are asserted by a unit test
// instead of by reading the code and hoping.
inline bool shown(const std::string& e, bool library_enabled) {
    if (!library_enabled && e == "[Library]") return false;
    return true;
}

// Rank within the SHOWN order. -1 for a row that is not pinned or not shown, so a
// hidden section sorts as ordinary content rather than being pinned invisibly.
inline int shownRank(const std::string& e, bool library_enabled) {
    return shown(e, library_enabled) ? rank(e) : -1;
}

// The comparator's pinned-row rule, as a predicate the sort can call directly.
// Returns true when `a` must sort before `b` ON ACCOUNT OF PINNING, and reports
// through `decided` whether pinning settled the question at all.
//
// STRICT WEAK ORDERING: two rows with the SAME rank compare false, which is the
// defect this replaces. The previous form ran a sequence of
//   if (ea == "[Books]") return true;  if (eb == "[Books]") return false;
// so comparing a pinned row with ITSELF returned true - comp(a, a) == true is
// undefined behaviour in std::sort, latent in shipped code because duplicate
// pseudo-entries never actually occur. Ranks make it correct by construction.
inline bool before(const std::string& a, const std::string& b, bool& decided,
                   bool library_enabled = true) {
    const int ra = shownRank(a, library_enabled);
    const int rb = shownRank(b, library_enabled);
    if (ra < 0 && rb < 0) { decided = false; return false; }   // neither pinned
    decided = true;
    if (ra < 0) return false;    // only b is pinned -> b first
    if (rb < 0) return true;     // only a is pinned -> a first
    return ra < rb;              // both pinned -> by rank; equal -> false
}

} // namespace browserpins
