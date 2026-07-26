#pragma once

// ─── The browser's virtual-section flags, as one set ─────────────────────────
//
// The directory browser shows one of two things: the real contents of
// `current_dir_`, or a VIRTUAL SECTION - [Drives], [Recent], [FAVs], [Radio],
// [Books], [Podcasts], [Library] - which populates the same two row vectors from
// somewhere else entirely.
//
// WHY THIS FILE EXISTS. That set of flags was written out by hand in four places
// and they did not agree:
//
//   refreshDir()      cleared all ten, and says "BOTH sites, or a refresh leaks"
//   enterDriveList()  cleared all ten, and says "keep the two sites identical"
//   the dir poll      tested exactly ONE of them - `!in_drive_list_`
//   (a fourth, in an earlier form, missed the two search sub-modes entirely)
//
// The poll's copy was six sections out of date, and the consequence was not
// cosmetic: it ran `refreshDir()` - the very function that tears every section
// down - while a section was on screen, so [Library] and five others evicted
// themselves whenever the folder the browser had last shown changed on disk. On
// Linux under /mnt/hgfs that happened by itself, about a second after entry.
//
// Two hand-maintained copies were already documented as a hazard IN THE CODE
// ("the reset trap", "the completeness of this list IS the fix"). This header is
// the fix those comments were asking for: the set is enumerated exactly ONCE, in
// UIManager's `section_flags_` array beside the members themselves, and the two
// things anyone ever does with it live here, pure and testable.
//
// SHAPE. These take a pointer-array and a count rather than a struct of bools,
// deliberately: turning the ten members into a struct would rename 262 reference
// sites across UIManager for no behavioural gain, and a wide mechanical rename is
// a worse trade than a narrow one. UIManager keeps its members; this owns the
// operations, and the array that names them is the single enumeration.
//
// PURE: no curses, no filesystem, no UIManager. The test links nothing.

#include <cstddef>

namespace browsersec {

// Is the browser showing something OTHER than the real contents of current_dir_?
//
// THE PRECONDITION THIS EXPRESSES, which is the thing to keep in mind rather than
// the list that currently implements it: anything keyed on `current_dir_` - the
// mtime poll above all - is MEANINGLESS while a virtual section is on screen,
// because the rows on screen did not come from that directory. A section's flag
// being set is how we know that; it is not itself the reason.
//
// Sub-mode flags (a podcast feed, a radio search) are included even though their
// parent section's flag is always set alongside them. Including them costs one
// comparison and means a sub-mode surviving alone - which is exactly the bug the
// enterDriveList comment describes - still reads as "in a section", which is the
// safe answer.
inline bool anySet(bool* const* flags, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (flags[i] && *flags[i]) return true;
    return false;
}

// Clear every flag in the set. The two reset sites call this instead of writing
// the list out, so they cannot drift apart - which is the entire point, since
// both already carried a comment begging future readers to keep them in step.
inline void clearAll(bool* const* flags, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (flags[i]) *flags[i] = false;
}

} // namespace browsersec
