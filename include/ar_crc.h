// ar_crc.h — pure AccurateRip CRC math, extracted for off-disc unit testing.
// No windows.h / disc I/O. The CD-buffer packing + sector read loop stay in
// CDRipper; this owns only the checksum arithmetic.
#pragma once
#include <cstdint>
#include <utility>

namespace ar {

// Sector 450 of track 1, track-relative, used for pressing-offset detection.
inline constexpr uint32_t FRAME450_START = 450u * 588u + 1u;  // mul_by 264601
inline constexpr uint32_t FRAME450_END   = 451u * 588u;       // mul_by 265188

// Frame-450 offset-find CRCs over `len` stereo frames starting at frame `start` of
// an interleaved int16 L/R buffer (packed L-low | R-high, as on disc):
//   .first  (local)  uses mul 1..len          (k+1)
//   .second (global) uses mul FRAME450_START.. (FRAME450_START + k)
// Caller guarantees [start, start+len) is in range (CDRipper bounds-checks the sweep).
std::pair<uint32_t,uint32_t> frame450Crcs(const int16_t* pcm, int start, int len);

// Streaming AccurateRip v1/v2 accumulator (whipper formula). Fed one stereo frame at
// a time at the SAME three sites the rip loop used (preamble, main, tail); advances
// mul_by every call. v1 = csum_lo ; v2 = csum_lo + csum_hi.
class TrackCrc {
public:
    // checkFrom/checkTo gate which disc-absolute mul_by positions contribute;
    // isFirst enables the track-1 frame-450 accumulation; enabled == !isLocal(mode).
    TrackCrc(uint32_t checkFrom, uint32_t checkTo, bool isFirst, bool enabled)
        : checkFrom_(checkFrom), checkTo_(checkTo), isFirst_(isFirst), enabled_(enabled) {}

    void sample(int16_t l, int16_t r) {
        const uint32_t s = (uint32_t)(uint16_t)l | ((uint32_t)(uint16_t)r << 16);
        if (enabled_ && mulBy_ >= checkFrom_ && mulBy_ <= checkTo_) {
            const uint64_t product = (uint64_t)s * (uint64_t)mulBy_;
            csumHi_ += (uint32_t)(product >> 32);
            csumLo_ += (uint32_t)(product);
            if (nAccum_ == 0) {                  // first contribution (cross-check log)
                firstSamp32_    = s;
                firstContribLo_ = (uint32_t)product;
                firstMulBy_     = mulBy_;
            }
            ++nAccum_;
        }
        if (isFirst_ && mulBy_ >= FRAME450_START && mulBy_ <= FRAME450_END) {
            frame450Global_ += s * mulBy_;
            frame450Local_  += s * (mulBy_ - FRAME450_START + 1);
        }
        ++mulBy_;
    }

    // Advance mul_by without accumulating ("preamble before disc start" path).
    void skip(uint32_t n) { mulBy_ += n; }

    uint32_t v1()  const { return csumLo_; }
    uint32_t v2()  const { return csumLo_ + csumHi_; }
    uint32_t csumLo() const { return csumLo_; }
    uint32_t csumHi() const { return csumHi_; }
    uint32_t frame450Local()  const { return frame450Local_; }
    uint32_t frame450Global() const { return frame450Global_; }
    uint64_t nAccumulated()   const { return nAccum_; }
    uint32_t firstSamp32()    const { return firstSamp32_; }
    uint32_t firstContribLo() const { return firstContribLo_; }
    uint32_t firstMulBy()     const { return firstMulBy_; }

private:
    uint32_t checkFrom_, checkTo_;
    bool     isFirst_, enabled_;
    uint32_t mulBy_  = 1u;
    uint32_t csumLo_ = 0, csumHi_ = 0;
    uint32_t frame450Local_ = 0, frame450Global_ = 0;
    uint64_t nAccum_ = 0;
    uint32_t firstSamp32_ = 0, firstContribLo_ = 0, firstMulBy_ = 0;
};

// ── Drive / pressing offset normalization ────────────────────────────────────
// The rip applies a signed total sample skip (drive_offset + pressing_offset) as a
// whole-sector LBA advance PLUS a sub-sector sample skip. Decompose with FLOORED
// division so the sub-sector remainder is ALWAYS non-negative for either sign:
//     lba_adv * SECTOR_SAMPLES + sub_skip == total_skip,   with 0 <= sub_skip < 588.
// The pre-fix inline math used truncating '/' and '%', which for a NEGATIVE offset
// produced a negative sub_skip that (a) underflowed the preamble source pointer
// (pbuf.data() + sub_skip*2) -> out-of-bounds read, and (b) was silently dropped on
// the main-rip path (gated `> 0`) -> samples fed at the wrong disc-absolute mul_by
// (wrong CRC phase). For NON-NEGATIVE total_skip this is byte-identical to '/' & '%'.
inline constexpr int SECTOR_SAMPLES = 588;   // stereo frames per CD sector (2352/4)

struct SkipParts { int lba_adv; int sub_skip; };

inline SkipParts normalizeSkip(int total_skip) {
    int adv = total_skip / SECTOR_SAMPLES;
    int sub = total_skip % SECTOR_SAMPLES;
    if (sub < 0) { sub += SECTOR_SAMPLES; --adv; }   // floor: keep 0 <= sub < 588
    return SkipParts{ adv, sub };
}

// The AR preamble reads 150 sectors before the offset-corrected track start. With a
// negative lba_adv that start moves earlier, so bounds-check in SIGNED space: a track
// too close to disc LBA 0 declines gracefully instead of underflowing. (The pre-fix
// guard cast `(DWORD)corr_lba_adv`, wrapping a negative advance to ~4e9.) 150 = the
// physical AccurateRip lead-in preamble — unchanged, a property of the disc by design.
inline bool arPreambleReadable(uint32_t track_start_lba, int lba_adv) {
    return (long long)track_start_lba - 150 + lba_adv >= 0;
}

} // namespace ar
