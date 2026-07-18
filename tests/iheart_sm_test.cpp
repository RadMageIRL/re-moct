// Pure unit tests for IHeartNowPlayingSM — no deps, returns nonzero on failure.
// Real golden fixtures (derived from iheart_http_dump / SniffIHeartRadio captures)
// land in a follow-up; this scaffold pins the debounce + floor-stall invariants.
#include "IHeartNowPlayingSM.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

static IHeartTick base(uint32_t now){
    IHeartTick t; t.nowMs=now; t.stationName="WTEST"; t.digitalActive=true; t.repinArmed=true; return t;
}

int main(){
    // 1) manifest song commits in ONE tick
    { IHeartNowPlayingSM sm; IHeartTick t=base(1000);
      t.mfCls=IHeartMfCls::Song; t.mfSong="Artist - Title";
      IHeartDecision d=sm.tick(t);
      CHECK(d.committed); CHECK(d.newKind==IHNow::Song);
      CHECK(d.newDisp=="Artist - Title"); CHECK(sm.state()==IHNow::Song); }

    // 2) ad needs THREE ticks
    { IHeartNowPlayingSM sm; IHeartTick t=base(1000); t.mfCls=IHeartMfCls::Ad;
      CHECK(!sm.tick(t).committed); CHECK(!sm.tick(t).committed);
      IHeartDecision d=sm.tick(t);
      CHECK(d.committed); CHECK(d.newKind==IHNow::Ad);
      CHECK(d.newDisp=="WTEST - Commercial break"); }

    // 3) murk floors to LIVE after TWO ticks
    { IHeartNowPlayingSM sm; IHeartTick t=base(1000);
      CHECK(!sm.tick(t).committed);
      IHeartDecision d=sm.tick(t);
      CHECK(d.committed); CHECK(d.newKind==IHNow::Live); CHECK(d.newDisp=="WTEST - LIVE"); }

    // 4) ctm-confirmed live song beats a lagging ad marker (digital), commits in 1 tick
    { IHeartNowPlayingSM sm; IHeartTick t=base(1000);
      t.mfCls=IHeartMfCls::Ad;                       // manifest says ad...
      t.ctmOk=true; t.ctmStatus=200; t.ctmArtist="A"; t.ctmTitle="B"; t.ctmEndedSecsAgo=0;
      IHeartDecision d=sm.tick(t);
      CHECK(d.committed); CHECK(d.newDisp=="A - B"); }

    // 5) LIVE-floor stall fires a re-pin past LIVE_STALL_MS, once, when armed
    { IHeartNowPlayingSM sm;
      sm.tick(base(1000));                            // liveSince_=1000
      sm.tick(base(2000));                            // commit LIVE; 1s elapsed, no fire
      IHeartDecision d=sm.tick(base(40000));          // 39s elapsed -> fire
      CHECK(d.liveStallFired); CHECK(d.liveStallElapsedMs==39000); }

    // 6) not armed -> no fire even past the window
    { IHeartNowPlayingSM sm;
      sm.tick(base(1000)); sm.tick(base(2000));
      IHeartTick t=base(40000); t.repinArmed=false;
      CHECK(!sm.tick(t).liveStallFired); }

    // 7) F6 off mode (repinMode=0): never fires, even far past the window
    { IHeartNowPlayingSM sm;
      IHeartTick a=base(1000);   a.repinMode=0; sm.tick(a);
      IHeartTick b=base(2000);   b.repinMode=0; sm.tick(b);
      IHeartTick c=base(400000); c.repinMode=0;      // 399s on the floor
      CHECK(!sm.tick(c).liveStallFired); }

    // 8) F6 smart mode (repinMode=2): rides out a short break (no fire at 39s), fires
    //    only once the floor passes the longer SMART_STALL_MS (~150s).
    { IHeartNowPlayingSM sm;
      IHeartTick a=base(1000);   a.repinMode=2; sm.tick(a);
      IHeartTick b=base(2000);   b.repinMode=2; sm.tick(b);
      IHeartTick c=base(40000);  c.repinMode=2;      // 39s -> below smart threshold
      CHECK(!sm.tick(c).liveStallFired);
      IHeartTick e=base(160000); e.repinMode=2;      // 159s -> past smart threshold
      CHECK(sm.tick(e).liveStallFired); }

    if(!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
