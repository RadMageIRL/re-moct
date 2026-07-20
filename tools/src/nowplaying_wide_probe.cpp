// nowplaying_wide_probe.cpp — WIDE now-playing visibility probe (scope nowplaying-wide-probe).
//
// Brackets the full cross-product every tick and lets rates-with-n narrow it — NO
// pre-narrowing, NO single-window verdicts:
//   station  x  session  x  channel , each correlated with in-band ground truth.
//
//   station : 1469 Z100 (target) + controls 5663 / 4366 / 4257
//   session : none | anon | minted | legit   (legit from local config, never hardcoded)
//   channel : ctm | trackHistory | graphql(GetOnAirNow) | inband(ground truth) [+ catalog chained]
//
// All HTTP goes through core::IHttp (WinInet on Windows, curl on Linux) so the tool is
// both-toolchain and reuses RE-MOCT's exact transport + IHeartIdentity mint. Identity is
// written TAIL-ONLY to the capture (never full profileId/sessionId/cookie).
//
// Build (Windows, UCRT64):
//   g++ -std=c++20 -I../../include -I../../plugins/stream -I../../lib nowplaying_wide_probe.cpp \
//       ../../plugins/stream/IHeartIdentity.cpp ../../src/platform/win/HttpWinInet.cpp \
//       -o nowplaying_wide_probe.exe -lwininet
// Build (Linux): swap HttpWinInet.cpp -> ../../src/platform/linux/HttpCurl.cpp, -lcurl.
//
// Run (full drive-time window unattended, 4s poll, into np/):
//   ./nowplaying_wide_probe.exe --dur 14400 --poll 8 \
//       --stations=1469:whtz-fm,5663:,4366:,4257: --outdir=np
//   legit arm: set NP_LEGIT_BEARER=<token> and/or NP_LEGIT_COOKIE=<cookie> in the env
//   (exported from the logged-in browser session), else the legit cells log "unavailable".

#include "core/IHttp.h"
#include "IHeartIdentity.h"
#include "json.hpp"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cctype>
#include <memory>
#include <functional>
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

// ── tiny HTTP wrapper over core::IHttp (one browser-UA session, reused) ──────────
static const char* BROWSER_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36";

struct Resp { long status = 0; bool ok = false; std::string body; std::string finalUrl; };

static Resp httpGet(core::IHttpSession& sess, const std::string& url,
                    const std::vector<std::pair<std::string,std::string>>& headers) {
    core::HttpRequest req;
    req.url      = url;
    req.method   = "GET";
    req.headers  = headers;
    req.max_body = 1u*1024u*1024u;
    req.redirect = core::RedirectPolicy::FollowAll;
    core::HttpResponse r = sess.fetch(req);
    return { r.status, r.ok, std::move(r.body), std::move(r.final_url) };
}

// squash to comparable token (lowercase alnum only)
static std::string squashS(const std::string& s){ std::string o; for(unsigned char c:s) if(std::isalnum(c)) o+=(char)std::tolower(c); return o; }
// robust track equality: tolerant of "feat."/"/" artist variants and title suffixes.
// Titles must match or one contain the other; artists must overlap (substring either way)
// unless one is absent (a strong title match alone then suffices).
static bool sameTrack(const std::string& a1,const std::string& t1,const std::string& a2,const std::string& t2){
    std::string T1=squashS(t1),T2=squashS(t2); if(T1.empty()||T2.empty()) return false;
    if(!(T1==T2 || T1.find(T2)!=std::string::npos || T2.find(T1)!=std::string::npos)) return false;
    std::string A1=squashS(a1),A2=squashS(a2);
    if(A1.empty()||A2.empty()) return true;
    return A1==A2 || A1.find(A2)!=std::string::npos || A2.find(A1)!=std::string::npos;
}

// ── manifest in-band classify (ground truth) — ported from iheart_http_dump ──────
static std::string escAttr(const std::string& s, const char* key) {
    size_t p = s.find(key); if (p == std::string::npos) return {};
    p += std::strlen(key);
    while (p < s.size() && (s[p]=='\\' || s[p]=='"')) ++p;
    std::string v; while (p < s.size() && s[p]!='\\' && s[p]!='"') v += s[p++];
    return v;
}
static uint64_t segNum(const std::string& line) {
    size_t a = line.find(".aac"); if (a == std::string::npos) return 0;
    size_t s = a; while (s>0 && std::isdigit((unsigned char)line[s-1])) --s;
    return s==a ? 0 : std::strtoull(line.substr(s,a-s).c_str(), nullptr, 10);
}
struct Seg { std::string song_spot, spot_instance, title, artist; };
static const char* classify(const Seg& g) {
    if (g.song_spot == "M") return "Music";
    if (!g.spot_instance.empty() && g.spot_instance != "-1") return "Ad";
    return "Imaging";
}
static std::string firstUri(const std::string& b) {
    size_t i=0; while (i<b.size()) {
        size_t e=b.find('\n',i); if (e==std::string::npos) e=b.size();
        std::string l=b.substr(i,e-i);
        while (!l.empty() && (l.back()=='\r'||l.back()==' ')) l.pop_back();
        size_t s=l.find_first_not_of(" \t"); if (s!=std::string::npos) l=l.substr(s);
        if (!l.empty() && l[0]!='#') return l;
        i=e+1;
    } return {};
}
// newest segment of a media playlist -> class + artist/title
struct Inband { const char* cls="POLL-FAIL"; std::string artist,title,spot; uint64_t seq=0; long http=0; };
static Inband inbandOf(core::IHttpSession& sess, const std::string& mediaUrl, long http) {
    Inband r; r.http = http;
    // parse: walk EXTINF/segment pairs, keep the highest seg number
    return r; // filled by caller after fetch (kept simple below)
}

// digital master URL for a zc station (baseline params, sessionless)
static std::string digitalMaster(const std::string& id) {
    return "https://stream.revma.ihrhls.com/zc" + id + "/hls.m3u8?streamid=" + id +
        "&zip=&aw_0_1st.playerid=iHeartRadioWebPlayer&clientType=web&companionAds=false"
        "&deviceName=web-mobile&dist=iheart&host=webapp.US&listenerId=&playedFrom=157"
        "&pname=live_profile&stationid=" + id + "&terminalId=159&territory=US&us_privacy=1-N-";
}

// ── channels ────────────────────────────────────────────────────────────────────
static const char* GRAPHQL_ONAIR_HASH = "2cd74ba2499ba5299a0c362b4d7e30ed627060f7e94a39fff77f34fc4086a524";

static std::vector<std::pair<std::string,std::string>> browserHeaders(const std::string& refHost) {
    return {
        {"Accept", "application/json, text/plain, */*"},
        {"X-hostName", "webapp.US"},
        {"X-Locale", "en-US"},
        {"Origin", "https://" + refHost},
        {"Referer", "https://" + refHost + "/"},
    };
}

// session credentials for the session axis
struct Session {
    std::string label;                       // none | anon | minted | legit
    bool        available = true;
    std::string profileId, sessionId, deviceId, listenerId, bearer, cookie;
};

// ctm URL — the web-player form (no session query params; the web player sent none).
static std::string ctmUrl(const std::string& id, const Session&) {
    return "https://us.api.iheart.com/api/v3/live-meta/stream/" + id +
           "/currentTrackMeta?defaultMetadata=true";
}
// Session goes in HEADERS the ctm OPTIONS response ADVERTISES as accepted:
//   X-IHR-Profile-ID / X-IHR-Session-ID / X-User-Id / X-Session-Id / Authorization / X-Token.
// Wire the minted profileId/sessionId into every advertised form (narrow WHICH one later if
// this flips 204->200). legit adds its stream cookie / real bearer if present.
static std::vector<std::pair<std::string,std::string>> ctmHeaders(const std::string& slug, const Session& s) {
    auto h = browserHeaders(slug + ".iheart.com");
    if (!s.profileId.empty()) {
        h.push_back({"X-IHR-Profile-ID", s.profileId});
        h.push_back({"X-User-Id",        s.profileId});
    }
    if (!s.sessionId.empty()) {
        h.push_back({"X-IHR-Session-ID", s.sessionId});
        h.push_back({"X-Session-Id",     s.sessionId});
        h.push_back({"X-Token",          s.sessionId});
        h.push_back({"Authorization",    "Bearer " + s.sessionId});
    } else if (!s.bearer.empty()) {
        h.push_back({"Authorization",    "Bearer " + s.bearer});
    }
    if (!s.cookie.empty()) h.push_back({"Cookie", s.cookie});
    return h;
}

struct StationCfg { std::string id, slug, cookie; std::string mediaUrl; };

int main(int argc, char** argv) {
    int durSec = 14400, pollSec = 8;
    std::string outdir = "np";
    bool noneOnly = false;   // --none-only: session axis proven null off-peak -> collapse to `none`
    std::vector<StationCfg> stations;
    for (int i=1;i<argc;i++) {
        std::string a = argv[i];
        auto val=[&](const char* k)->std::string{ return a.substr(std::strlen(k)); };
        if (a.rfind("--dur",0)==0 && i+1<argc && a=="--dur") durSec=std::atoi(argv[++i]);
        else if (a.rfind("--poll",0)==0 && a=="--poll" && i+1<argc) pollSec=std::atoi(argv[++i]);
        else if (a=="--none-only") noneOnly=true;
        else if (a.rfind("--outdir=",0)==0) outdir=val("--outdir=");
        else if (a.rfind("--stations=",0)==0) {
            std::string s=val("--stations="); size_t p=0;
            while (p<s.size()) {
                size_t c=s.find(',',p); if (c==std::string::npos) c=s.size();
                std::string tok=s.substr(p,c-p);   // id[:slug[:cookie]]
                StationCfg sc; size_t c1=tok.find(':');
                if (c1==std::string::npos) sc.id=tok;
                else { sc.id=tok.substr(0,c1); std::string rest=tok.substr(c1+1);
                       size_t c2=rest.find(':');
                       if (c2==std::string::npos) sc.slug=rest;
                       else { sc.slug=rest.substr(0,c2); sc.cookie=rest.substr(c2+1); } }
                if (!sc.id.empty()) stations.push_back(sc);
                p=c+1;
            }
        }
    }
    if (stations.empty()) stations.push_back({"1469","whtz-fm",""});

    // ── sessions ──
    std::vector<Session> sessions;
    sessions.push_back({"none"});
    if(!noneOnly){ Session a; a.label="anon"; static const char* H="0123456789abcdef";
      std::srand((unsigned)std::time(nullptr));
      for (int i=0;i<32;i++) a.listenerId+=H[std::rand()&15]; sessions.push_back(a); }
    if(!noneOnly){ Session m; m.label="minted";
      IHeartIdentity::Identity id = IHeartIdentity::mintOrLoad(core::http(),
          [](const std::string& s){ std::fprintf(stderr,"[identity] %s\n", s.c_str()); });
      if (id.ok) { m.profileId=id.profileId; m.sessionId=id.sessionId; m.deviceId=id.deviceId;
                   std::fprintf(stderr,"[identity] minted tail=...%s\n", id.profileTail().c_str()); }
      else { m.available=false; std::fprintf(stderr,"[identity] mint unavailable -> minted cells skipped\n"); }
      sessions.push_back(m); }
    if(!noneOnly){ Session l; l.label="legit";
      const char* b=std::getenv("NP_LEGIT_BEARER"); const char* c=std::getenv("NP_LEGIT_COOKIE");
      if (b) l.bearer=b; if (c) l.cookie=c;
      bool anyStationCookie=false; for(auto&s:stations) if(!s.cookie.empty()) anyStationCookie=true;
      l.available = (b||c||anyStationCookie);   // per-station --stations cookie also enables the arm
      std::fprintf(stderr,"[legit] %s (bearer=%s cookie=%s per-station-cookie=%s)\n",
          l.available?"ACTIVE":"skipped", b?"yes":"no", c?"env":"no", anyStationCookie?"yes":"no");
      sessions.push_back(l); }

    // ── browser-UA session for all fetches ──
    core::HttpSessionConfig scfg; scfg.user_agent = BROWSER_UA; scfg.timeout_ms = 8000;
    std::unique_ptr<core::IHttpSession> sess = core::http().openSession(scfg);
    if (!sess) { std::fprintf(stderr,"openSession FAILED\n"); return 1; }

    std::string path = outdir + "/nowplaying_wide_" + std::to_string((long)std::time(nullptr)) + ".jsonl";
    FILE* log = std::fopen(path.c_str(),"wb");
    if (!log) { std::fprintf(stderr,"cannot open %s (does --outdir exist?)\n", path.c_str()); return 1; }
    std::fprintf(stderr,"capture -> %s\n", path.c_str());

    // per-cell tallies over inband=Music ticks: correct-track hits + n
    struct Cell { long musicN=0, correct=0, http200=0, httpTotal=0; };
    std::map<std::string,Cell> tally;   // key "station|session|channel"

    time_t t0=std::time(nullptr); int poll=0;
    while ((int)(std::time(nullptr)-t0) < durSec) {
        ++poll;
        json tick; tick["ts"]=(long)std::time(nullptr); tick["poll"]=poll;
        json cells = json::array();
        std::string hb;
        for (auto& stn : stations) {
            // ground truth: in-band newest segment
            if (stn.mediaUrl.empty()) {
                Resp m = httpGet(*sess, digitalMaster(stn.id), {});
                if (m.status==200) { std::string ref=firstUri(m.body);
                    if (!ref.empty()) {
                        if (ref.rfind("http",0)==0) stn.mediaUrl=ref;
                        else { std::string base=m.finalUrl.empty()?digitalMaster(stn.id):m.finalUrl;
                               size_t q=base.find('?'); if(q!=std::string::npos) base=base.substr(0,q);
                               size_t sl=base.rfind('/'); stn.mediaUrl=(sl==std::string::npos?base:base.substr(0,sl+1))+ref; } } }
            }
            const char* ibCls="POLL-FAIL"; std::string ibArtist,ibTitle; uint64_t ibSeq=0;
            if (!stn.mediaUrl.empty()) {
                Resp mm = httpGet(*sess, stn.mediaUrl, {});
                if (mm.status==200) {
                    // newest EXTINF/segment
                    size_t i=0; std::string pend; uint64_t best=0; Seg bestSeg;
                    while (i<mm.body.size()) {
                        size_t e=mm.body.find('\n',i); if (e==std::string::npos) e=mm.body.size();
                        std::string l=mm.body.substr(i,e-i); while(!l.empty()&&l.back()=='\r') l.pop_back(); i=e+1;
                        if (l.rfind("#EXTINF",0)==0){pend=l;continue;}
                        if (l.empty()||l[0]=='#') continue;
                        uint64_t n=segNum(l); if(!n) {pend.clear();continue;}
                        if (n>best){best=n; bestSeg.song_spot=escAttr(pend,"song_spot=");
                            bestSeg.spot_instance=escAttr(pend,"spotInstanceId=");
                            bestSeg.artist=escAttr(pend,"artist="); bestSeg.title=escAttr(pend,"title=");}
                        pend.clear();
                    }
                    if (best){ibCls=classify(bestSeg); ibArtist=bestSeg.artist; ibTitle=bestSeg.title; ibSeq=best;}
                    else ibCls="EMPTY";
                } else { stn.mediaUrl.clear(); }
            }
            bool music = std::string(ibCls)=="Music";
            auto matches=[&](const std::string&a,const std::string&t){ return music && sameTrack(ibArtist,ibTitle,a,t); };

            json sc; sc["station"]=stn.id; sc["slug"]=stn.slug;
            sc["inband"]=ibCls; sc["inbandArtist"]=ibArtist; sc["inbandTitle"]=ibTitle; sc["inbandSeq"]=(long long)ibSeq;

            // trackHistory (session-independent public feed) — co-timing
            { Resp th=httpGet(*sess,"https://us.api.iheart.com/api/v3/live-meta/stream/"+stn.id+"/trackHistory",
                              browserHeaders(stn.slug.empty()?"www":stn.slug+".iheart.com"));
              std::string thArt,thTit; bool thok=false;
              if (th.status==200){ try{ json j=json::parse(th.body);
                  if (j.contains("data")&&j["data"].is_array()&&!j["data"].empty()){
                      const json& d0=j["data"][0]; thArt=d0.value("artist",std::string()); thTit=d0.value("title",std::string()); thok=!thArt.empty();}}catch(...){}}
              bool thm = matches(thArt,thTit);
              sc["trackHistory"]={{"status",th.status},{"artist",thArt},{"title",thTit},{"matchesInband",thm}};
              if (music){auto&c=tally[stn.id+"|public|trackHistory"];++c.musicN;if(thm)++c.correct;} }

            // graphql GetOnAirNow (public; slug-keyed) — is it phase-locked?
            if (!stn.slug.empty()){
              std::string gu="https://webapi.radioedit.iheart.com/graphql?operationName=GetOnAirNow"
                  "&variables=%7B%22slug%22%3A%22"+stn.slug+"%22%7D"
                  "&extensions=%7B%22persistedQuery%22%3A%7B%22version%22%3A1%2C%22sha256Hash%22%3A%22"+std::string(GRAPHQL_ONAIR_HASH)+"%22%7D%7D";
              Resp g=httpGet(*sess,gu,browserHeaders(stn.slug+".iheart.com"));
              // one-time full-body dump per slug so GetOnAirNow's shape is inspectable offline
              static std::set<std::string> dumped;
              if (g.status==200 && !g.body.empty() && dumped.insert(stn.slug).second){
                  std::string dp=outdir+"/graphql_GetOnAirNow_"+stn.slug+"_sample.json";
                  if(FILE* df=std::fopen(dp.c_str(),"wb")){std::fwrite(g.body.data(),1,g.body.size(),df);std::fclose(df);
                      std::fprintf(stderr,"[graphql] dumped %s (%zu bytes) for offline shape analysis\n",dp.c_str(),g.body.size());}
              }
              std::string gArt,gTit; bool gok=false;
              if (g.status==200 && !g.body.empty()){ try{ json j=json::parse(g.body);
                  // shape unknown -> best-effort: find artist/title-ish fields anywhere in the tree
                  std::function<void(const json&)> walk=[&](const json& x){
                      if (x.is_object()){ for(const char* ak:{"artist","artistName","trackArtist"}) if(gArt.empty()&&x.contains(ak)&&x[ak].is_string())gArt=x[ak];
                                          for(const char* tk:{"title","trackTitle","song","name"}) if(gTit.empty()&&x.contains(tk)&&x[tk].is_string())gTit=x[tk];
                                          for(auto&kv:x.items())walk(kv.value()); }
                      else if (x.is_array()) for(auto&v:x)walk(v); };
                  walk(j); gok=!gArt.empty()||!gTit.empty();
              }catch(...){}}
              bool gm = matches(gArt,gTit);
              sc["graphqlOnAir"]={{"status",g.status},{"bodyLen",(long)g.body.size()},{"artist",gArt},{"title",gTit},{"matchesInband",gm}};
              if (music){auto&c=tally[stn.id+"|public|graphqlOnAir"];++c.musicN;if(gm)++c.correct;}
            }

            // ctm x session — THE untested matrix
            json ctmArr=json::array();
            for (auto& S : sessions) {
                json cell; cell["session"]=S.label;
                if (!S.available){ cell["status"]="unavailable"; ctmArr.push_back(cell); continue; }
                Session Suse=S; if (S.label=="legit" && !stn.cookie.empty()) Suse.cookie=stn.cookie;  // per-station cookie
                Resp r=httpGet(*sess, ctmUrl(stn.id,Suse), ctmHeaders(stn.slug.empty()?"www":stn.slug,Suse));
                std::string art,tit; bool ok=false;
                if (r.status==200 && !r.body.empty()){ try{ json j=json::parse(r.body);
                    art=j.value("artist",std::string()); tit=j.value("title",std::string()); ok=!(art.empty()&&tit.empty()); }catch(...){}}
                bool match = ok && matches(art,tit);
                cell["status"]=r.status; cell["artist"]=art; cell["title"]=tit; cell["matchesInband"]=match;
                ctmArr.push_back(cell);
                auto& c=tally[stn.id+"|"+S.label+"|ctm"]; ++c.httpTotal; if(r.status==200)++c.http200;
                if (music){++c.musicN; if(match)++c.correct;}
            }
            sc["ctm"]=ctmArr;
            cells.push_back(sc);
            hb += " " + stn.id + "=" + ibCls;
        }
        tick["cells"]=cells;
        std::string line=tick.dump(); std::fwrite(line.data(),1,line.size(),log); std::fputc('\n',log); std::fflush(log);
        if (poll%8==1) std::fprintf(stderr,"poll %d,%s\n", poll, hb.c_str());
        for (int s=0;s<pollSec && (int)(std::time(nullptr)-t0)<durSec;++s) {
            struct timespec ts{1,0}; (void)ts;
            #ifdef _WIN32
            Sleep(1000);
            #else
            struct timespec req{1,0}; nanosleep(&req,nullptr);
            #endif
        }
    }
    std::fclose(log);

    // ── summary: per-cell correct-track-rate over inband=Music ticks, with n ──
    std::fprintf(stderr,"\n=== SUMMARY: correct-airing-track rate over inband=Music ticks ===\n");
    for (auto& [k,c] : tally) {
        if (c.musicN==0 && c.httpTotal==0) continue;
        double rate = c.musicN ? 100.0*c.correct/c.musicN : 0.0;
        std::fprintf(stderr,"  %-34s correct=%3ld/%-4ld (%.0f%% of music)  ctm200=%ld/%ld\n",
            k.c_str(), c.correct, c.musicN, rate, c.http200, c.httpTotal);
    }
    std::fprintf(stderr,"\nRead the matrix: does legit beat none/anon on 1469|*|ctm; is any channel phase-locked (high %%); is 1469 ctm dead across ALL sessions.\n");
    return 0;
}
