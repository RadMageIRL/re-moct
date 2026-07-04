// LatencyProbe.cpp  —  standalone stream-latency probe for RE-MOCT
//
// Measures how far RE-MOCT's RAW iHeart rendition (zc####/hls.m3u8) sits behind
// the iHeart WEB PLAYER's rendition, to decompose the ~100s metadata/audio lead
// we measured in the deep log into (a) our deliberate prime buffer, (b) the raw
// rendition's live edge sitting behind the web rendition, and (c) iHeart's
// metadata simply running ahead of playout.
//
// Both renditions draw segments from the SAME pool (cloud-proxy-hls .../main/<N>.aac),
// so the segment-file number N is a shared timeline coordinate. The probe reads the
// live-edge N of each playlist, converts the difference to seconds using the
// per-segment N increment and the target duration parsed from the RAW manifest, and
// reports the offset once per sample over a short run.
//
// NOT part of the RE-MOCT build. Compile on demand:
//   g++ -std=c++20 LatencyProbe.cpp -o LatencyProbe.exe -lwininet
//
// Usage:
//   LatencyProbe.exe <raw_master_url> [web_playlist_url]
//     <raw_master_url>   the canonical zc####/hls.m3u8 RE-MOCT plays
//     [web_playlist_url] (optional) the tokenized playlist.m3u8 from the web
//                        player (devtools -> Network -> copy the playlist.m3u8 URL).
//                        Token is session-bound but valid for a probe run.
//   With both URLs it prints the offset automatically each sample. With only the
//   raw URL it prints the raw live edge + the N-per-second scale so you can diff
//   it by hand against a .aac segment number read from devtools.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <ctime>

// ── HTTP GET via WinINet (mirrors SniffIHeartRadio) ──────────────────────────
struct HttpResult {
    bool        ok     = false;
    DWORD       status = 0;
    std::string body;
    std::string final_url;
    DWORD       win_err = 0;
};

static HttpResult httpGet(HINTERNET hInet, const std::string& url) {
    HttpResult r;
    const char* headers =
        "Accept: */*\r\n"
        "Origin: https://z100.iheart.com\r\n"
        "Referer: https://z100.iheart.com/\r\n";
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION |
                  INTERNET_FLAG_SECURE;
    HINTERNET h = InternetOpenUrlA(hInet, url.c_str(), headers, (DWORD)-1L, flags, 0);
    if (!h) { r.win_err = GetLastError(); return r; }

    char fbuf[2048]; DWORD flen = sizeof(fbuf);
    if (InternetQueryOptionA(h, INTERNET_OPTION_URL, fbuf, &flen)) r.final_url.assign(fbuf, flen);
    else r.final_url = url;

    DWORD code = 0, clen = sizeof(code), idx = 0;
    if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, &idx))
        r.status = code;

    std::string body;
    char chunk[8192]; DWORD got = 0;
    while (InternetReadFile(h, chunk, sizeof(chunk), &got) && got > 0) {
        body.append(chunk, got);
        if (body.size() > 4u * 1024u * 1024u) break;
    }
    InternetCloseHandle(h);
    r.body = std::move(body);
    r.ok   = true;
    return r;
}

// ── small helpers ────────────────────────────────────────────────────────────
static std::string nowStamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return b;
}

static long readTagLong(const std::string& body, const char* tag) {
    size_t p = body.find(tag);
    if (p == std::string::npos) return -1;
    p += std::char_traits<char>::length(tag);
    long v = 0; bool any = false;
    while (p < body.size() && std::isdigit((unsigned char)body[p])) { v = v*10 + (body[p]-'0'); ++p; any = true; }
    return any ? v : -1;
}

// Resolve a (possibly relative) URI against a base URL.
static std::string resolveUrl(const std::string& base, const std::string& ref) {
    if (ref.rfind("http", 0) == 0) return ref;
    if (!ref.empty() && ref[0] == '/') {                 // absolute path
        size_t s = base.find("://"); if (s == std::string::npos) return ref;
        size_t host_end = base.find('/', s + 3);
        return (host_end == std::string::npos ? base : base.substr(0, host_end)) + ref;
    }
    size_t slash = base.find_last_of('/');               // relative to dir
    return (slash == std::string::npos ? base : base.substr(0, slash + 1)) + ref;
}

// Pull the trailing integer from a segment URI filename (".../main/178534035.aac" -> 178534035).
static long long segNumOf(const std::string& uri) {
    size_t q = uri.find('?'); std::string u = (q==std::string::npos)? uri : uri.substr(0,q);
    size_t dot = u.rfind(".aac"); if (dot == std::string::npos) dot = u.find_last_of('.');
    if (dot == std::string::npos) dot = u.size();
    size_t e = dot; size_t s = e;
    while (s > 0 && std::isdigit((unsigned char)u[s-1])) --s;
    if (s == e) return -1;
    long long v = 0; for (size_t i = s; i < e; ++i) v = v*10 + (u[i]-'0');
    return v;
}

struct Edge {
    bool        ok        = false;
    long        status    = 0;
    long        mediaSeq  = -1;
    long        targetDur = -1;
    int         segCount  = 0;
    long long   edgeNum   = -1;   // live-edge segment file number
    double      incr      = 0.0;  // avg file-number increment per segment
};

// Fetch a playlist (resolving master->variant if needed) and read its live edge.
static Edge fetchEdge(HINTERNET hInet, const std::string& url) {
    Edge e;
    HttpResult r = httpGet(hInet, url);
    e.status = (long)r.status;
    if (!r.ok || r.status != 200) return e;

    std::string body = r.body, base = r.final_url;
    if (body.find("#EXT-X-STREAM-INF") != std::string::npos) {   // master -> first variant
        std::string variant;
        size_t p = 0;
        while (p < body.size()) {
            size_t nl = body.find('\n', p);
            std::string line = body.substr(p, (nl==std::string::npos? body.size():nl) - p);
            while (!line.empty() && (line.back()=='\r'||line.back()==' ')) line.pop_back();
            if (!line.empty() && line[0] != '#') { variant = line; break; }
            p = (nl==std::string::npos)? body.size() : nl + 1;
        }
        if (variant.empty()) return e;
        std::string vurl = resolveUrl(base, variant);
        HttpResult r2 = httpGet(hInet, vurl);
        e.status = (long)r2.status;
        if (!r2.ok || r2.status != 200) return e;
        body = r2.body; base = r2.final_url;
    }

    e.mediaSeq  = readTagLong(body, "#EXT-X-MEDIA-SEQUENCE:");
    e.targetDur = readTagLong(body, "#EXT-X-TARGETDURATION:");

    std::vector<long long> segs;
    size_t p = 0;
    while (p < body.size()) {
        size_t nl = body.find('\n', p);
        std::string line = body.substr(p, (nl==std::string::npos? body.size():nl) - p);
        while (!line.empty() && (line.back()=='\r'||line.back()==' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') {
            long long n = segNumOf(line);
            if (n >= 0) segs.push_back(n);
        }
        p = (nl==std::string::npos)? body.size() : nl + 1;
    }
    e.segCount = (int)segs.size();
    if (!segs.empty()) {
        e.edgeNum = segs.back();
        if (segs.size() > 1) e.incr = double(segs.back() - segs.front()) / double(segs.size() - 1);
    }
    e.ok = (e.edgeNum >= 0);
    return e;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: LatencyProbe.exe <raw_master_url> [web_playlist_url]\n");
        return 1;
    }
    std::string rawUrl = argv[1];
    std::string webUrl = (argc >= 3) ? argv[2] : std::string();

    // Impersonate the web player: tokenized renditions enforce a browser UA +
    // Origin/Referer (session/anti-hotlink binding). Harmless on the raw host.
    HINTERNET hInet = InternetOpenA(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { std::printf("InternetOpen failed (%lu)\n", GetLastError()); return 2; }

    const int SAMPLES = 12, PERIOD_MS = 5000;
    std::printf("LatencyProbe: raw=%s\n", rawUrl.c_str());
    if (!webUrl.empty()) std::printf("            web=%s\n", webUrl.c_str());
    std::printf("%-9s %-14s %-7s %-6s %-5s | %-14s %-7s | %s\n",
                "time", "raw_edge", "rawSeq", "segDur", "incr", "web_edge", "webSeq", "offset(web ahead)");
    std::printf("---------------------------------------------------------------------------------------------\n");

    double sum = 0; int n = 0;
    for (int i = 0; i < SAMPLES; ++i) {
        Edge raw = fetchEdge(hInet, rawUrl);
        Edge web; if (!webUrl.empty()) web = fetchEdge(hInet, webUrl);

        std::string ts = nowStamp();
        if (!raw.ok) {
            std::printf("%-9s raw FETCH FAIL (status=%ld)\n", ts.c_str(), raw.status);
        } else if (webUrl.empty()) {
            double nps = (raw.targetDur > 0 && raw.incr > 0) ? raw.incr / raw.targetDur : 0;
            std::printf("%-9s %-14lld %-7ld %-6ld %-5.1f | (no web url) N/sec=%.2f\n",
                        ts.c_str(), raw.edgeNum, raw.mediaSeq, raw.targetDur, raw.incr, nps);
        } else if (!web.ok) {
            std::printf("%-9s %-14lld %-7ld %-6ld %-5.1f | web FETCH FAIL (status=%ld)\n",
                        ts.c_str(), raw.edgeNum, raw.mediaSeq, raw.targetDur, raw.incr, web.status);
        } else {
            double offset = 0;
            if (raw.incr > 0 && raw.targetDur > 0)
                offset = double(web.edgeNum - raw.edgeNum) * raw.targetDur / raw.incr;
            std::printf("%-9s %-14lld %-7ld %-6ld %-5.1f | %-14lld %-7ld | %+.1fs\n",
                        ts.c_str(), raw.edgeNum, raw.mediaSeq, raw.targetDur, raw.incr,
                        web.edgeNum, web.mediaSeq, offset);
            sum += offset; ++n;
        }
        if (i < SAMPLES - 1) Sleep(PERIOD_MS);
    }

    if (n > 0)
        std::printf("\nmean web-ahead offset over %d samples: %+.1fs\n", n, sum / n);
    std::printf("(offset = web rendition live edge minus raw rendition live edge, in seconds.\n"
                " add ~%ds for RE-MOCT's 2-segment prime to estimate total audio lag vs the web player.)\n",
                20);

    InternetCloseHandle(hInet);
    return 0;
}

#endif // _WIN32
