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

    if(!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
