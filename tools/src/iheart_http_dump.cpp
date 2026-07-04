// iheart_http_dump.cpp — standalone iHeart digital-manifest probe.
//
// Reproduces RE-MOCT's exact digital (web-player) handshake for a station, then
// polls the resolved short-lived variant playlist and dumps every manifest
// revision verbatim to a log, so we can inspect the steering tags the browser
// uses: #EXT-X-PROGRAM-DATE-TIME, #EXT-X-DISCONTINUITY, #EXT-X-MEDIA-SEQUENCE,
// EXTINF, etc. The audio segments are NOT downloaded — only the small .m3u8 text.
//
// Build (MSYS2 UCRT64):
//   g++ -std=c++20 iheart_http_dump.cpp -o iheart_http_dump.exe -lwininet
//
// Run:  iheart_http_dump.exe [duration] [pollSec] [variant] [logPath]
//   variant = baseline | clone | emptyid | synth   (default baseline)
//   defaults: 180s, poll 3s, log ihearthttpdump_<variant>.log
//
//   iheart_http_dump.exe 180 3 baseline
//   iheart_http_dump.exe 180 3 clone
//   iheart_http_dump.exe 180 3 emptyid
//   iheart_http_dump.exe 180 3 synth
//
// Start each during a song and let it run through a break. Catch 2-3 breaks per
// variant before trusting a difference — ad fill is partly luck-of-the-draw.

#include <windows.h>
#include <wininet.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

static const char* USER_AGENT =
    "RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)";

static std::string nowStamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    char b[64];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

// Build the digital handshake URL for one of four test variants. The point is to
// find out which session-identity params control the ad-fill length (web player
// gets a clean ~3-min break; RE-MOCT's anonymous-new-every-time session gets a
// 15-20 min house-bin backfill). Real web-player values observed: profileId and
// aw_0_1st.skey both = 12663682685, listenerId empty, zip empty.
//   baseline : current RE-MOCT — fresh random listenerId, no profile      (control)
//   clone    : exact web-player string — real profileId + skey, empty lid
//   emptyid  : RE-MOCT params but listenerId empty, still no profile
//   synth    : empty lid + a SYNTHETIC stable profileId/skey (shippable test)
static const char* SYNTH_PROFILE = "12700000420";   // fake but plausible, stable across runs

static std::string buildDigitalUrl(const std::string& base, const std::string& variant) {
    std::string id;
    size_t p = base.find("/zc");
    if (p != std::string::npos) {
        p += 3;
        while (p < base.size() && std::isdigit((unsigned char)base[p])) id += base[p++];
    }
    if (id.empty()) return base;

    // random 32-hex listenerId (baseline only)
    static const char* H = "0123456789abcdef";
    std::string lid; lid.reserve(32);
    for (int i = 0; i < 32; ++i) lid += H[std::rand() & 15];

    std::string skey, profile, listener;
    if (variant == "clone") {
        skey = "12663682685"; profile = "12663682685"; listener = "";
    } else if (variant == "emptyid") {
        skey = "";            profile = "";            listener = "";
    } else if (variant == "synth") {
        skey = SYNTH_PROFILE; profile = SYNTH_PROFILE; listener = "";
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

// First non-comment line of a playlist (the variant URI in a master).
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

// GET; returns body, the post-redirect final URL, and HTTP status. Mirrors hlsHttpGet.
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

// Quick presence flags so the console gives an at-a-glance verdict per poll.
static void flags(const std::string& m, bool& pdt, bool& disc, std::string& mseq) {
    pdt  = m.find("#EXT-X-PROGRAM-DATE-TIME") != std::string::npos;
    disc = m.find("#EXT-X-DISCONTINUITY")     != std::string::npos;
    size_t p = m.find("#EXT-X-MEDIA-SEQUENCE:");
    if (p != std::string::npos) {
        p += 22; size_t e = m.find_first_of("\r\n", p);
        mseq = m.substr(p, (e == std::string::npos ? m.size() : e) - p);
    } else mseq = "?";
}

int main(int argc, char** argv) {
    int durationSec = (argc > 1) ? std::atoi(argv[1]) : 180;
    int pollSec     = (argc > 2) ? std::atoi(argv[2]) : 3;
    std::string variant = (argc > 3) ? argv[3] : "baseline";   // baseline|clone|emptyid|synth
    std::string station = "zc4366";                            // Breeze
    // Default log name carries variant + start timestamp so repeated runs never
    // overwrite and sort chronologically: ihearthttpdump_<variant>_YYYYMMDD-HHMMSS.log
    std::string logPath;
    if (argc > 4) {
        logPath = argv[4];
    } else {
        SYSTEMTIME st; GetLocalTime(&st);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        logPath = "ihearthttpdump_" + variant + "_" + stamp + ".log";
    }
    if (durationSec < 1) durationSec = 180;
    if (pollSec < 1) pollSec = 3;

    std::srand((unsigned)(GetTickCount() ^ (unsigned)time(nullptr)));

    std::string base = "https://stream.revma.ihrhls.com/" + station + "/hls.m3u8";
    std::string digital = buildDigitalUrl(base, variant);

    FILE* log = std::fopen(logPath.c_str(), "wb");
    if (!log) { std::printf("cannot open %s\n", logPath.c_str()); return 1; }

    auto out = [&](const std::string& s){ std::fwrite(s.data(),1,s.size(),log); std::fflush(log); };

    out("=== iHeart digital manifest dump ===\n");
    out("started:  " + nowStamp() + "\n");
    out("variant:  " + variant + "\n");
    out("station:  " + station + "\n");
    out("base:     " + base + "\n");
    out("digital:  " + digital + "\n");
    out("duration: " + std::to_string(durationSec) + "s  poll: " + std::to_string(pollSec) + "s\n\n");
    std::printf("probe -> %s  (%ds, every %ds)\n", logPath.c_str(), durationSec, pollSec);

    HINTERNET hInet = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { out("InternetOpenA FAILED\n"); std::fclose(log); return 1; }

    // Handshake: GET the digital URL (302 -> node), then resolve the variant.
    std::string body, finalUrl; DWORD status = 0;
    if (!httpGet(hInet, digital, &body, &finalUrl, &status)) {
        out("HANDSHAKE GET FAILED\n"); std::fclose(log); InternetCloseHandle(hInet); return 1;
    }
    out("handshake final-url (after redirects):\n  " + finalUrl + "\n");
    out("handshake HTTP status: " + std::to_string(status) + "\n");

    std::string variantUrl;
    if (body.find("#EXT-X-STREAM-INF") != std::string::npos) {
        variantUrl = resolveUrl(finalUrl, firstUri(body));
        out("master playlist -> variant:\n  " + variantUrl + "\n\n");
    } else if (body.find("#EXTINF") != std::string::npos) {
        variantUrl = finalUrl;                       // already a media playlist
        out("(handshake returned a media playlist directly)\n\n");
    } else {
        out("UNRECOGNIZED handshake body (" + std::to_string(body.size()) + " bytes):\n");
        out(body + "\n");
        std::fclose(log); InternetCloseHandle(hInet); return 1;
    }
    std::printf("variant resolved. polling...\n");

    // Poll the variant playlist, dumping each revision verbatim.
    DWORD t0 = GetTickCount();
    int n = 0;
    while ((int)((GetTickCount() - t0) / 1000) < durationSec) {
        std::string m, fu; DWORD st = 0;
        bool ok = httpGet(hInet, variantUrl, &m, &fu, &st);
        std::string ts = nowStamp();
        out("\n----- poll #" + std::to_string(++n) + "  " + ts +
            "  HTTP " + std::to_string(st) + "  bytes=" + std::to_string(m.size()) + " -----\n");
        if (!ok) { out("(GET failed)\n"); }
        else {
            bool pdt=false, disc=false; std::string mseq;
            flags(m, pdt, disc, mseq);
            out("[flags] PDT=" + std::string(pdt?"yes":"no") +
                "  DISCONTINUITY=" + std::string(disc?"yes":"no") +
                "  MEDIA-SEQUENCE=" + mseq + "\n");
            out(m);
            if (!m.empty() && m.back() != '\n') out("\n");
            std::printf("  #%d %s  PDT=%d DISC=%d mseq=%s\n",
                        n, ts.c_str()+11, pdt, disc, mseq.c_str());
        }
        Sleep(pollSec * 1000);
    }

    out("\n=== done: " + nowStamp() + " ===\n");
    std::fclose(log);
    InternetCloseHandle(hInet);
    std::printf("done -> %s\n", logPath.c_str());
    return 0;
}
