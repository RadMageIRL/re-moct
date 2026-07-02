// Pure unit tests for ar_crc — synthetic vectors, no disc. Returns nonzero on fail.
#include "ar_crc.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

int main(){
    // ---- frame450Crcs: hand-computed small vectors ----
    { int16_t pcm[2] = {1,0};                       // one frame, s=1
      auto r = ar::frame450Crcs(pcm, 0, 1);
      CHECK(r.first  == 1u);
      CHECK(r.second == ar::FRAME450_START); }
    { int16_t pcm[4] = {1,0, 1,0};                  // two frames, both s=1
      auto r = ar::frame450Crcs(pcm, 0, 2);
      CHECK(r.first  == 3u);                                          // 1*1 + 1*2
      CHECK(r.second == ar::FRAME450_START + (ar::FRAME450_START+1u)); }
    { int16_t pcm[4] = {9,9, 1,0};                  // start offset skips frame 0
      auto r = ar::frame450Crcs(pcm, 1, 1);
      CHECK(r.first == 1u); CHECK(r.second == ar::FRAME450_START); }

    // ---- TrackCrc: v1/v2 + gating + mul_by advance ----
    { ar::TrackCrc a(1, 1000, false, true);         // s=2 at mul_by 1,2,3 -> 12
      a.sample(2,0); a.sample(2,0); a.sample(2,0);
      CHECK(a.v1()==12u); CHECK(a.v2()==12u); CHECK(a.nAccumulated()==3u); }
    { ar::TrackCrc a(2, 1000, false, true);         // checkFrom=2 drops mul_by=1
      a.sample(2,0); a.sample(2,0); a.sample(2,0);
      CHECK(a.v1()==10u); CHECK(a.nAccumulated()==2u); }
    { ar::TrackCrc a(1, 1000, false, false);        // disabled (isLocal): nothing
      a.sample(5,5); a.sample(5,5);
      CHECK(a.v1()==0u); CHECK(a.nAccumulated()==0u); }
    { ar::TrackCrc a(1, 1000, false, true);         // skip advances mul_by
      a.skip(4); a.sample(1,0);                      // contributes 1*5
      CHECK(a.v1()==5u); }
    { ar::TrackCrc a(1, 1000, false, true);         // exercise csum_hi carry
      a.sample((int16_t)0xFFFF,(int16_t)0xFFFF);     // s=0xFFFFFFFF, mb=1
      a.sample((int16_t)0xFFFF,(int16_t)0xFFFF);     // mb=2 -> hi=1, lo wraps
      CHECK(a.csumHi()==1u); CHECK(a.csumLo()==0xFFFFFFFDu);
      CHECK(a.v2()==0xFFFFFFFEu); }

    // ═══ normalizeSkip / negative-offset OOB + wrong-phase fix ═══════════════
    // Synthetic-only. Proves the pure logic; final NEGATIVE-offset real-drive
    // behavior stays a HydrogenAudio cross-check item (no negative-offset HW here).

    constexpr int S = ar::SECTOR_SAMPLES;          // 588
    constexpr int PREGAP = 150 * S;                // 88200 — 150-sector preamble, UNCHANGED

    // ---- (A) normalization invariants over a sign-spanning sweep ----
    // 0 <= sub_skip < 588 AND lba_adv*588 + sub_skip == total_skip, for EVERY ts.
    // The non-negative sub_skip invariant is what structurally kills the OOB.
    for (int ts = -1200; ts <= 1200; ++ts) {
        auto p = ar::normalizeSkip(ts);
        CHECK(p.sub_skip >= 0);
        CHECK(p.sub_skip <  S);
        CHECK(p.lba_adv * S + p.sub_skip == ts);
    }

    // ---- (B) NON-NEGATIVE path is byte-identical to the old truncating / and % ----
    // (Requirement: the +6 / all-non-negative path must not change.)
    for (int ts = 0; ts <= 1200; ++ts) {
        auto p = ar::normalizeSkip(ts);
        CHECK(p.lba_adv  == ts / S);               // same as pre-fix inline math
        CHECK(p.sub_skip == ts % S);
    }

    // ---- (C) OOB witness: old math underflows the preamble ptr; fixed stays in-bounds ----
    for (int ts : { -1, -12, -600 }) {
        int old_sub = ts % S;                      // pre-fix truncating remainder
        CHECK(old_sub < 0);                        // ps = pbuf.data()+old_sub*2 -> OOB read
        auto p = ar::normalizeSkip(ts);
        // Mirror CDRipper's fixed preamble buffer sizing and index the same way.
        int  preamble_secs = ((PREGAP + p.sub_skip + S - 1) / S) + 1;
        long pbuf_len   = (long)preamble_secs * S * 2;          // int16 element count
        long first_idx  = (long)p.sub_skip * 2;                 // ps[0]
        long last_idx   = (long)(p.sub_skip + PREGAP - 1) * 2 + 1;  // ps[(PREGAP-1)*2+1]
        CHECK(first_idx >= 0);                      // no underflow
        CHECK(last_idx  <  pbuf_len);               // no overflow
    }

    // ---- (D) phase / CRC equivalence over a synthetic disc ----
    // Model: sample value at absolute disc frame `a` is the packed s = a, so the CRC
    // directly encodes which frame landed at which mul_by. The whole feed (150-sector
    // preamble + main) is a CONTIGUOUS run of offset-corrected disc frames starting at
    // trackRelStart*588 + total_skip. Feed the same values a real feed would, in order.
    const long long trackStartLba  = 200;                 // >= 151, room for adv = -2
    const long long trackRelStart  = trackStartLba - 150; // 50
    const int       N              = 3 * S;               // 1764 main frames (small track)

    auto feed = [](ar::TrackCrc& c, long long start, int count) {
        for (int j = 0; j < count; ++j) {
            uint32_t a = (uint32_t)(start + j);
            c.sample((int16_t)(uint16_t)(a & 0xFFFF),
                     (int16_t)(uint16_t)((a >> 16) & 0xFFFF));
        }
    };
    // Reference = ideal offset-corrected contiguous stream (ground truth, no decomposition).
    auto referenceCrc = [&](int ts) {
        ar::TrackCrc r(1, 0xFFFFFFFFu, false, true);
        feed(r, trackRelStart * S + ts, PREGAP + N);
        return r;
    };
    // Fixed = what the rip feeds via normalizeSkip decomposition (two segments that must
    // reconstruct the contiguous reference exactly, for either sign).
    auto fixedCrc = [&](int ts) {
        auto p = ar::normalizeSkip(ts);
        ar::TrackCrc f(1, 0xFFFFFFFFu, false, true);
        feed(f, (trackRelStart + p.lba_adv) * S + p.sub_skip, PREGAP);  // preamble
        feed(f, (trackStartLba  + p.lba_adv) * S + p.sub_skip, N);      // main
        return f;
    };

    for (int ts : { -600, -12, 0, 6, 588 }) {      // negatives, zero, +6, whole-sector
        auto ref = referenceCrc(ts), fx = fixedCrc(ts);
        CHECK(fx.v1() == ref.v1());
        CHECK(fx.v2() == ref.v2());
    }

    // Buggy negative path really computed the WRONG phase (not just crashed): the main
    // rip dropped the sub-sector skip (`> 0` guard) and truncated lba_adv, so its samples
    // land shifted vs the reference. Compare MAIN-only (preamble was OOB, covered by C).
    for (int ts : { -12, -600 }) {
        int adv_t = ts / S;                        // truncating advance (pre-fix)
        ar::TrackCrc bug(1, 0xFFFFFFFFu, false, true), refm(1, 0xFFFFFFFFu, false, true);
        feed(bug,  (trackStartLba + adv_t) * S + 0, N);   // sub-skip dropped
        feed(refm, trackStartLba * S + ts,          N);   // correct phase
        CHECK(bug.v1() != refm.v1());
    }

    // +6 (and any non-negative) main feed is byte-identical old-vs-fixed decomposition.
    for (int ts : { 6, 588, 600 }) {
        auto p = ar::normalizeSkip(ts);
        int adv_t = ts / S, sub_t = ts % S;        // old truncating == fixed for ts>=0
        CHECK(p.lba_adv == adv_t && p.sub_skip == sub_t);
        ar::TrackCrc oldc(1, 0xFFFFFFFFu, false, true), newc(1, 0xFFFFFFFFu, false, true);
        feed(oldc, (trackStartLba + adv_t)     * S + sub_t,      N);
        feed(newc, (trackStartLba + p.lba_adv) * S + p.sub_skip, N);
        CHECK(oldc.v1() == newc.v1());
        CHECK(oldc.v2() == newc.v2());
    }

    // ---- (E) boundaries: whole-sector, zero, and the signed preamble guard ----
    { auto p = ar::normalizeSkip(-588); CHECK(p.lba_adv == -1 && p.sub_skip == 0); }
    { auto p = ar::normalizeSkip( 588); CHECK(p.lba_adv ==  1 && p.sub_skip == 0); }
    { auto p = ar::normalizeSkip(   0); CHECK(p.lba_adv ==  0 && p.sub_skip == 0); }
    CHECK(ar::arPreambleReadable(200, -2) == true);    // 200-150-2 = 48  ok
    CHECK(ar::arPreambleReadable(151, -1) == true);    // 151-150-1 = 0   ok
    CHECK(ar::arPreambleReadable(150, -1) == false);   // 150-150-1 = -1  decline
    CHECK(ar::arPreambleReadable(150,  0) == true);    // normal track 1 (lba 150), no offset
    CHECK(ar::arPreambleReadable(182,  0) == true);    // Relish 32-frame pregap (lba 182)
    CHECK(ar::arPreambleReadable(300,  6) == true);    // +6 drive, normal

    if(!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
