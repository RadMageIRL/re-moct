// ArtEmbed.h - embedded cover-art carryover (art-embed-shared slice).
//
// The READER is the genuinely new piece: nothing in RE-MOCT extracts an
// embedded picture OUT of an encoded file today (rip and recorder receive art
// as raw bytes from the network/CoverArt module and write it in). Convert needs
// to pull the source file's own cover across to the output, so this module adds:
//   - extractEmbeddedArt: per-format picture extraction (front cover preferred).
//   - embedArt: the convert-side writer, all encodable formats.
//
// Scope note (Finding B): this is READER-ONLY SHARED. The rip (CDRipper::tagFile)
// and recorder (StreamRecorder::tagCut) art writers stay INLINE and untouched -
// consolidating them here is not cleanly byte-identical (rip hardcodes
// image/jpeg while rec derives MIME from magic; a shared writer would also add a
// second save pass), and their output is under regression gates. embedArt is
// used by convert only, whose outputs are freshly created (a separate art save
// is fine there).
#pragma once

#include "RipFormats.h"   // RipFormat

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ArtBlob {
    std::vector<uint8_t> bytes;
    std::string          mime;   // e.g. "image/jpeg" / "image/png"
};

// Extract the source file's embedded front cover (or the first picture). Returns
// the raw image bytes + its stored MIME. nullopt when there is no embedded art
// (including WAV, which has no such concept) or the file cannot be read.
std::optional<ArtBlob> extractEmbeddedArt(const std::string& path);

// Write the blob as the output's embedded cover, per format (MP3 APIC / FLAC +
// Opus FLAC::Picture / MP4 covr / WavPack APEv2 binary). WAV is a no-op. Best-
// effort: false on any failure, never throws. Convert-side only.
bool embedArt(const std::string& path, RipFormat fmt, const ArtBlob& blob);
