// R128Gain.h — the R128 <-> ReplayGain conversion, BOTH directions, one home.
//
// RFC 7845: Opus carries gain as R128_TRACK_GAIN / R128_ALBUM_GAIN — an
// INTEGER in Q7.8 fixed-point dB (value/256), referenced to -23 LUFS. The
// ReplayGain convention this codebase speaks everywhere else references
// -18 LUFS, hence the +/-5 dB rebase.
//
// Encode (CDRipper::tagFile Opus branch) and decode (LocalFileSource's RG
// read) BOTH call here and nowhere else, so the two directions cannot drift
// — getting either sign/reference wrong plays Opus ~5 dB off (the decode
// slice found exactly that bug in the wild). The round trip is EXACT for
// every representable Q7.8 value: /256 and +/-5 are exact in binary floating
// point over the integer range — asserted over the full range by the
// round-trip test in rip_encoder_seam_test, not argued.
#pragma once

#include <cmath>

// Q7.8 tag value -> ReplayGain dB (the decode direction).
inline float dbFromR128(int q78) {
    return (float)q78 / 256.0f + 5.0f;
}

// ReplayGain dB -> Q7.8 tag value (the encode direction; exact inverse).
inline int r128FromDb(double db) {
    return (int)std::lround((db - 5.0) * 256.0);
}
