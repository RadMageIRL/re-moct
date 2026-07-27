#pragma once

// ─── Hidden track one audio: is there any, and where ─────────────────────────
//
// A CD's first track does not have to start at the beginning of the disc. When
// it starts later, the addressable audio ahead of it is playable but is not a
// track: no TOC entry describes it, so nothing that walks the TOC can see it.
// Some records put a song there. They Might Be Giants' Factory Showroom puts
// 61 seconds of "Token Back to Brooklyn" in front of track 1.
//
// DETECTION IS THE TOC AND NOTHING ELSE. The pregap length is known the instant
// the disc is identified, so the answer costs no read, no spin-up and no wait.
// It is deliberately not a silence test: reading before answering would mean
// spinning the drive on every disc to learn something about almost none of
// them, and a drive that returns zeros with a success status is
// indistinguishable from a genuinely silent pregap without a reference disc.
// The cost of TOC-only detection is that a disc can offer a hidden track that
// turns out to be silence. That is accepted and it is the ruling.
//
// THE THRESHOLD IS 375 SECTORS - five seconds - AND IT IS STRICTLY GREATER.
// It comes from CUETools, which emits an HTOA file only when
// `_toc.Pregap > 75 * 5` (CUESheet.cs:2307). Ordinary discs carry a short
// pregap for entirely mechanical reasons: Relish, the CD-path regression disc,
// starts track 1 at LBA 32, which is 0.427 s of nothing in particular. Matching
// an established tool matters more here than picking our own number, and the
// margin between 32 and 375 is wide enough that the gate disc cannot grow a
// hidden track - which is what makes "a disc without HTOA behaves exactly as it
// did" provable rather than asserted.
//
// The span is always anchored at LBA 0. Every reference implementation extracts
// LBA 0 through track 1's INDEX 01 minus one, and none of them reads a negative
// address: cdparanoia returns 0 from `cdda_track_firstsector(d, 0)` with the
// comment "pre-gap of first track always starts at lba 0" (toc.c:26), whipper's
// getHTOA() returns index 0's absolute position (program.py:511), and CUERipper
// derives it from the same pregap length. We take the TOC's number, as
// cdparanoia does, rather than reading the subchannel for it.
//
// THE ARGUMENT IS AN LBA, NOT A FRAME. Hand this `CDTrack::lba()`, never
// `start_frame` - the two differ by the 150-frame lead-in, and passing the
// wrong one silently moves the span by two seconds and shifts the threshold
// comparison by the same amount. Taking a bare LBA rather than the track struct
// is what keeps this header pure: the test links nothing, needs no disc, no
// device and no filesystem, exactly as RipSelection.h does.

#include <cstdint>
#include <optional>

namespace htoa {

// CUETools' threshold, in sectors. Comparison is strictly greater: a pregap of
// exactly 375 sectors does not qualify.
inline constexpr uint32_t kMinPregapSectors = 375;

// Sectors per second on a Red Book disc. Here so the duration helper does not
// mint a second copy of a constant the rest of the tree already knows.
inline constexpr uint32_t kSectorsPerSecond = 75;

// The extraction span. `sectors` is a COUNT, so the last readable sector is
// `sectors - 1` and a zero-length span is never produced - span() returns
// nullopt instead, so "no hidden track" is a distinct value rather than a
// length of zero that reads like one.
struct Span {
    uint32_t start_lba = 0;   // always 0; named rather than implied
    uint32_t sectors   = 0;   // length in sectors

    uint32_t lastLba()  const { return sectors - 1; }
    double   seconds()  const { return (double)sectors / kSectorsPerSecond; }
};

// Does this disc offer hidden track one audio? `track1_lba` is track 1's read
// address - CDTrack::lba().
inline bool present(uint32_t track1_lba) {
    return track1_lba > kMinPregapSectors;
}

// The span to extract, or nullopt when the disc has no hidden track. The whole
// pregap is taken: LBA 0 through track1_lba - 1.
inline std::optional<Span> span(uint32_t track1_lba) {
    if (!present(track1_lba)) return std::nullopt;
    return Span{ 0, track1_lba };
}

} // namespace htoa
