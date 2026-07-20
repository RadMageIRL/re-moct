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

    // 5) LIVE-floor stall reports past the uniform REPIN_FLOOR_MS (150s), once, when
    //    armed - and NOT at 39s (the old 35s 'on' floor is retired; f6-repin-finalize).
    { IHeartNowPlayingSM sm;
      sm.tick(base(1000));                            // liveSince_=1000
      sm.tick(base(2000));                            // commit LIVE; 1s elapsed, no fire
      CHECK(!sm.tick(base(40000)).liveStallFired);    // 39s -> below the uniform floor
      IHeartDecision d=sm.tick(base(160000));         // 159s elapsed -> report
      CHECK(d.liveStallFired); CHECK(d.liveStallElapsedMs==159000); }

    // 6) not armed -> no fire even past the window
    { IHeartNowPlayingSM sm;
      sm.tick(base(1000)); sm.tick(base(2000));
      IHeartTick t=base(160000); t.repinArmed=false;
      CHECK(!sm.tick(t).liveStallFired); }

    // 7) F6 off mode (repinMode=0): never fires, even far past the window
    { IHeartNowPlayingSM sm;
      IHeartTick a=base(1000);   a.repinMode=0; sm.tick(a);
      IHeartTick b=base(2000);   b.repinMode=0; sm.tick(b);
      IHeartTick c=base(400000); c.repinMode=0;      // 399s on the floor
      CHECK(!sm.tick(c).liveStallFired); }

    // 8) every ACTIVE mode shares the uniform floor: ad-escape (1) and timed (3) also
    //    ride out 39s and report at 159s - the SM no longer picks a floor by mode
    //    (evidence-gating of the actual fire is the caller's job; f6-repin-finalize).
    { for (int m : {1, 2, 3}) {
        IHeartNowPlayingSM sm;
        IHeartTick a=base(1000);   a.repinMode=m; sm.tick(a);
        IHeartTick b=base(2000);   b.repinMode=m; sm.tick(b);
        IHeartTick c=base(40000);  c.repinMode=m;    // 39s -> below the uniform floor
        CHECK(!sm.tick(c).liveStallFired);
        IHeartTick e=base(160000); e.repinMode=m;    // 159s -> past the floor
        CHECK(sm.tick(e).liveStallFired); } }

    if(!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
