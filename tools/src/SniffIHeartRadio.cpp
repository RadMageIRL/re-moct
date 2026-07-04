// ─────────────────────────────────────────────────────────────────────────────
// SniffIHeartRadio.cpp  —  standalone iHeart now-playing API prober for RE-MOCT
//
// NOT part of the RE-MOCT build. Compile on demand, point it at an iHeart HLS
// URL, and it walks iHeart's (undocumented) API to discover how to resolve a
// station's now-playing track. It is an ACTIVE PROBER, not a packet sniffer:
// it makes the same HTTP requests the future IHeartRadio module will make and
// dumps every stage's raw + pretty JSON so we can see exactly what iHeart returns.
//
// Two outputs:
//   1. %TEMP%\re-moct-iheartbeat.log        — human-readable trace (for diffing
//                                              when iHeart churns their API)
//   2. %TEMP%\re-moct-iheart-stations.json  — structured identity cache the app
//                                              can later read (zc#### -> id/meta_url)
//
// Build (MSYS2 ucrt64):
//   g++ -std=c++20 SniffIHeartRadio.cpp IHeartRadio.cpp -o SniffIHeartRadio.exe -lwininet
//   (Uses the nlohmann single-header json.hpp sitting next to this .cpp. If it
//    lives elsewhere, add -I<dir-containing-json.hpp>.)
//
// Usage:
//   SniffIHeartRadio.exe -S https://stream.revma.ihrhls.com/zc4366/hls.m3u8   (exhaustive sniff)
//   SniffIHeartRadio.exe -M https://stream.revma.ihrhls.com/zc4366/hls.m3u8   (test IHeartRadio module)
// ─────────────────────────────────────────────────────────────────────────────

#include <windows.h>
#include <wininet.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <fstream>

#include "json.hpp"        // nlohmann single-header (sits next to this .cpp in E:\code)
#include "IHeartRadio.h"   // the production module — -M mode exercises it
using json = nlohmann::json;

// ── Dual logging: stdout + %TEMP%\re-moct-iheartbeat.log ─────────────────────
static std::string g_logpath;

static std::string tempDir() {
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);   // includes trailing backslash
    if (n == 0 || n > MAX_PATH) return ".\\";
    return std::string(buf, n);
}

static void logf(const char* fmt, ...) {
    char buf[8192];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
    if (!g_logpath.empty()) {
        FILE* f = std::fopen(g_logpath.c_str(), "a");
        if (f) { std::fputs(buf, f); std::fputs("\n", f); std::fclose(f); }
    }
}

// Truncate the log if its last-write date isn't today, so it never grows
// unbounded across days. Same-day reruns still append. Call once at startup,
// before any logf().
static void resetLogIfStale(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa))
        return;                                   // no file yet -> logf creates it fresh
    SYSTEMTIME lf{}; FILETIME lft{};
    if (!FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lft) || !FileTimeToSystemTime(&lft, &lf))
        return;                                   // can't read date -> leave as-is
    SYSTEMTIME now{}; GetLocalTime(&now);
    bool same_day = (lf.wYear == now.wYear && lf.wMonth == now.wMonth && lf.wDay == now.wDay);
    if (!same_day) {
        FILE* f = std::fopen(path.c_str(), "w");  // truncate -> fresh day
        if (f) std::fclose(f);
    }
}

static std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    tmv = *std::localtime(&t);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tmv);
    return b;
}

// ── HTTP GET via WinINet ─────────────────────────────────────────────────────
struct HttpResult {
    bool        ok      = false;   // request completed (any status)
    DWORD       status  = 0;       // HTTP status code
    std::string body;              // response body
    std::string final_url;         // post-redirect URL
    DWORD       win_err = 0;       // GetLastError if the request failed to open
};

static HttpResult httpGet(HINTERNET hInet, const std::string& url) {
    HttpResult r;
    // A browser-ish UA + JSON Accept; iHeart's amp API is picky about some of this.
    const char* headers = "Accept: application/json\r\n";
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION |
                  INTERNET_FLAG_SECURE;
    HINTERNET h = InternetOpenUrlA(hInet, url.c_str(), headers, (DWORD)-1L, flags, 0);
    if (!h) { r.win_err = GetLastError(); return r; }

    // Post-redirect URL
    char fbuf[2048]; DWORD flen = sizeof(fbuf);
    if (InternetQueryOptionA(h, INTERNET_OPTION_URL, fbuf, &flen)) r.final_url.assign(fbuf, flen);
    else r.final_url = url;

    // Status code
    DWORD code = 0, clen = sizeof(code), idx = 0;
    if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, &idx))
        r.status = code;

    // Body
    std::string body;
    char chunk[8192]; DWORD got = 0;
    while (InternetReadFile(h, chunk, sizeof(chunk), &got) && got > 0) {
        body.append(chunk, got);
        if (body.size() > 4u * 1024u * 1024u) break;   // sanity cap
    }
    InternetCloseHandle(h);
    r.body = std::move(body);
    r.ok   = true;
    return r;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────
// Recursively find the first string value under any key whose name matches one
// of `keys` (case-insensitive). Returns "" if none found. Lets us spot artist/
// title/name fields regardless of how iHeart nests them.
static std::string lower(std::string s){ for(char&c:s)c=(char)std::tolower((unsigned char)c); return s; }

static std::string findFirstString(const json& j, const std::vector<std::string>& keys) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string k = lower(it.key());
            for (const auto& want : keys)
                if (k == want && it.value().is_string())
                    return it.value().get<std::string>();
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string sub = findFirstString(it.value(), keys);
            if (!sub.empty()) return sub;
        }
    } else if (j.is_array()) {
        for (const auto& e : j) {
            std::string sub = findFirstString(e, keys);
            if (!sub.empty()) return sub;
        }
    }
    return {};
}

// Dump a response: raw (truncated) + pretty JSON if parseable, + heuristic fields.
static void dumpResponse(const HttpResult& r, bool look_for_track) {
    if (!r.ok) { logf("    (request failed to open, WinINet err=%lu)", r.win_err); return; }
    logf("    HTTP %lu   final_url=%s", r.status, r.final_url.c_str());
    logf("    body bytes: %zu", r.body.size());
    // Raw, truncated to keep the log readable
    std::string raw = r.body.substr(0, 1200);
    logf("    raw: %s%s", raw.c_str(), r.body.size() > 1200 ? " ...[truncated]" : "");
    // Try to parse + pretty-print
    json j;
    bool parsed = false;
    try { j = json::parse(r.body); parsed = true; } catch (...) { parsed = false; }
    if (!parsed) { logf("    (body is not valid JSON)"); return; }
    std::string pretty = j.dump(2);
    if (pretty.size() > 3000) pretty = pretty.substr(0, 3000) + "\n    ...[pretty truncated]";
    logf("    pretty:\n%s", pretty.c_str());
    // Heuristic field spotting
    std::string name   = findFirstString(j, {"name","stationname","description","callletters","calllettersleft"});
    if (!name.empty()) logf("    >> spotted station name-ish: \"%s\"", name.c_str());
    if (look_for_track) {
        std::string artist = findFirstString(j, {"artist","artistname","artist_name"});
        std::string title  = findFirstString(j, {"title","tracktitle","songtitle","track_title","song"});
        if (!artist.empty() || !title.empty())
            logf("    >> spotted now-playing: artist=\"%s\" title=\"%s\"",
                 artist.c_str(), title.c_str());
        else
            logf("    >> no artist/title fields spotted");
    }
}

// ── Sidecar identity cache (merge-write) ─────────────────────────────────────
static void writeSidecar(const std::string& zc, long station_id,
                         const std::string& meta_url, const std::string& station_name) {
    std::string path = tempDir() + "re-moct-iheart-stations.json";
    json root = json::object();
    { std::ifstream in(path); if (in) { try { in >> root; } catch (...) { root = json::object(); } } }
    if (!root.is_object()) root = json::object();
    json entry = json::object();
    entry["station_id"]   = station_id;
    entry["meta_url"]     = meta_url;
    if (!station_name.empty()) entry["station_name"] = station_name;
    entry["resolved_at"]  = nowStamp();
    root[zc] = entry;
    std::ofstream out(path, std::ios::trunc);
    if (out) { out << root.dump(2) << "\n"; logf("sidecar written: %s", path.c_str()); }
    else      logf("sidecar WRITE FAILED: %s", path.c_str());
}

// ── Extract the zc#### token and its numeric part from a revma URL ───────────
static bool extractZc(const std::string& url, std::string& zc_out, long& num_out) {
    auto p = url.find("/zc");
    if (p == std::string::npos) return false;
    p += 1;                                  // point at 'z'
    size_t e = p;
    while (e < url.size() && url[e] != '/') ++e;
    zc_out = url.substr(p, e - p);           // e.g. "zc4366"
    std::string digits;
    for (char c : zc_out) if (c >= '0' && c <= '9') digits += c;
    if (digits.empty()) return false;
    num_out = std::strtol(digits.c_str(), nullptr, 10);   // e.g. 4366
    return true;
}

static int runSniff(const std::string& url) {
    logf("==================================================================");
    logf("SniffIHeartRadio  run=%s", nowStamp().c_str());
    logf("input url: %s", url.c_str());

    std::string zc; long sid = 0;
    if (!extractZc(url, zc, sid)) {
        logf("ERROR: could not extract a zc#### token from the URL.");
        return 1;
    }
    logf("extracted token: %s   candidate station_id (numeric part): %ld", zc.c_str(), sid);
    logf("NOTE: whether the zc number equals iHeart's station id is exactly what");
    logf("      this probe is here to confirm -- watch the [1] liveStations result.");

    HINTERNET hInet = InternetOpenA(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RE-MOCT-Probe/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { logf("FATAL: InternetOpenA failed err=%lu", GetLastError()); return 1; }
    DWORD to = 8000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    // Candidate endpoints, best-guess first. Each is dumped regardless of outcome.
    std::string N = std::to_string(sid);
    struct Probe { std::string label; std::string url; bool track; };
    std::vector<Probe> probes = {
        { "[1] liveStations (validates id + station name)",
          "https://api.iheart.com/api/v2/content/liveStations/" + N, false },
        { "[2] currentTrackMeta (the now-playing)",
          "https://api.iheart.com/api/v3/live-meta/stream/" + N + "/currentTrackMeta", true },
        { "[3] trackHistory (recent songs)",
          "https://api.iheart.com/api/v3/live-meta/stream/" + N + "/trackHistory", true },
        { "[4] currentTrackMeta on us.api host",
          "https://us.api.iheart.com/api/v3/live-meta/stream/" + N + "/currentTrackMeta", true },
        { "[5] track-history (alt path spelling)",
          "https://api.iheart.com/api/v3/live-meta/stream/" + N + "/track-history", true },
    };

    std::string good_meta_url, station_name, np_artist, np_title;
    bool id_confirmed = false;

    for (const auto& pr : probes) {
        logf("------------------------------------------------------------------");
        logf("%s", pr.label.c_str());
        logf("    GET %s", pr.url.c_str());
        HttpResult r = httpGet(hInet, pr.url);
        dumpResponse(r, pr.track);

        if (r.ok && r.status == 200) {
            json j; bool parsed = false;
            try { j = json::parse(r.body); parsed = true; } catch (...) {}
            if (parsed) {
                if (!pr.track) {   // station-details probe
                    std::string nm = findFirstString(j, {"name","description"});
                    if (!nm.empty()) { station_name = nm; id_confirmed = true; }
                } else {           // track probe
                    std::string a = findFirstString(j, {"artist","artistname","artist_name"});
                    std::string t = findFirstString(j, {"title","tracktitle","songtitle","track_title","song"});
                    if ((!a.empty() || !t.empty()) && good_meta_url.empty()) {
                        good_meta_url = pr.url; np_artist = a; np_title = t;
                    }
                }
            }
        }
    }

    logf("==================================================================");
    logf("RESULT SUMMARY");
    logf("  token: %s   numeric id tried: %ld", zc.c_str(), sid);
    logf("  station id confirmed via liveStations: %s", id_confirmed ? "YES" : "no");
    if (!station_name.empty()) logf("  station name: %s", station_name.c_str());
    if (!good_meta_url.empty()) {
        logf("  WORKING now-playing endpoint: %s", good_meta_url.c_str());
        logf("  current now-playing: artist=\"%s\" title=\"%s\"",
             np_artist.c_str(), np_title.c_str());
        writeSidecar(zc, sid, good_meta_url, station_name);
    } else {
        logf("  no now-playing endpoint produced artist/title.");
        logf("  -> none of the guessed endpoints worked as-is. The dumps above");
        logf("     show what each returned; we adjust the candidate list from those.");
    }
    logf("log file: %s", g_logpath.c_str());
    logf("==================================================================");

    InternetCloseHandle(hInet);
    return good_meta_url.empty() ? 1 : 0;
}

// ── -M mode: exercise the ACTUAL IHeartRadio module (resolve + poll) ─────────
// Proves the production module compiles and produces correct now-playing, using
// the exact code RE-MOCT will call. Routes the module's diagnostics into the
// same iheartbeat log.
static int runModuleTest(const std::string& url) {
    logf("==================================================================");
    logf("SniffIHeartRadio MODULE TEST (-M)  run=%s", nowStamp().c_str());
    logf("input url: %s", url.c_str());
    logf("isIHeartUrl: %s", IHeartRadio::isIHeartUrl(url) ? "yes" : "no");

    IHeartRadio ihr;
    ihr.setLogger([](const std::string& s){ logf("    %s", s.c_str()); });

    logf("--- resolve() ---");
    bool ok = ihr.resolve(url);
    logf("resolved=%s station_id=%ld name=\"%s\"",
         ok ? "true" : "false", ihr.stationId(), ihr.stationName().c_str());
    if (!ok) { logf("resolve failed; nothing to poll."); return 1; }

    logf("--- pollNowPlaying() ---");
    std::string np = ihr.pollNowPlaying();
    if (np.empty()) logf("now-playing: (empty - ad/talk break, or error; caller shows live stream)");
    else            logf("now-playing: %s", np.c_str());

    logf("sidecar: %s", IHeartRadio::sidecarPath().c_str());
    logf("==================================================================");
    return np.empty() ? 1 : 0;
}

// ── -P mode: dump the HLS variant playlist's EXTINF lines ────────────────────
// Resolves master -> variant (the way StreamSource does) and prints the media
// playlist's per-segment tags, flagging whether iHeart embeds song metadata
// (title=/artist=/song_spot=) inline — a potential second now-playing source
// that, unlike trackHistory, can't freeze without stopping the audio.
static std::string resolveUrl(const std::string& base, const std::string& ref) {
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
    // scheme://host
    size_t sch = base.find("://");
    std::string scheme_host = base;
    if (sch != std::string::npos) {
        size_t slash = base.find('/', sch + 3);
        scheme_host = (slash == std::string::npos) ? base : base.substr(0, slash);
    }
    if (!ref.empty() && ref[0] == '/') return scheme_host + ref;           // root-relative
    size_t lastSlash = base.find_last_of('/');
    std::string dir = (lastSlash == std::string::npos) ? base : base.substr(0, lastSlash + 1);
    return dir + ref;                                                       // path-relative
}

static int runPlaylistDump(const std::string& url) {
    logf("==================================================================");
    logf("SniffIHeartRadio PLAYLIST DUMP (-P)  run=%s", nowStamp().c_str());
    logf("input url: %s", url.c_str());

    HINTERNET hInet = InternetOpenA(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RE-MOCT-Probe/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { logf("InternetOpenA failed err=%lu", GetLastError()); return 2; }
    DWORD to = 8000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    // 1. master
    logf("--- master playlist ---");
    HttpResult m = httpGet(hInet, url);
    if (!m.ok || m.status != 200) {
        logf("master GET failed status=%lu win_err=%lu", m.status, m.win_err);
        InternetCloseHandle(hInet); return 1;
    }
    logf("master final_url: %s", m.final_url.c_str());

    std::string media_url, media_body;
    if (m.body.find("#EXT-X-STREAM-INF") != std::string::npos) {
        // master with variants — take the first variant URI (first non-# line)
        std::string variant;
        size_t i = 0;
        while (i < m.body.size()) {
            size_t e = m.body.find('\n', i);
            if (e == std::string::npos) e = m.body.size();
            std::string line = m.body.substr(i, e - i);
            while (!line.empty() && (line.back()=='\r'||line.back()==' ')) line.pop_back();
            if (!line.empty() && line[0] != '#') { variant = line; break; }
            i = e + 1;
        }
        if (variant.empty()) { logf("no variant URI found in master"); InternetCloseHandle(hInet); return 1; }
        media_url = resolveUrl(m.final_url, variant);
        logf("variant uri: %s", variant.c_str());
        logf("--- media playlist ---");
        logf("media url: %s", media_url.c_str());
        HttpResult v = httpGet(hInet, media_url);
        if (!v.ok || v.status != 200) { logf("variant GET failed status=%lu", v.status); InternetCloseHandle(hInet); return 1; }
        media_body = v.body;
    } else if (m.body.find("#EXTINF") != std::string::npos) {
        logf("(master is already a media playlist)");
        media_body = m.body;
    } else {
        logf("unrecognized playlist (no STREAM-INF, no EXTINF):");
        logf("%s", m.body.substr(0, 1200).c_str());
        InternetCloseHandle(hInet); return 1;
    }

    // 2. print EXTINF lines + scan for embedded metadata attributes
    logf("--- EXTINF lines ---");
    int extinf = 0, with_title = 0, with_artist = 0, with_song = 0, spot_T = 0, spot_F = 0, adctx = 0;
    size_t i = 0;
    while (i < media_body.size()) {
        size_t e = media_body.find('\n', i);
        if (e == std::string::npos) e = media_body.size();
        std::string line = media_body.substr(i, e - i);
        while (!line.empty() && (line.back()=='\r'||line.back()==' ')) line.pop_back();
        if (line.rfind("#EXTINF", 0) == 0) {
            extinf++;
            logf("  %s", line.c_str());
            bool hasTitle  = line.find("title=")  != std::string::npos;
            bool hasArtist = line.find("artist=") != std::string::npos;
            if (hasTitle)  with_title++;
            if (hasArtist) with_artist++;
            bool isSpotT = line.find("song_spot=\\\"T\\\"") != std::string::npos ||
                           line.find("song_spot=\"T\"")     != std::string::npos;
            if (isSpotT) spot_T++;
            if (line.find("song_spot=\\\"F\\\"") != std::string::npos ||
                line.find("song_spot=\"F\"")     != std::string::npos) spot_F++;
            if (line.find("adContext") != std::string::npos) adctx++;
            // REAL song content, mirroring StreamSource::classifyIHeartManifest exactly:
            // a line with title=/artist= is NOT a song if it's flagged as a traffic spot
            // (song_spot="T"), a Spot Block boundary, a zero-length marker, or has a blank
            // artist. KBKS ads carry a REAL artist string (artist="Kroger") so the blank
            // check alone misses them — song_spot="T" is the decisive ad discriminator and
            // must gate here just as `spot != 'T'` does in classifyIHeartManifest. (Z100's
            // "Spot Block End" lines have no song_spot at all, caught by spotBlock/zeroLen.)
            if (hasTitle && hasArtist) {
                bool spotBlock = line.find("Spot Block") != std::string::npos;
                bool zeroLen   = line.find("length=\\\"00:00:00\\\"") != std::string::npos ||
                                 line.find("length=\"00:00:00\"")     != std::string::npos;
                bool blankArtist = line.find("artist=\" \"")  != std::string::npos ||  // single space
                                   line.find("artist=\"\"")   != std::string::npos ||  // empty
                                   line.find("artist=\\\" \\\"") != std::string::npos ||
                                   line.find("artist=\\\"\\\"")  != std::string::npos;
                if (!isSpotT && !spotBlock && !zeroLen && !blankArtist) with_song++;
            }
        }
        i = e + 1;
    }

    logf("------------------------------------------------------------------");
    logf("VERDICT");
    logf("  EXTINF segments: %d   with title=: %d   with artist=: %d", extinf, with_title, with_artist);
    logf("  real song lines: %d   song_spot=T (ads): %d   song_spot=F (songs): %d   adContext: %d",
         with_song, spot_T, spot_F, adctx);
    if (with_song > 0)
        logf("  => RICH: this station embeds REAL song metadata IN THE MANIFEST. Inline now-playing is viable.");
    else if (with_title > 0 && with_artist > 0)
        logf("  => MARKERS-ONLY (this capture): manifest carries title=/artist= but only ad/boundary "
             "markers -- song_spot=\"T\" spots, Spot Block End, blank artist, or zero length. NO song data "
             "right now; trackHistory is the source. (Re-run during music: a rich station will show song lines.)");
    else if (adctx > 0)
        logf("  => BARE+adContext: ads flagged via adContext but NO inline title/artist. Manifest can't supply song names.");
    else
        logf("  => BARE: no inline song metadata. trackHistory JSON is the only now-playing source.");
    logf("==================================================================");
    InternetCloseHandle(hInet);
    return (with_song > 0) ? 0 : 1;
}

int main(int argc, char** argv) {
    g_logpath = tempDir() + "re-moct-iheartbeat.log";
    resetLogIfStale(g_logpath);   // fresh log on a new day; same-day reruns append

    if (argc < 3 || (std::string(argv[1]) != "-S" && std::string(argv[1]) != "-M" && std::string(argv[1]) != "-P")) {
        std::fprintf(stderr,
            "SniffIHeartRadio - iHeart now-playing API tool\n"
            "Usage: %s -S <url>   exhaustive API sniff (explore/diagnose)\n"
            "       %s -M <url>   module test (exercise IHeartRadio resolve+poll)\n"
            "       %s -P <url>   playlist dump (EXTINF lines -- check for inline song metadata)\n"
            "Example: %s -S https://stream.revma.ihrhls.com/zc4366/hls.m3u8\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    std::string url  = argv[2];
    if (mode == "-M") return runModuleTest(url);
    if (mode == "-P") return runPlaylistDump(url);
    return runSniff(url);
}

