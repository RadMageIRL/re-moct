// StreamHandshakeProbe.cpp  —  does the iHeart digital-rendition handshake work
// with THROWAWAY credentials, or does it need a real session?
//
// RE-MOCT plays the raw broadcast (zc####/hls.m3u8, heavy terrestrial ads). The
// web player hits the SAME base URL with a player query string + listenerId/
// profileId, gets a 302 to a regional node carrying a server-minted `rj-tok`,
// and lands on an ad-reduced "digital" rendition. This probe answers the only
// open question before building an "upgrade-with-fallback" connect path:
//
//   Will the revma edge mint a token for an ARBITRARY (random) listenerId /
//   profileId, or are those bound to a real account/session?
//
// It runs three requests and prints the full redirect chain for each:
//   [RAW]      base URL, no params           — control (today's RE-MOCT behavior)
//   [DIGITAL]  base URL + params, RANDOM ids  — the decisive test
//   [NO-PROF]  base URL + params, no profileId/skey — is profileId even required?
//
// Reading the result:
//   * DIGITAL reaches a 200 playlist (and the chain shows `rj-tok`) with random
//     ids  -> OUTCOME 1: fully automatable, no session call. Build it.
//   * DIGITAL fails (403/redirect-to-nothing) but RAW works                 ->
//     OUTCOME 2/3: profileId/skey must be real -> session-mint call needed, or
//     it's account-bound and not worth it.
//
// NOT part of the RE-MOCT build. Compile on demand:
//   g++ -std=c++20 StreamHandshakeProbe.cpp -o StreamHandshakeProbe.exe -lwininet
// Usage:
//   StreamHandshakeProbe.exe [base_url]
//     base_url defaults to https://stream.revma.ihrhls.com/zc1469/hls.m3u8
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <ctime>

// ── one HTTP hop, auto-redirect DISABLED so we can see each 302 + Location ────
struct Hop {
    long        status  = 0;
    std::string location;
    std::string body;
    DWORD       win_err = 0;
};

static Hop getOnce(HINTERNET hInet, const std::string& url) {
    Hop h;
    const char* headers =
        "Accept: */*\r\n"
        "Origin: https://www.iheart.com\r\n"
        "Referer: https://www.iheart.com/\r\n";
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION |
                  INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_AUTO_REDIRECT;
    HINTERNET hr = InternetOpenUrlA(hInet, url.c_str(), headers, (DWORD)-1L, flags, 0);
    if (!hr) { h.win_err = GetLastError(); return h; }

    DWORD code = 0, clen = sizeof(code), idx = 0;
    if (HttpQueryInfoA(hr, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, &idx))
        h.status = (long)code;

    char loc[4096]; DWORD llen = sizeof(loc), i2 = 0;
    if (HttpQueryInfoA(hr, HTTP_QUERY_LOCATION, loc, &llen, &i2))
        h.location.assign(loc, llen);

    std::string body; char buf[8192]; DWORD got = 0;
    while (InternetReadFile(hr, buf, sizeof(buf), &got) && got > 0) {
        body.append(buf, got);
        if (body.size() > 65536) break;
    }
    InternetCloseHandle(hr);
    h.body = std::move(body);
    return h;
}

static bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

static std::string resolveLoc(const std::string& base, const std::string& loc) {
    if (loc.rfind("http", 0) == 0) return loc;
    if (!loc.empty() && loc[0] == '/') {
        size_t s = base.find("://"); if (s == std::string::npos) return loc;
        size_t he = base.find('/', s + 3);
        return (he == std::string::npos ? base : base.substr(0, he)) + loc;
    }
    size_t sl = base.find_last_of('/');
    return (sl == std::string::npos ? base : base.substr(0, sl + 1)) + loc;
}

struct ChainResult { bool reachedPlaylist = false; bool sawRjTok = false; long firstStatus = 0; };

static ChainResult runChain(HINTERNET hInet, const std::string& start, const char* label) {
    std::printf("\n=== [%s] ===\n", label);
    ChainResult cr;
    std::string url = start;
    for (int hop = 0; hop < 6; ++hop) {
        Hop h = getOnce(hInet, url);
        if (hop == 0) cr.firstStatus = h.status;
        if (h.status == 0) {
            std::printf("  hop%d: REQUEST FAILED (win_err=%lu)\n         %s\n", hop, h.win_err, url.c_str());
            return cr;
        }
        std::printf("  hop%d: HTTP %ld  %s\n", hop, h.status,
                    url.size() > 110 ? (url.substr(0, 107) + "...").c_str() : url.c_str());
        if (h.status >= 300 && h.status < 400 && !h.location.empty()) {
            bool tok = contains(h.location, "rj-tok");
            if (tok) cr.sawRjTok = true;
            std::printf("        -> Location: %s%s\n",
                        h.location.size() > 100 ? (h.location.substr(0, 97) + "...").c_str() : h.location.c_str(),
                        tok ? "   ** rj-tok minted **" : "");
            url = resolveLoc(url, h.location);
            continue;
        }
        if (h.status == 200) {
            bool pl = contains(h.body, "#EXTM3U");
            cr.reachedPlaylist = pl;
            std::printf("        200 body=%zub  playlist=%s (STREAM-INF=%s EXTINF=%s)\n",
                        h.body.size(), pl ? "YES" : "no",
                        contains(h.body, "#EXT-X-STREAM-INF") ? "y" : "n",
                        contains(h.body, "#EXTINF") ? "y" : "n");
            std::string pre = h.body.substr(0, 180);
            for (char& c : pre) if (c == '\n' || c == '\r') c = ' ';
            std::printf("        body[0:180]: %s\n", pre.c_str());
            return cr;
        }
        std::printf("        (status %ld, not a redirect or 200 — stopping)\n", h.status);
        return cr;
    }
    std::printf("  (redirect limit reached)\n");
    return cr;
}

// ── throwaway credential generators ──────────────────────────────────────────
static std::string randHex(int n) {
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(n);
    for (int i = 0; i < n; ++i) s += H[std::rand() & 15];
    return s;
}
static std::string randDigits(int n) {
    std::string s; s.reserve(n);
    s += char('1' + (std::rand() % 9));           // no leading zero
    for (int i = 1; i < n; ++i) s += char('0' + (std::rand() % 10));
    return s;
}

static std::string extractStationId(const std::string& url) {
    size_t p = url.find("/zc");
    if (p == std::string::npos) return "1469";
    p += 3; std::string id;
    while (p < url.size() && std::isdigit((unsigned char)url[p])) id += url[p++];
    return id.empty() ? "1469" : id;
}

// Build the web-player query string. profileId==skey in observed traffic.
static std::string playerParams(const std::string& id, const std::string& lid,
                                const std::string& pid, bool includeProfile) {
    std::string q = "?streamid=" + id +
        "&zip=&aw_0_1st.playerid=iHeartRadioWebPlayer";
    if (includeProfile) q += "&aw_0_1st.skey=" + pid;
    q += "&clientType=web&companionAds=false&deviceName=web-mobile&dist=iheart&host=webapp.US"
         "&listenerId=" + lid +
         "&playedFrom=157&pname=live_profile";
    if (includeProfile) q += "&profileId=" + pid;
    q += "&stationid=" + id + "&terminalId=159&territory=US&us_privacy=1-N-";
    return q;
}

int main(int argc, char** argv) {
    std::srand((unsigned)(std::time(nullptr) ^ GetTickCount()));
    std::string base = (argc >= 2) ? argv[1]
                                   : "https://stream.revma.ihrhls.com/zc1469/hls.m3u8";
    std::string id  = extractStationId(base);
    std::string lid = randHex(32);
    std::string pid = randDigits(11);

    std::printf("StreamHandshakeProbe\n base=%s\n station=%s\n throwaway listenerId=%s  profileId=%s\n",
                base.c_str(), id.c_str(), lid.c_str(), pid.c_str());

    HINTERNET hInet = InternetOpenA(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { std::printf("InternetOpen failed (%lu)\n", GetLastError()); return 2; }

    ChainResult raw  = runChain(hInet, base, "RAW (no params, today's behavior)");
    ChainResult dig  = runChain(hInet, base + playerParams(id, lid, pid, true),
                                "DIGITAL (player params, RANDOM ids)");
    ChainResult nop  = runChain(hInet, base + playerParams(id, lid, pid, false),
                                "NO-PROF (player params, no profileId/skey)");

    std::printf("\n================ VERDICT ================\n");
    std::printf(" RAW     : %s (first HTTP %ld)\n", raw.reachedPlaylist ? "playlist OK" : "no playlist", raw.firstStatus);
    std::printf(" DIGITAL : %s%s (first HTTP %ld)\n",
                dig.reachedPlaylist ? "playlist OK" : "FAILED",
                dig.sawRjTok ? ", rj-tok minted" : "", dig.firstStatus);
    std::printf(" NO-PROF : %s%s (first HTTP %ld)\n",
                nop.reachedPlaylist ? "playlist OK" : "FAILED",
                nop.sawRjTok ? ", rj-tok minted" : "", nop.firstStatus);
    std::printf("-----------------------------------------\n");
    if (dig.reachedPlaylist && dig.sawRjTok)
        std::printf(" => OUTCOME 1: edge mints a token for ARBITRARY ids. Fully automatable,\n"
                    "    no session call. The upgrade-with-fallback path is worth building.%s\n",
                    nop.reachedPlaylist ? "\n    (profileId not even required — NO-PROF also worked.)" : "");
    else if (raw.reachedPlaylist)
        std::printf(" => OUTCOME 2/3: random ids did NOT yield a digital rendition, but RAW works.\n"
                    "    profileId/skey likely must be session-minted (one more call to find) or\n"
                    "    account-bound (not worth it). Stay raw unless the session-mint call is cheap.\n");
    else
        std::printf(" => INCONCLUSIVE: even RAW failed — network/headers/geo issue, not the handshake.\n"
                    "    (iHeart streams are US-geo; check connectivity.)\n");

    InternetCloseHandle(hInet);
    return 0;
}

#endif // _WIN32
