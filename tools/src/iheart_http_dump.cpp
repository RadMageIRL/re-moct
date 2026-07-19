// iheart_http_dump.cpp — standalone iHeart digital-manifest + ad-tier probe.
//
// Reproduces RE-MOCT's digital (web-player) handshake for a station, polls the
// resolved short-lived variant playlist, dumps every manifest revision verbatim,
// and — the point of this tool — MEASURES the ad load per handshake variant from
// the manifest text alone (no audio downloaded). The measurable question is not
// "did it 200" but "did the AD TIER change": the web player gets clean, short,
// aligned breaks; RE-MOCT's anonymous-new-every-time session can get a long
// house-bin backfill. Answer = ad-block duration, baseline vs fullhandshake.
//
// The ad signal is IN-BAND, per segment: each #EXTINF carries
//   url="song_spot=\"M\" spotInstanceId=\"-1\" length=\"...\" ... "
// song_spot="M" + spotInstanceId="-1" == music; any other spot type (and a real
// spotInstanceId) == an ad/spot. Segments are keyed by their .aac sequence
// number, so polling the sliding window and de-duplicating by that number
// reconstructs the full played timeline and its contiguous ad blocks.
//
// Build (MSYS2 UCRT64) - -I../../lib for the vendored json.hpp (capture mode):
//   g++ -std=c++20 -I../../lib iheart_http_dump.cpp -o iheart_http_dump.exe -lwininet
// Build (Linux, libcurl):
//   g++ -std=c++20 -DUSE_CURL -I../../lib iheart_http_dump.cpp -o iheart_http_dump -lcurl
//
// Run:  iheart_http_dump [duration] [pollSec] [variant] [logPath] [--profile=ID]
//                        [--listener=ID] [--no-triton] [--station=zc4366]
//   variant = baseline | clone | emptyid | synth | fullhandshake   (default baseline)
//   defaults: 180s, poll 3s, log ihearthttpdump_<variant>_<stamp>.log
//
//   baseline       : current RE-MOCT - fresh random listenerId, no profile   (CONTROL)
//   clone          : exact web-player string - captured profileId + skey, empty lid
//   emptyid        : RE-MOCT params but listenerId empty, still no profile
//   synth          : empty lid + a SYNTHETIC stable profileId/skey (shippable test)
//   fullhandshake  : the COMPLETE web-player handshake - identity params (profileId
//                    + aw_0_1st.skey + listenerId) + follow the 302 + keep the
//                    rj-listener-cookie + fire the Triton lt?sid=<sid>&vid=<profileId>
//                    listen-tracking call + poll. --profile/--listener override the
//                    identity (paste a freshly captured pair; do NOT ship a personal
//                    one). --no-triton skips the Triton call (the "does the lt call
//                    matter" A/B). Without --profile it uses the captured control id.
//
// Dual-station trackHistory capture mode (iheart-adboundary-capture):
//   --capture [durationSec] [pollSec]  [--stations=1469,4366] [--outdir=DIR] [--rollmb=40]
//     Polls the trackHistory JSON feed for TWO+ stations on a fixed timer and logs the
//     FULL data[] each tick as JSON-lines, plus what the PLAYER'S pollNowPlaying WOULD
//     select (the four guards ported verbatim: future-filter, newest-aired scan,
//     monotonic accepted_max, staleness). Per record: ts/epoch, http, full data[] with
//     unknown fields preserved, computed state (SONG/EMPTY/HELD/FUTURE-REJECTED/LIVE/
//     POLL-FAIL), selIdx, would-string, ended, futureRej, acceptedMax. A SONG->gap->SONG
//     boundary emits a TRANSITION record with the gap duration (the headline number).
//     Logs roll at --rollmb (default 40 MB); stderr prints a heartbeat every ~minute.
//     A failed/empty poll logs POLL-FAIL and continues - never crashes the run.
//     PHASE 1 (now, off-hours): run long + unattended = the low-ad-density CONTROL of
//       clean post-ad recoveries on both stations. Confirm capture-and-forget (rolls,
//       survives blips). e.g.  iheart_http_dump --capture 21600 5 --outdir=cap
//     PHASE 2 (later, drive time): the SAME tool run in a high-ad-density window where
//       Z100's gap manifests - the failing transitions to diff against Phase 1. Dos runs
//       this live. Stations: Z100=zc1469, Breeze=zc4366 (control), Hits=zc4257 (optional).
//
// Ad-tier analysis modes (no network):
//   --analyze=<log>        re-score a saved log with the current classifier and
//                          print the ad-tier summary (music / imaging / PAID ads,
//                          break durations, BACKFILL flags, verdict).
//   --diff=<logA>,<logB>   Task-2 divergence: do two concurrent pulls serve the
//                          SAME content? Segment-number overlap (Jaccard) +
//                          each side's tier. Use Windows paths (no MSYS comma xlate).
// A live run also emits a ">>> BACKFILL-MARKER" line the moment a trailing paid-ad
// run crosses 5 min - the PROXY repin/desync marker (the standalone tool cannot
// fire a real repin; that is the live player's job). Ad = spotInstanceId != "-1"
// (a real paid spot); song_spot="T" is a MIXED jingle+ad bucket - never the ad
// signal on its own. A talk/news station (or a music station in show/talk daypart)
// shows generic branding as imaging with no song titles - flag the station type.
//
// For a clean A/B, run baseline and fullhandshake CONCURRENTLY (same broadcast
// clock, so the same real break is seen by both). Catch 2-3 breaks per variant
// before trusting a difference - ad fill is partly luck-of-the-draw. Caveat: if
// iHeart keys the ad tier on client IP rather than the session id, same-IP runs
// can't separate the tiers - the browser captures (normal vs incognito, same IP,
// different tier) argue it is id-based, but note it.
//
// NOTE (Task A, the identity mint): the web player mints profileId/listenerId per
// fresh anonymous session via a bootstrap call that fires BEFORE the stream
// request. That call is not reproduced here - it needs a fresh incognito capture
// of the FULL request list from first page load to identify. Until then,
// fullhandshake takes the id as an argument (--profile/--listener) or uses the
// captured control id. See the report for exactly what to re-capture.

#include <windows.h>
#include <wininet.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include "json.hpp"          // nlohmann single-header (vendored in lib/); trackHistory capture mode
using json = nlohmann::json;

static const char* USER_AGENT =
    "RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)";

// Captured web-player control identity (The Breeze / zc4366). Session-bound; used
// only as the probe's default when no fresh pair is pasted. Never shipped.
static const char* CAPTURED_PROFILE = "12663682685";
static const char* TRITON_SID       = "20730";        // constant for this station

static std::string SYNTH_PROFILE = "12700000420";     // fake but plausible, stable

// A contiguous PAID-ad run this long (seconds) is flagged as a BACKFILL block -
// the "15-20 min house-bin" bad tier, materially longer than the ~2-3 min aligned
// breaks. The standalone tool cannot fire a real repin (that is the live player's
// job), so a crossing of this threshold is the PROXY repin/desync marker: it is
// the stream condition that would trigger one. On-peak, correlate these markers
// with when repins are actually heard/seen.
static const double BACKFILL_SEC = 300.0;             // 5 min

// Runtime identity for fullhandshake (set from args).
static std::string g_profile;      // profileId + aw_0_1st.skey
static std::string g_listener;     // listenerId
static bool        g_no_triton = false;

static std::string nowStamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    char b[64];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

static std::string buildDigitalUrl(const std::string& base, const std::string& variant) {
    std::string id;
    size_t p = base.find("/zc");
    if (p != std::string::npos) {
        p += 3;
        while (p < base.size() && std::isdigit((unsigned char)base[p])) id += base[p++];
    }
    if (id.empty()) return base;

    static const char* H = "0123456789abcdef";
    std::string lid; lid.reserve(32);
    for (int i = 0; i < 32; ++i) lid += H[std::rand() & 15];

    std::string skey, profile, listener;
    if (variant == "clone") {
        skey = CAPTURED_PROFILE; profile = CAPTURED_PROFILE; listener = "";
    } else if (variant == "emptyid") {
        skey = "";            profile = "";            listener = "";
    } else if (variant == "synth") {
        skey = SYNTH_PROFILE; profile = SYNTH_PROFILE; listener = "";
    } else if (variant == "fullhandshake") {
        // Full web-player identity. g_profile/g_listener come from args, else the
        // captured control id (with the captured-shape empty listener default).
        profile  = g_profile.empty()  ? std::string(CAPTURED_PROFILE) : g_profile;
        skey     = profile;
        listener = g_listener;                 // may be empty (matches the capture)
    } else { // baseline
        skey = "";            profile = "";            listener = lid;
    }

    std::string u = base + "?streamid=" + id +
        "&zip=&aw_0_1st.playerid=iHeartRadioWebPlayer";
    if (!skey.empty()) u += "&aw_0_1st.skey=" + skey;
    u += "&clientType=web&companionAds=false&deviceName=web-mobile&dist=iheart"
         "&host=webapp.US&listenerId=" + listener +
         "&playedFrom=157&pname=live_profile";
    if (!profile.empty()) u += "&profileId=" + profile;
    u += "&stationid=" + id + "&terminalId=159&territory=US&us_privacy=1-N-";
    return u;
}

static std::string firstUri(const std::string& body) {
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(i, e - i);
        while (!line.empty() && (line.back()=='\r'||line.back()==' '||line.back()=='\t')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s != std::string::npos) line = line.substr(s);
        if (!line.empty() && line[0] != '#') return line;
        i = e + 1;
    }
    return {};
}

static std::string resolveUrl(const std::string& base, const std::string& ref) {
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
    char out[2048]; DWORD len = sizeof(out);
    if (InternetCombineUrlA(base.c_str(), ref.c_str(), out, &len, ICU_NO_ENCODE))
        return std::string(out, len);
    return ref;
}

static bool httpGet(HINTERNET hInet, const std::string& url,
                    std::string* body, std::string* finalUrl, DWORD* statusOut) {
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION;
    HINTERNET h = InternetOpenUrlA(hInet, url.c_str(), nullptr, 0, flags, 0);
    if (!h) { std::printf("  open FAILED err=%lu\n", GetLastError()); return false; }

    if (finalUrl) {
        char fbuf[2048]; DWORD flen = sizeof(fbuf);
        if (InternetQueryOptionA(h, INTERNET_OPTION_URL, fbuf, &flen)) finalUrl->assign(fbuf, flen);
        else *finalUrl = url;
    }
    DWORD status = 0, slen = sizeof(status), sidx = 0;
    HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &slen, &sidx);
    if (statusOut) *statusOut = status;

    std::vector<char> buf; char chunk[8192]; DWORD got = 0;
    for (;;) {
        if (!InternetReadFile(h, chunk, sizeof(chunk), &got)) { InternetCloseHandle(h); return false; }
        if (got == 0) break;
        buf.insert(buf.end(), chunk, chunk + got);
        if (buf.size() > 4u*1024u*1024u) break;
    }
    InternetCloseHandle(h);
    if (body) body->assign(buf.data(), buf.size());
    return true;
}

// ── ad-tier classification ────────────────────────────────────────────────────
// One played segment, keyed by its .aac sequence number (dedup across polls).
struct Seg {
    double      dur = 0;
    std::string song_spot;      // "M" = music; anything else = a spot/ad
    std::string spot_instance;  // "-1" for music; a real id for a spot
    std::string title;
    std::string artist;
};

// Extract a backslash-escaped attribute value: key is e.g. song_spot=  and the
// on-wire form is  song_spot=\"M\"  (bytes: ... = \ " M \ " ...).
static std::string escAttr(const std::string& s, const char* key) {
    size_t p = s.find(key);
    if (p == std::string::npos) return {};
    p += std::strlen(key);
    while (p < s.size() && (s[p] == '\\' || s[p] == '"')) ++p;   // skip \ and "
    std::string v;
    while (p < s.size() && s[p] != '\\' && s[p] != '"') v += s[p++];
    return v;
}

// Segment number from a .../<n>.aac?... URL (0 = not a segment line).
static uint64_t segNumFromUrl(const std::string& line) {
    size_t a = line.find(".aac");
    if (a == std::string::npos) return 0;
    size_t s = a;
    while (s > 0 && std::isdigit((unsigned char)line[s-1])) --s;
    if (s == a) return 0;
    return std::strtoull(line.substr(s, a - s).c_str(), nullptr, 10);
}

// Three buckets, because song_spot="T" is a MIXED signal - it covers both station
// jingles/imaging (spotInstanceId="-1", ~5-9s, normal programming) AND real paid
// commercials (a real spotInstanceId, 30-60s). The "trash ad tier" question is
// about PAID ads, so the ad signal is spotInstanceId != "-1", not song_spot.
enum class Kind { Music, Imaging, Ad };
static Kind classify(const Seg& sg) {
    if (sg.song_spot == "M") return Kind::Music;
    if (!sg.spot_instance.empty() && sg.spot_instance != "-1") return Kind::Ad;
    return Kind::Imaging;   // song_spot != M but no real spot id = jingle/promo
}

// Fold one manifest revision into the timeline (new segment numbers only).
static void ingest(const std::string& m, std::map<uint64_t, Seg>& timeline) {
    size_t i = 0;
    std::string pendingExtinf;
    while (i < m.size()) {
        size_t e = m.find('\n', i);
        if (e == std::string::npos) e = m.size();
        std::string line = m.substr(i, e - i);
        while (!line.empty() && (line.back()=='\r')) line.pop_back();
        i = e + 1;
        if (line.rfind("#EXTINF", 0) == 0) { pendingExtinf = line; continue; }
        if (line.empty() || line[0] == '#') continue;
        uint64_t n = segNumFromUrl(line);
        if (n == 0 || timeline.count(n)) { pendingExtinf.clear(); continue; }
        Seg sg;
        size_t c = pendingExtinf.find(':');
        if (c != std::string::npos) sg.dur = std::atof(pendingExtinf.c_str() + c + 1);
        sg.song_spot     = escAttr(pendingExtinf, "song_spot=");
        sg.spot_instance = escAttr(pendingExtinf, "spotInstanceId=");
        sg.title         = escAttr(pendingExtinf, "title=");
        sg.artist        = escAttr(pendingExtinf, "artist=");
        timeline[n] = sg;
        pendingExtinf.clear();
    }
}

// The trailing contiguous PAID-ad run (walking back from the newest segment).
// Used live to raise a backfill/desync marker the moment the run gets long.
struct TrailRun { double dur = 0; uint64_t first = 0, last = 0; int segs = 0; bool ongoing = false; };
static TrailRun trailingAdRun(const std::map<uint64_t, Seg>& tl) {
    TrailRun r;
    if (tl.empty()) return r;
    auto it = tl.rbegin();
    if (classify(it->second) != Kind::Ad) return r;   // newest segment isn't an ad
    r.ongoing = true; r.last = it->first;
    uint64_t prev = it->first + 1;
    for (; it != tl.rend(); ++it) {
        if (it->first + 1 != prev) break;              // contiguity broken
        if (classify(it->second) != Kind::Ad) break;
        r.dur += it->second.dur; r.first = it->first; ++r.segs;
        prev = it->first;
    }
    return r;
}

// Contiguous run of ad segments == one break. A gap in segment numbers (missed
// poll window) breaks contiguity and is reported as a coverage gap.
struct Break { uint64_t first = 0, last = 0; double dur = 0; int segs = 0; std::string spots; };

static void summarize(FILE* log, const std::string& variant,
                      const std::map<uint64_t, Seg>& tl) {
    auto out = [&](const std::string& s){ std::fwrite(s.data(),1,s.size(),log); };
    if (tl.empty()) { out("\n[SUMMARY] no segments captured\n"); return; }

    std::vector<Break> breaks;
    std::map<std::string,int> spotHist;
    double music_sec = 0, imaging_sec = 0, ad_sec = 0;
    int gaps = 0;
    uint64_t prev = 0;
    Break cur; bool inAd = false;

    for (const auto& [n, sg] : tl) {
        if (prev && n != prev + 1) ++gaps;      // missed segments between polls
        spotHist[sg.song_spot.empty() ? "(none)" : sg.song_spot]++;
        Kind k = classify(sg);
        if (k == Kind::Ad) {
            ad_sec += sg.dur;
            if (!inAd) { cur = Break{}; cur.first = n; inAd = true; }
            cur.last = n; cur.dur += sg.dur; cur.segs++;
            if (cur.spots.find(sg.song_spot) == std::string::npos)
                cur.spots += (cur.spots.empty()? "" : ",") + sg.song_spot;
        } else {
            if (k == Kind::Music) music_sec += sg.dur; else imaging_sec += sg.dur;
            if (inAd) { breaks.push_back(cur); inAd = false; }
        }
        prev = n;
    }
    if (inAd) breaks.push_back(cur);

    // Median of break durations.
    std::vector<double> ds; for (auto& b : breaks) ds.push_back(b.dur);
    std::sort(ds.begin(), ds.end());
    double mean = 0, median = 0;
    if (!ds.empty()) {
        for (double d : ds) mean += d;
        mean /= ds.size();
        median = ds.size()%2 ? ds[ds.size()/2] : (ds[ds.size()/2-1]+ds[ds.size()/2])/2.0;
    }

    char buf[256];
    out("\n========== AD-TIER SUMMARY (" + variant + ") ==========\n");
    std::snprintf(buf,sizeof(buf),"segments seen: %zu   coverage gaps: %d   (a gap = polls missed a window)\n",
                  tl.size(), gaps); out(buf);
    std::snprintf(buf,sizeof(buf),"music: %.0fs (%.1f min)   imaging/jingles: %.0fs   PAID ads: %.0fs (%.1f min)\n",
                  music_sec, music_sec/60.0, imaging_sec, ad_sec, ad_sec/60.0); out(buf);
    out("(ad = spotInstanceId != -1, a real paid spot; imaging = song_spot!=M with no spot id)\n");
    out("song_spot histogram (raw signal): ");
    for (auto& [k,v] : spotHist) { std::snprintf(buf,sizeof(buf),"%s=%d  ",k.c_str(),v); out(buf); }
    out("\n");
    std::snprintf(buf,sizeof(buf),"ad breaks: %zu   mean break: %.0fs   median break: %.0fs\n",
                  breaks.size(), mean, median); out(buf);
    int bi = 0, backfills = 0;
    for (auto& b : breaks) {
        bool bf = b.dur >= BACKFILL_SEC;
        if (bf) ++backfills;
        std::snprintf(buf,sizeof(buf),"  break #%d: %.0fs (%.1f min)  %d segs  seq %llu..%llu  spots=[%s]%s\n",
                      ++bi, b.dur, b.dur/60.0, b.segs,
                      (unsigned long long)b.first, (unsigned long long)b.last, b.spots.c_str(),
                      bf ? "   *** BACKFILL (bad tier) ***" : "");
        out(buf);
        if (bf) {
            // Dump the composition of a backfill block: the distinct paid spots
            // inside it (spotInstanceId + title), so the bad tier is characterized.
            out("      backfill composition (distinct paid spots):\n");
            std::map<std::string,std::pair<std::string,double>> spots; // id -> (title, total dur)
            for (uint64_t n = b.first; n <= b.last; ++n) {
                auto it = tl.find(n); if (it == tl.end()) continue;
                if (classify(it->second) != Kind::Ad) continue;
                auto& e = spots[it->second.spot_instance];
                e.first = it->second.title; e.second += it->second.dur;
            }
            for (auto& [id, tv] : spots) {
                std::snprintf(buf,sizeof(buf),"        spot %-10s %4.0fs  \"%s\"\n",
                              id.c_str(), tv.second, tv.first.c_str());
                out(buf);
            }
        }
    }
    if (backfills)
        out("VERDICT: " + std::to_string(backfills) + " BACKFILL block(s) seen - bad tier REPRODUCED.\n");
    else
        out("VERDICT: no backfill block >= " + std::to_string((int)BACKFILL_SEC) +
            "s - only aligned breaks (bad tier NOT reproduced this run).\n");
    out("=====================================================\n");
    // Console echo.
    std::printf("[%s] segs=%zu music=%.0fs ads=%.0fs breaks=%zu mean=%.0fs median=%.0fs\n",
                variant.c_str(), tl.size(), music_sec, ad_sec, breaks.size(), mean, median);
}

// ── Dual-station trackHistory capture (iheart-adboundary-capture) ────────────
// Polls the trackHistory JSON feed for TWO+ stations on a fixed timer and logs the
// FULL data[] each tick as JSON-lines, plus what the PLAYER'S pollNowPlaying WOULD
// select. Instrumentation only: no player/plugin/audio change. The gap duration of
// a SONG -> gap -> SONG transition across an ad boundary is the headline number.
//
// wouldSelect() is the four guards ported VERBATIM from
// plugins/stream/IHeartRadio.cpp::pollNowPlaying (future-filter FUTURE_GRACE=60,
// newest-aired scan of the whole jumbled data[], monotonic accepted_max guard,
// staleness GRACE=30). Only the I/O and the accepted_max storage are adapted (the
// player holds accepted_max_start_ as a member; here it is per-station caller state).
// state: SONG / EMPTY / HELD / FUTURE-REJECTED / LIVE  (POLL-FAIL handled by caller).
static std::string wouldSelect(const json& data, long now, long& accepted_max,
                               int& selIdx, std::string& would, long& ended, int& futureRej) {
    const long FUTURE_GRACE = 60;
    selIdx = -1; would.clear(); ended = -1; futureRej = 0;
    int best = -1; long bestStart = 0;
    for (int i = 0; i < (int)data.size(); ++i) {
        long s = data[i].value("startTime", 0L);
        if (s <= 0) continue;                                    // undated
        if (s > now + FUTURE_GRACE) { ++futureRej; continue; }   // not-yet-aired
        if (best < 0 || s > bestStart) { best = i; bestStart = s; }
    }
    if (best < 0) return futureRej > 0 ? "FUTURE-REJECTED" : "EMPTY";  // nothing aired
    if (bestStart < accepted_max) { selIdx = best; return "HELD"; }    // monotonic: regressed
    accepted_max = bestStart;                                         // (== IHeartRadio.cpp:227)
    const json& t0 = data[best];
    long startTime = bestStart;
    long endTime   = t0.value("endTime", 0L);
    long dur       = t0.value("trackDuration", 0L);
    if (endTime == 0 && startTime > 0 && dur > 0) endTime = startTime + dur;
    ended = (endTime > 0) ? (now - endTime) : -1;
    selIdx = best;
    std::string artist = t0.value("artist", std::string());
    std::string title  = t0.value("title",  std::string());
    if (artist.empty() && title.empty()) return "EMPTY";             // aired entry, no metadata
    would = artist.empty() ? title : (title.empty() ? artist : artist + " - " + title);
    const long GRACE = 30;
    if (endTime > 0 && ended > GRACE) return "LIVE";                 // stale -> would show LIVE
    return "SONG";
}

struct StationCap {
    std::string id;                 // "1469"
    std::string zc;                 // "zc1469"
    long        accepted_max = 0;   // monotonic watermark (per-station)
    std::string lastState = "INIT";
    long        gapStart  = 0;      // epoch when SONG was lost (0 = not in a gap)
    std::string lastSong;           // last SONG string shown
};

static int runCapture(const std::vector<std::string>& ids, int durationSec, int pollSec,
                      const std::string& outdir, long rollBytes) {
    HINTERNET hInet = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { std::fprintf(stderr, "capture: InternetOpenA FAILED\n"); return 1; }

    std::vector<StationCap> st;
    for (const auto& id : ids) st.push_back(StationCap{ id, "zc" + id, 0, "INIT", 0, "" });

    int  fileIdx = 0, filesRolled = 0;
    long bytesOut = 0;
    FILE* log = nullptr;
    auto openLog = [&]() {
        SYSTEMTIME s; GetLocalTime(&s);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                      s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
        std::string p = outdir + "/iheart_capture_" + stamp + "_" + std::to_string(fileIdx) + ".jsonl";
        log = std::fopen(p.c_str(), "wb");
        bytesOut = 0;
        std::fprintf(stderr, "capture -> %s\n", p.c_str());
    };
    openLog();
    if (!log) { std::fprintf(stderr, "capture: cannot open log in %s\n", outdir.c_str()); InternetCloseHandle(hInet); return 1; }

    auto emit = [&](const std::string& line) {
        if (!log) return;
        std::fwrite(line.data(), 1, line.size(), log);
        std::fputc('\n', log);
        std::fflush(log);
        bytesOut += (long)line.size() + 1;
        if (rollBytes > 0 && bytesOut >= rollBytes) {   // roll: never unbounded
            std::fclose(log);
            ++fileIdx; ++filesRolled;
            openLog();
        }
    };

    std::fprintf(stderr, "capture: %zu stations, poll %ds, dur %ds, roll %ldMB\n",
                 st.size(), pollSec, durationSec, rollBytes / (1024*1024));

    DWORD t0 = GetTickCount();
    int poll = 0;
    while ((int)((GetTickCount() - t0) / 1000) < durationSec) {
        ++poll;
        std::string hb;
        for (auto& s : st) {
            std::string url = "https://api.iheart.com/api/v3/live-meta/stream/" + s.id + "/trackHistory";
            std::string body, furl; DWORD http = 0;
            bool ok = httpGet(hInet, url, &body, &furl, &http);
            std::string ts  = nowStamp();
            long        now = (long)std::time(nullptr);

            std::string state; int selIdx = -1; std::string would; long ended = -1; int futureRej = 0;
            json rec;
            rec["ts"] = ts; rec["epoch"] = now; rec["station"] = s.zc; rec["id"] = s.id; rec["http"] = (long)http;
            if (!ok || http != 200) {
                rec["parse"] = "poll-fail";            // a failed poll is itself a data point
                state = "POLL-FAIL";
            } else {
                json j;
                bool parsed = true;
                try { j = json::parse(body); } catch (...) { parsed = false; }
                if (parsed && j.is_object() && j.contains("data") && j["data"].is_array()) {
                    const json& data = j["data"];
                    rec["parse"] = "ok";
                    rec["n"]     = (int)data.size();
                    rec["data"]  = data;               // FULL feed verbatim; unknown fields preserved
                    state = wouldSelect(data, now, s.accepted_max, selIdx, would, ended, futureRej);
                } else {
                    rec["parse"] = parsed ? "no-data" : "parse-fail";
                    state = "EMPTY";
                }
            }
            rec["state"] = state; rec["selIdx"] = selIdx; rec["would"] = would;
            rec["ended"] = ended; rec["futureRej"] = futureRej; rec["acceptedMax"] = s.accepted_max;
            emit(rec.dump());

            // TRANSITION: SONG -> (gap) -> SONG. Gap = any non-SONG state. The gap
            // duration is the number we care about; fromState says which guard held.
            const bool isSong = (state == "SONG");
            const bool isGap  = !isSong && state != "INIT";
            if (s.lastState == "SONG" && isGap && s.gapStart == 0) s.gapStart = now;
            if (isSong) {
                if (s.gapStart > 0) {
                    long gap = now - s.gapStart;
                    json tr;
                    tr["ts"] = ts; tr["epoch"] = now; tr["station"] = s.zc; tr["TRANSITION"] = true;
                    tr["gapSec"] = gap; tr["fromState"] = s.lastState;
                    tr["fromSong"] = s.lastSong; tr["toSong"] = would;
                    emit(tr.dump());
                    std::fprintf(stderr, ">>> TRANSITION %s gap=%lds (%s)  '%s' -> '%s'\n",
                                 s.zc.c_str(), gap, s.lastState.c_str(), s.lastSong.c_str(), would.c_str());
                    s.gapStart = 0;
                }
                s.lastSong = would;
            }
            s.lastState = state;
            hb += " " + s.zc + "=" + state;
        }
        // Heartbeat to stderr (~once a minute at 5s cadence) so a long run is observable.
        if (poll % 12 == 1)
            std::fprintf(stderr, "poll %d,%s, %d files rolled\n", poll, hb.c_str(), filesRolled);
        Sleep(pollSec * 1000);
    }

    if (log) std::fclose(log);
    InternetCloseHandle(hInet);
    std::fprintf(stderr, "capture done: %d polls, %d files rolled\n", poll, filesRolled);
    return 0;
}

int main(int argc, char** argv) {
    int durationSec = 180, pollSec = 3;
    std::string variant = "baseline", logPath, station = "zc4366", analyze;
    bool capture = false;
    std::string cap_stations = "1469,4366", cap_outdir = ".";   // Z100 vs Breeze by default
    long cap_rollbytes = 40L * 1024 * 1024;                      // 40 MB roll (never unbounded)
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--profile=",0)==0)  g_profile  = a.substr(10);
        else if (a.rfind("--listener=",0)==0) g_listener = a.substr(11);
        else if (a == "--no-triton")     g_no_triton = true;
        else if (a == "--capture")       capture = true;
        else if (a.rfind("--stations=",0)==0) cap_stations = a.substr(11);
        else if (a.rfind("--outdir=",0)==0)   cap_outdir   = a.substr(9);
        else if (a.rfind("--rollmb=",0)==0)   cap_rollbytes = (long)std::atoi(a.substr(9).c_str()) * 1024 * 1024;
        else if (a.rfind("--station=",0)==0)  station = a.substr(10);
        else if (a.rfind("--analyze=",0)==0)  analyze = a.substr(10);
        else pos.push_back(a);
    }

    auto slurp = [](const std::string& p, std::string& all) -> bool {
        FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false;
        char b[65536]; size_t g;
        while ((g = std::fread(b,1,sizeof(b),f)) > 0) all.append(b, g);
        std::fclose(f); return true;
    };

    // Re-analyze a saved log with the current classifier (no network). The saved
    // log embeds every verbatim manifest, so ingest() reconstructs the timeline.
    if (!analyze.empty()) {
        std::string all;
        if (!slurp(analyze, all)) { std::printf("cannot open %s\n", analyze.c_str()); return 1; }
        std::map<uint64_t, Seg> tl; ingest(all, tl);
        summarize(stdout, "analyze:" + analyze, tl);
        return 0;
    }

    // Task 2 diff: --diff=logA,logB - do two concurrent pulls serve the SAME
    // content? Compares segment-number sets (same mount/stitch = high overlap;
    // divergent = disjoint) and each side's backfill verdict, so it shows WHICH
    // pull got the bad tier when they split.
    {
        size_t cpos;
        for (int i = 1; i < argc; ++i) { std::string a=argv[i];
            if (a.rfind("--diff=",0)==0 && (cpos=a.find(','))!=std::string::npos) {
                std::string pa = a.substr(7, cpos-7), pb = a.substr(cpos+1);
                std::string sa, sb;
                if (!slurp(pa,sa) || !slurp(pb,sb)) { std::printf("cannot open diff inputs\n"); return 1; }
                std::map<uint64_t,Seg> ta, tb; ingest(sa, ta); ingest(sb, tb);
                size_t both=0, onlyA=0, onlyB=0;
                for (auto& [n,s] : ta) { (void)s; if (tb.count(n)) ++both; else ++onlyA; }
                for (auto& [n,s] : tb) { (void)s; if (!ta.count(n)) ++onlyB; }
                double jac = (both+onlyA+onlyB) ? (double)both/(both+onlyA+onlyB) : 0;
                std::printf("=== DIFF ===\nA=%s (%zu segs)\nB=%s (%zu segs)\n",
                            pa.c_str(), ta.size(), pb.c_str(), tb.size());
                std::printf("shared segments: %zu   only-A: %zu   only-B: %zu   overlap(Jaccard)=%.3f\n",
                            both, onlyA, onlyB, jac);
                std::printf("%s\n", jac > 0.9 ? "-> SAME content (converged: same mount/stitch)"
                                   : jac < 0.5 ? "-> DIVERGENT content (different stitch/mount)"
                                               : "-> PARTIAL divergence (split at some point)");
                std::printf("\n[A tier]\n"); summarize(stdout, "A:"+pa, ta);
                std::printf("\n[B tier]\n"); summarize(stdout, "B:"+pb, tb);
                return 0;
            }
        }
    }
    if (pos.size() > 0) durationSec = std::atoi(pos[0].c_str());
    if (pos.size() > 1) pollSec     = std::atoi(pos[1].c_str());
    if (pos.size() > 2) variant     = pos[2];
    if (pos.size() > 3) logPath      = pos[3];
    if (durationSec < 1) durationSec = 180;
    if (pollSec < 1) pollSec = 3;

    // Dual-station trackHistory capture mode (iheart-adboundary-capture). Separate
    // from the manifest/ad-tier path below; positionals are [durationSec] [pollSec]
    // (poll defaults to 5s here, tighter than the player's ~10s, when not given).
    if (capture) {
        std::vector<std::string> ids;
        for (size_t p = 0; p < cap_stations.size(); ) {
            size_t c = cap_stations.find(',', p);
            std::string tok = cap_stations.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (tok.rfind("zc", 0) == 0) tok = tok.substr(2);     // accept "1469" or "zc1469"
            if (!tok.empty()) ids.push_back(tok);
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (ids.empty()) { std::fprintf(stderr, "capture: no stations (use --stations=1469,4366)\n"); return 1; }
        if (cap_rollbytes < 1024 * 1024) cap_rollbytes = 40L * 1024 * 1024;
        int capPoll = (pos.size() >= 2) ? pollSec : 5;            // 5s default unless a poll positional is given
        return runCapture(ids, durationSec, capPoll, cap_outdir, cap_rollbytes);
    }

    if (logPath.empty()) {
        SYSTEMTIME st; GetLocalTime(&st);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        logPath = "ihearthttpdump_" + variant + "_" + stamp + ".log";
    }

    std::srand((unsigned)(GetTickCount() ^ (unsigned)time(nullptr)));

    std::string base = "https://stream.revma.ihrhls.com/" + station + "/hls.m3u8";
    std::string digital = buildDigitalUrl(base, variant);

    FILE* log = std::fopen(logPath.c_str(), "wb");
    if (!log) { std::printf("cannot open %s\n", logPath.c_str()); return 1; }
    auto out = [&](const std::string& s){ std::fwrite(s.data(),1,s.size(),log); std::fflush(log); };

    out("=== iHeart digital manifest + ad-tier dump ===\n");
    out("started:  " + nowStamp() + "\n");
    out("variant:  " + variant + "\n");
    out("station:  " + station + "\n");
    if (variant == "fullhandshake") {
        out("profileId: " + (g_profile.empty()? std::string(CAPTURED_PROFILE)+" (captured control - not shippable)" : g_profile) + "\n");
        out("listenerId:" + (g_listener.empty()? std::string("(empty - matches capture)") : g_listener) + "\n");
        out("triton lt: " + std::string(g_no_triton ? "DISABLED (--no-triton A/B)" : "enabled") + "\n");
    }
    out("base:     " + base + "\n");
    out("digital:  " + digital + "\n");
    out("duration: " + std::to_string(durationSec) + "s  poll: " + std::to_string(pollSec) + "s\n\n");
    std::printf("probe -> %s  (%ds, every %ds, variant=%s)\n",
                logPath.c_str(), durationSec, pollSec, variant.c_str());

    HINTERNET hInet = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { out("InternetOpenA FAILED\n"); std::fclose(log); return 1; }

    std::string body, finalUrl; DWORD status = 0;
    if (!httpGet(hInet, digital, &body, &finalUrl, &status)) {
        out("HANDSHAKE GET FAILED\n"); std::fclose(log); InternetCloseHandle(hInet); return 1;
    }
    out("handshake final-url (after redirects):\n  " + finalUrl + "\n");
    out("handshake HTTP status: " + std::to_string(status) + "\n");

    // Triton listen-tracking call (fullhandshake only). vid = the profileId; the
    // web player fires this alongside the stream request. A/B via --no-triton.
    if (variant == "fullhandshake" && !g_no_triton) {
        std::string pid = g_profile.empty()? CAPTURED_PROFILE : g_profile;
        std::string lt = "https://lt110.tritondigital.com/lt?sid=" + std::string(TRITON_SID) +
                         "&vid=" + pid + "&cb=" + std::to_string((unsigned)GetTickCount());
        std::string ltbody, ltfinal; DWORD ltst = 0;
        bool ltok = httpGet(hInet, lt, &ltbody, &ltfinal, &ltst);
        out("triton lt call: " + lt + "\n");
        out("triton lt status: " + std::string(ltok? std::to_string(ltst) : "FAILED") +
            "  (" + std::to_string(ltbody.size()) + " bytes)\n");
    }

    std::string variantUrl;
    if (body.find("#EXT-X-STREAM-INF") != std::string::npos) {
        variantUrl = resolveUrl(finalUrl, firstUri(body));
        out("master playlist -> variant:\n  " + variantUrl + "\n\n");
    } else if (body.find("#EXTINF") != std::string::npos) {
        variantUrl = finalUrl;
        out("(handshake returned a media playlist directly)\n\n");
    } else {
        out("UNRECOGNIZED handshake body (" + std::to_string(body.size()) + " bytes):\n");
        out(body + "\n");
        std::fclose(log); InternetCloseHandle(hInet); return 1;
    }
    std::printf("variant resolved. polling...\n");

    std::map<uint64_t, Seg> timeline;
    DWORD t0 = GetTickCount();
    int n = 0;
    double bfAlerted = 0;          // longest trailing-ad-run already marked this run
    while ((int)((GetTickCount() - t0) / 1000) < durationSec) {
        std::string m, fu; DWORD st = 0;
        bool ok = httpGet(hInet, variantUrl, &m, &fu, &st);
        std::string ts = nowStamp();
        out("\n----- poll #" + std::to_string(++n) + "  " + ts +
            "  HTTP " + std::to_string(st) + "  bytes=" + std::to_string(m.size()) + " -----\n");
        if (!ok) { out("(GET failed)\n"); }
        else {
            size_t before = timeline.size();
            ingest(m, timeline);
            out(m);
            if (!m.empty() && m.back() != '\n') out("\n");
            // Live backfill/desync marker (the proxy repin condition): a trailing
            // paid-ad run crossing the threshold, re-marked each further ~60s it grows.
            TrailRun tr = trailingAdRun(timeline);
            if (tr.ongoing && tr.dur >= BACKFILL_SEC && tr.dur >= bfAlerted + 60) {
                bfAlerted = tr.dur;
                char mk[192];
                std::snprintf(mk, sizeof(mk),
                    ">>> BACKFILL-MARKER %s  trailing paid-ad run %.0fs (%.1f min) and counting  seq %llu..%llu\n",
                    ts.c_str(), tr.dur, tr.dur/60.0,
                    (unsigned long long)tr.first, (unsigned long long)tr.last);
                out(mk);
                std::printf("%s", mk);
            } else if (!tr.ongoing) {
                bfAlerted = 0;      // run ended; arm for the next block
            }
            // Console line: new segments this poll + the current window's lead spot.
            std::string spot;
            { size_t p = m.find("song_spot="); spot = p==std::string::npos? "?" : escAttr(m.substr(p), "song_spot="); }
            std::printf("  #%d %s  new_segs=%zu  lead_spot=%s  total=%zu\n",
                        n, ts.c_str()+11, timeline.size()-before, spot.c_str(), timeline.size());
        }
        Sleep(pollSec * 1000);
    }

    summarize(log, variant, timeline);
    out("\n=== done: " + nowStamp() + " ===\n");
    std::fclose(log);
    InternetCloseHandle(hInet);
    std::printf("done -> %s\n", logPath.c_str());
    return 0;
}
