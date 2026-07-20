// gate_replay.cpp - shadow gate replay harness (scope shadow-gate-replay).
//
// OFFLINE, READ-ONLY. Replays RE-MOCT deep logs through (a) a MIRROR of the current
// SM target gate and (b) a proposed two-condition gate, and reports recovered-vs-
// regressed song-minutes, cause-tagged. Proves the fix on recorded air before any
// change to RE-MOCT. Touches nothing in plugins/stream/* or the running app.
//
// Build:  g++ -std=c++20 -I../../lib gate_replay.cpp -o gate_replay.exe
// Run:    ./gate_replay.exe ../../logs/remoct-deep-analysis-*.log
//
// Current gate mirrored from IHeartNowPlayingSM::tick() target ladder (lines 29-40):
//   mfSong -> Song ; else ctmSong -> Song ; else mfCls==Ad -> Ad ;
//   else thCurrent(thEnded<=60, or <=0 if state==Ad) -> Song ; else -> Live.
// stState (committed, post-debounce) in the log is the faithful "what was shown".
#include "json.hpp"
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cctype>
#include <algorithm>
using json = nlohmann::json;

static long parseTs(const std::string& ts){ // "YYYY-MM-DD HH:MM:SS...."
    std::tm tm{}; if (std::sscanf(ts.c_str(),"%d-%d-%d %d:%d:%d",&tm.tm_year,&tm.tm_mon,&tm.tm_mday,&tm.tm_hour,&tm.tm_min,&tm.tm_sec)!=6) return 0;
    tm.tm_year-=1900; tm.tm_mon-=1;
#ifdef _WIN32
    return (long)_mkgmtime(&tm);
#else
    return (long)timegm(&tm);
#endif
}
static int localHour(long ep){ return (int)(((ep-5*3600)/3600)%24); } // CDT
static std::string sq(const std::string& s){ std::string o; for(unsigned char c:s) if(std::isalnum(c)) o+=(char)std::tolower(c); return o; }

struct Rec {
    long ep=0; std::string mfCls, mfSong, mfArtist, mfTitle, th, tgtKind, tgtDisp, stState, stDisp;
    long thEnded=-1; bool digitalActive=false, ctmOk=false; long ctmStatus=0; long long ctmEnded=-1;
    std::string ctmArtist, ctmTitle;
};

// mirror the SM target ladder (pure function of tick inputs + prior committed state)
static std::string mirrorTgt(const Rec& r){
    if(!r.mfSong.empty()) return "Song";
    std::string ctmSong;
    if(r.digitalActive && r.ctmOk && r.ctmStatus==200 && !r.ctmTitle.empty() && r.ctmEnded<=0)
        ctmSong = r.ctmArtist.empty()? r.ctmTitle : (r.ctmArtist+" - "+r.ctmTitle);
    if(!ctmSong.empty()) return "Song";
    if(r.mfCls=="Ad") return "Ad";
    long thMax = (r.stState=="Ad")?0:60;
    bool thCurrent = !r.th.empty() && r.thEnded<=thMax;
    if(thCurrent) return "Song";
    return "Live";
}

int main(int argc,char**argv){
    std::vector<std::string> files;
    for(int i=1;i<argc;i++) files.push_back(argv[i]);
    if(files.empty()){ std::fprintf(stderr,"usage: gate_replay <deep-log...>\n"); return 1; }

    // aggregate
    double AG_recov=0, AG_regr=0, AG_agree=0, AG_other=0; long AG_faith_ok=0, AG_faith_n=0;
    double AG_t2live=0, AG_t2stale=0, AG_tcommit=0, AG_maxRun=0; int AG_sustained=0;
    struct Ep{ std::string ts,song; double liveMin; };
    std::vector<Ep> topRecovered, sampTL;

    for(const auto& f : files){
        std::FILE* fp=std::fopen(f.c_str(),"rb"); if(!fp){ std::fprintf(stderr,"skip %s\n",f.c_str()); continue; }
        std::string all; { char buf[65536]; size_t n; while((n=std::fread(buf,1,sizeof buf,fp))>0) all.append(buf,n);} std::fclose(fp);
        std::vector<Rec> recs;
        size_t i=0; while(i<all.size()){ size_t e=all.find('\n',i); if(e==std::string::npos)e=all.size();
            std::string line=all.substr(i,e-i); i=e+1; if(line.empty())continue;
            json o; try{o=json::parse(line);}catch(...){continue;} if(o.value("_meta",false))continue;
            Rec r; r.ep=parseTs(o.value("ts",std::string()));
            r.mfCls=o.value("mfCls",std::string()); r.mfSong=o.value("mfSong",std::string());
            r.mfArtist=o.value("mfArtist",std::string()); r.mfTitle=o.value("mfTitle",std::string());
            r.th=o.value("th",std::string()); r.thEnded=o.value("thEnded",-1L);
            r.tgtKind=o.value("tgtKind",std::string()); r.stState=o.value("stState",std::string());
            r.tgtDisp=o.value("tgtDisp",std::string()); r.stDisp=o.value("stDisp",std::string());
            r.digitalActive=o.value("digitalActive",false); r.ctmOk=o.value("ctmOk",false);
            r.ctmStatus=o.value("ctmStatus",0L); r.ctmEnded=o.value("ctmEndedSecsAgo",-1LL);
            r.ctmArtist=o.value("ctmArtist",std::string()); r.ctmTitle=o.value("ctmTitle",std::string());
            if(r.ep) recs.push_back(r);
        }
        if(recs.empty()) continue;

        // Step 1 faithfulness: mirrored tgtKind vs recorded tgtKind
        long faith_ok=0, faith_n=0;
        for(auto&r:recs){ if(r.tgtKind.empty())continue; faith_n++; if(mirrorTgt(r)==r.tgtKind)faith_ok++; }

        // Step 2/3: proposed gate + divergence, wall-clock weighted
        double recov=0,regr=0,agree=0,other=0;
        double t2live=0,t2stale=0,tcommit=0;   // target -> committed delta (commit-path loss)
        double tcRun=0, tcMaxRun=0; int tcSustained=0;   // contiguous tgt=Song-but-committed-wrong (sustained vs onset-lag)
        std::string lastGood;   // proposed cache: valid only while in-band Song
        int driveTicks=0;
        for(size_t k=0;k<recs.size();++k){
            Rec& r=recs[k];
            double w = (k+1<recs.size())? std::min((double)(recs[k+1].ep-r.ep),60.0) : 5.0;
            if(w<0)w=0;
            bool drive = (localHour(r.ep)>=6 && localHour(r.ep)<10); if(drive)driveTicks++;
            // proposed gate
            std::string prop;
            if(r.mfCls=="Song"){ prop="Song"; if(!r.mfSong.empty()) lastGood=r.mfSong; }
            else if(r.mfCls=="Ad"){ prop="Live"; lastGood.clear(); }
            else { prop = (r.stState.empty()? "Live" : r.stState); } // None -> defer to current
            // current committed display
            std::string cur = r.stState.empty()? "Live" : r.stState;
            // classify (weight by minutes). AGREE first: if proposed == current, proposed
            // introduced NOTHING (incl. deferring to current on mfCls==None) -> never a regression.
            double wm=w/60.0;
            if(prop==cur) agree+=wm;
            else if(cur=="Live" && prop=="Song" && r.mfCls=="Song"){ recov+=wm;   // the win
                if(topRecovered.size()<200) topRecovered.push_back({std::to_string(r.ep),(r.mfArtist+" - "+r.mfTitle),wm}); }
            else if(prop=="Song" && r.mfCls!="Song") regr+=wm;   // proposed DIVERGED to Song w/o music = cache-stickiness fail (must be ~0)
            else other+=wm;

            // target -> committed delta: SM TARGETED Song, but what got committed/published?
            // (NOTE: stState is the pre-tick snapshot = prior commit, so a 1-record onset lag
            //  is expected debounce; a SUSTAINED tgt=Song/committed!=Song is the commit-path bug.)
            if(r.tgtKind=="Song"){
                if(r.stState=="Song" && sq(r.tgtDisp)==sq(r.stDisp)) tcommit+=wm;   // committed == targeted
                else if(r.stState=="Live"){ t2live+=wm; if(sampTL.size()<300) sampTL.push_back({std::to_string(r.ep),r.tgtDisp,wm}); }
                else if(r.stState=="Song") t2stale+=wm;   // committed a DIFFERENT (stale) song than targeted
            }
            // sustained vs onset-lag: contiguous run where target=Song but committed != that song
            bool tcMiss = (r.tgtKind=="Song" && !(r.stState=="Song" && sq(r.tgtDisp)==sq(r.stDisp)));
            if(tcMiss){ tcRun+=wm; if(tcRun>tcMaxRun)tcMaxRun=tcRun; }
            else { if(tcRun>=1.0)tcSustained++; tcRun=0; }
        }
        if(tcRun>=1.0)tcSustained++;
        double tot=recov+regr+agree+other; if(tot<=0)tot=1;
        std::printf("=== %s ===\n", f.c_str());
        std::printf("  records=%zu drive-time-ticks=%d  faithfulness(mirror tgtKind vs recorded)=%.1f%% (%ld/%ld)\n",
            recs.size(), driveTicks, faith_n?100.0*faith_ok/faith_n:0, faith_ok, faith_n);
        std::printf("  gate A/B song-minutes: RECOVERED=%.2f  REGRESSED=%.2f  AGREE=%.1f  OTHER=%.2f  (AGREE %.1f%%)\n",
            recov,regr,agree,other,100.0*agree/tot);
        std::printf("  target->commit: of SM-targeted-Song min: committed==target=%.1f  committed-LIVE=%.2f  committed-STALE-song=%.2f\n",
            tcommit,t2live,t2stale);
        std::printf("  commit-lag runs: longest contiguous tgt-Song-but-committed-wrong=%.2f min  sustained(>=1min) runs=%d %s\n",
            tcMaxRun, tcSustained, tcSustained? "<-- REAL STUCK" : "(all onset-lag slivers)");
        AG_recov+=recov; AG_regr+=regr; AG_agree+=agree; AG_other+=other; AG_faith_ok+=faith_ok; AG_faith_n+=faith_n;
        AG_t2live+=t2live; AG_t2stale+=t2stale; AG_tcommit+=tcommit;
        AG_sustained+=tcSustained; if(tcMaxRun>AG_maxRun)AG_maxRun=tcMaxRun;
    }

    std::printf("\n==================== AGGREGATE (all logs) ====================\n");
    std::printf("faithfulness=%.1f%% (%ld/%ld)  [Step1 gate: >=~98%% to trust the harness]\n",
        AG_faith_n?100.0*AG_faith_ok/AG_faith_n:0, AG_faith_ok, AG_faith_n);
    std::printf("song-minutes RECOVERED=%.2f   REGRESSED=%.2f   AGREE=%.1f   OTHER=%.2f\n",
        AG_recov,AG_regr,AG_agree,AG_other);
    // top recovered episodes (merge consecutive same-song)
    std::sort(topRecovered.begin(),topRecovered.end(),[](const Ep&a,const Ep&b){return a.liveMin>b.liveMin;});
    std::printf("sample RECOVERED moments (in-band song shown instead of LIVE):\n");
    for(size_t i=0;i<topRecovered.size() && i<8;i++)
        std::printf("   ep=%s  '%s'  +%.2f min\n", topRecovered[i].ts.c_str(), topRecovered[i].song.c_str(), topRecovered[i].liveMin);
    std::printf("\n---- TARGET -> COMMITTED delta (where the loss actually lives) ----\n");
    std::printf("of SM-targeted-Song song-minutes: committed==target=%.1f  committed-LIVE=%.2f  committed-STALE-song=%.2f\n",
        AG_tcommit,AG_t2live,AG_t2stale);
    std::sort(sampTL.begin(),sampTL.end(),[](const Ep&a,const Ep&b){return a.liveMin>b.liveMin;});
    std::printf("sample tgt=Song-but-committed-LIVE moments (commit-path loss, tick ts):\n");
    for(size_t i=0;i<sampTL.size() && i<8;i++)
        std::printf("   ep=%s  targeted:'%s'  +%.2f min\n", sampTL[i].ts.c_str(), sampTL[i].song.c_str(), sampTL[i].liveMin);
    if(AG_t2live+AG_t2stale < 0.5*std::max(AG_tcommit,1.0))
        std::printf("  -> commit tracks target tightly (delta is onset-lag only): the loss is NOT in the commit path either.\n");
    else
        std::printf("  -> SUSTAINED tgt-Song-but-committed-not: the commit path (debounce/publish/repin flush) IS eating updates. LOCATED.\n");

    std::printf("\nVERDICT vs acceptance (recovered a meaningful BLOCK + regressed ZERO -> promote):\n");
    std::printf("  REGRESSED=%.2f min %s\n", AG_regr, AG_regr<0.01?"(zero - no cache-stickiness fail)":"(NONZERO - cache over-held, hard fail)");
    std::printf("  RECOVERED=%.2f min, but longest sustained tgt-Song-but-committed-wrong run=%.2f min, sustained(>=1min) runs=%d.\n",
        AG_recov, AG_maxRun, AG_sustained);
    if(AG_sustained==0)
        std::printf("  => The recovered/delta minutes are ALL per-boundary onset-lag slivers (~one poll-record each),\n"
                    "     NOT a sustained block. Neither the staleness gate NOR the commit path shows a stuck.\n"
                    "     The '15-min stuck on LIVE' symptom is NOT REPRODUCED in this corpus. Two outcomes:\n"
                    "     (1) the two-condition gate is a SAFE minor onset-snappiness win (zero regression), not a bug fix;\n"
                    "     (2) to fix a real 15-min stuck, we must first CATCH it live -- no logged air here contains it.\n");
    else
        std::printf("  => %d sustained stuck run(s), longest %.2f min: a real commit-path stuck exists -> inspect those ts.\n",
                    AG_sustained, AG_maxRun);
    return 0;
}
