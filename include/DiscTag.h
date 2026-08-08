#pragma once

// ─── DiscTag — the disc number, one definition per tag container ─────────────
//
// P5 of the release/disc-picker design. Until now RE-MOCT wrote NO disc number
// at all: a rip of disc 3 of 7 produced nineteen files whose only record of that
// fact was the folder name, so moving one file out of the folder lost it
// permanently. `grep -rn "DISCNUMBER\|TPOS" src/ include/` had exactly one hit,
// and it was the library scanner READING the tag other rippers write.
//
// ── Why this is a header and not five lines inside tagFile ───────────────────
// The writer and its round-trip test must share ONE definition. The risk being
// retired here is not tagFile's control flow — it is whether TagLib maps each
// container's native field onto the PropertyMap key LibraryScanner actually
// reads ("DISCNUMBER", see readTags in LibraryScanner.cpp). Asserting that
// mapping is not proving it, and a test that re-implements the write proves only
// that the test agrees with itself. disc_tag_test calls the functions below and
// then runs the REAL scanner over the result.
//
// ── Always written, including 1/1 ────────────────────────────────────────────
// Dos's ruling. Writing the number only on multi-disc releases would leave a
// missing DISCNUMBER meaning two different things — "this is a standalone disc"
// and "this was ripped before the tag existed" — which is the same
// value-collision CD-S2 closed in disc.json. Absence now means exactly one
// thing: not written by this program.
//
// ── Each container gets its NATIVE field, not a common denominator ───────────
//   ID3v2  (MP3)          TPOS         "3/7"   — the standard "part of a set" frame
//   Xiph   (FLAC, Opus)   DISCNUMBER   "3"     + TOTALDISCS "7"
//   APEv2  (WavPack)      DISC         "3/7"
//   MP4    (M4A)          disk         integer pair (3, 7)
//
// WAV has no tag container and never reaches here — the `taggable` column in
// kRipFormats already routes around it.

#include <taglib/tstring.h>
#include <taglib/tbytevector.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/xiphcomment.h>
#include <taglib/apetag.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4item.h>

#include <algorithm>
#include <string>

namespace disctag {

// "3/7" — the shape ID3v2's TPOS and APEv2's DISC both take.
inline std::string pairString(int disc, int total) {
    return std::to_string(disc) + "/" + std::to_string(total);
}

// A disc number below 1 is not a disc number. pickDiscForTrackCount cannot
// return one (it falls back to 1), so this is a guard against a future caller,
// not against today's — and it declines to write rather than writing a zero that
// a reader would have to interpret.
inline bool valid(int disc) { return disc >= 1; }

// "3 of 2" is not representable and would be worse than saying nothing about the
// total. The rip path cannot produce it (total_discs is the max over the same
// tracks pickDiscForTrackCount chose from), so this only ever fires for a caller
// that has not computed the pair together.
inline int clampTotal(int disc, int total) { return std::max(total, disc); }

// ── ID3v2 (MP3) ─────────────────────────────────────────────────────────────
// TPOS is a STANDARD text frame, so it takes the ordinary
// TextIdentificationFrame construction — not the TXXX/UserText shape the
// AccurateRip and ReplayGain keys need (mp3-rg-write: a TXXX written as one
// "KEY=value" blob lands entirely in the description and is invisible to every
// reader). TENC next door is the matching precedent.
//
// removeFrames first, unlike the inline TENC write beside it: addFrame APPENDS,
// and this helper is reachable from a test that may tag a file more than once.
// A duplicate TPOS is a legal ID3v2 tag that different readers resolve
// differently, which is a bug that would only ever appear on someone else's
// player.
inline void writeDiscTag(TagLib::ID3v2::Tag* tag, int disc, int total) {
    if (!tag || !valid(disc)) return;
    total = clampTotal(disc, total);
    tag->removeFrames("TPOS");
    auto* fr = new TagLib::ID3v2::TextIdentificationFrame("TPOS", TagLib::String::UTF8);
    fr->setText(TagLib::String(pairString(disc, total), TagLib::String::UTF8));
    tag->addFrame(fr);
}

// ── Xiph comments (FLAC and Opus) ───────────────────────────────────────────
// Two separate fields rather than one "3/7": that is the Vorbis-comment
// convention, and it is what the FLAC and Opus branches of tagFile already do
// for every other pair of related values. addField(..., replace=true) makes this
// idempotent with no explicit removal.
inline void writeDiscTag(TagLib::Ogg::XiphComment* tag, int disc, int total) {
    if (!tag || !valid(disc)) return;
    total = clampTotal(disc, total);
    tag->addField("DISCNUMBER",
                  TagLib::String(std::to_string(disc),  TagLib::String::UTF8), true);
    tag->addField("TOTALDISCS",
                  TagLib::String(std::to_string(total), TagLib::String::UTF8), true);
}

// ── APEv2 (WavPack) ─────────────────────────────────────────────────────────
// "DISC" is the APEv2 convention. Uppercase matches the AccurateRip / CTDB /
// ReplayGain keys the same branch writes. addValue(..., replace=true) replaces.
inline void writeDiscTag(TagLib::APE::Tag* tag, int disc, int total) {
    if (!tag || !valid(disc)) return;
    total = clampTotal(disc, total);
    tag->addValue("DISC",
                  TagLib::String(pairString(disc, total), TagLib::String::UTF8), true);
}

// ── MP4 (M4A) ───────────────────────────────────────────────────────────────
// The "disk" atom is an INTEGER PAIR, not text — the same shape "trkn" takes for
// the track number, which is why setTrack() exists for one and nothing does for
// the other. setItem replaces.
inline void writeDiscTag(TagLib::MP4::Tag* tag, int disc, int total) {
    if (!tag || !valid(disc)) return;
    total = clampTotal(disc, total);
    tag->setItem("disk", TagLib::MP4::Item(disc, total));
}

}  // namespace disctag
