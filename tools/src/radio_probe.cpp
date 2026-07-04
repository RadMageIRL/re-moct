// radio_probe.cpp — standalone WinINet radio-stream probe.
//
// Mirrors StreamSource::connect() EXACTLY (same User-Agent, 8s timeouts, the same
// RELOAD|NO_CACHE_WRITE|PRAGMA_NOCACHE|KEEP_CONNECTION flags, and the same
// "Icy-MetaData: 1" request header) — but with NO decoder, NO ring buffer, NO UI,
// NO scrobbler, NO Discord. Just raw bytes off the socket, timed.
//
// Purpose: answer one question cleanly — does the station periodically CLOSE the
// connection (the 30-60s drop), independent of anything in RE-MOCT? Because this
// tool shares none of RE-MOCT's playback code, if it reproduces the same cadence
// the cause is upstream of everything we built.
//
// Build (MSYS2 ucrt64):
//     g++ -std=c++20 radio_probe.cpp -o radio_probe.exe -lwininet
// Run (use the SAME station that drops in RE-MOCT; 180s default):
//     ./radio_probe.exe "http://the-station-url/stream"
//     ./radio_probe.exe "http://the-station-url/stream" 300
//
#include <windows.h>
#include <wininet.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <chrono>

using clk = std::chrono::steady_clock;
static clk::time_point g_t0;
static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - g_t0).count();
}

static HINTERNET g_inet = nullptr, g_conn = nullptr;
static DWORD     g_recv_to = 8000;   // receive timeout (ms) — overridable via argv[3]

// ── Identical connect path to StreamSource::connect() ────────────────────────
static bool probe_connect(const std::string& url) {
    g_inet = InternetOpenA("RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)",
                           INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!g_inet) return false;

    DWORD ct = 8000;            // connect timeout stays tight (fast fail on connect)
    InternetSetOptionA(g_inet, INTERNET_OPTION_CONNECT_TIMEOUT, &ct, sizeof(ct));
    InternetSetOptionA(g_inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &g_recv_to, sizeof(g_recv_to));
    InternetSetOptionA(g_inet, INTERNET_OPTION_SEND_TIMEOUT,    &ct, sizeof(ct));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION;
    const char* icy_hdr = "Icy-MetaData: 1\r\n";
    g_conn = InternetOpenUrlA(g_inet, url.c_str(), icy_hdr, (DWORD)-1L, flags, 0);
    if (!g_conn) {
        InternetCloseHandle(g_inet); g_inet = nullptr;
        return false;
    }

    // Report what the server told us — useful for spotting redirects, wrong
    // content-type, chunked transfer, or a low bitrate.
    char  qbuf[512]; DWORD qlen; DWORD qidx;
    DWORD code = 0, clen = sizeof(code); qidx = 0;
    if (HttpQueryInfoA(g_conn, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, &qidx))
        printf("    HTTP status   : %lu\n", code);

    qlen = sizeof(qbuf); qidx = 0; qbuf[0] = 0;
    if (HttpQueryInfoA(g_conn, HTTP_QUERY_CONTENT_TYPE, qbuf, &qlen, &qidx))
        printf("    Content-Type  : %s\n", qbuf);

    std::strcpy(qbuf, "icy-br"); qlen = sizeof(qbuf); qidx = 0;
    if (HttpQueryInfoA(g_conn, HTTP_QUERY_CUSTOM, qbuf, &qlen, &qidx))
        printf("    icy-br (kbps) : %s\n", qbuf);

    std::strcpy(qbuf, "icy-metaint"); qlen = sizeof(qbuf); qidx = 0; int metaint = 0;
    if (HttpQueryInfoA(g_conn, HTTP_QUERY_CUSTOM, qbuf, &qlen, &qidx)) metaint = atoi(qbuf);
    printf("    icy-metaint   : %d\n", metaint);

    std::strcpy(qbuf, "Server"); qlen = sizeof(qbuf); qidx = 0;
    if (HttpQueryInfoA(g_conn, HTTP_QUERY_CUSTOM, qbuf, &qlen, &qidx))
        printf("    Server        : %s\n", qbuf);

    std::strcpy(qbuf, "Transfer-Encoding"); qlen = sizeof(qbuf); qidx = 0;
    if (HttpQueryInfoA(g_conn, HTTP_QUERY_CUSTOM, qbuf, &qlen, &qidx))
        printf("    Transfer-Enc  : %s\n", qbuf);

    return true;
}

static void probe_close() {
    if (g_conn) { InternetCloseHandle(g_conn); g_conn = nullptr; }
    if (g_inet) { InternetCloseHandle(g_inet); g_inet = nullptr; }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: radio_probe.exe <stream-url> [run_seconds] [recv_timeout_ms]\n");
        return 1;
    }
    std::string url = argv[1];
    int run_secs = (argc >= 3) ? atoi(argv[2]) : 180;
    if (argc >= 4) g_recv_to = (DWORD)atoi(argv[3]);   // receive timeout ms (default 8000)

    printf("=== RE-MOCT radio probe (no decoder / ring / UI) ===\n");
    printf("url          : %s\n", url.c_str());
    printf("run          : %ds\n", run_secs);
    printf("recv timeout : %lums%s\n\n", (unsigned long)g_recv_to,
           g_recv_to == 8000 ? " (RE-MOCT's current value)" : " (test value)");

    g_t0 = clk::now();
    const long long run_ms = (long long)run_secs * 1000;

    int       connect_num = 0;
    int       closes      = 0;
    long long first_close_ms = -1, last_close_ms = -1;
    long long sum_conn_dur = 0;     // for average connection lifetime

    while (now_ms() < run_ms) {
        printf("[%7lldms] connect #%d ...\n", now_ms(), connect_num);
        if (!probe_connect(url)) {
            printf("[%7lldms] CONNECT FAILED (GetLastError=%lu)\n", now_ms(), GetLastError());
            probe_close();
            Sleep(1000);
            continue;
        }
        long long conn_ms = now_ms();
        printf("[%7lldms] connected #%d, reading...\n", conn_ms, connect_num);
        connect_num++;

        uint8_t   buf[16384];
        uint64_t  bytes_conn = 0, bytes_window = 0;
        long long last_report = conn_ms;
        bool      ended = false;

        while (now_ms() < run_ms) {
            DWORD got = 0;
            BOOL  ok  = InternetReadFile(g_conn, buf, sizeof(buf), &got);
            long long t = now_ms();

            if (!ok) {
                printf("[%7lldms] *** READ ERROR (GetLastError=%lu) after %llu bytes / %.1fs ***\n",
                       t, GetLastError(), bytes_conn, (t - conn_ms) / 1000.0);
                ended = true; break;
            }
            if (got == 0) {
                printf("[%7lldms] *** SERVER CLOSED CONNECTION after %llu bytes / %.1fs ***\n",
                       t, bytes_conn, (t - conn_ms) / 1000.0);
                ended = true; break;
            }
            bytes_conn   += got;
            bytes_window += got;

            if (t - last_report >= 5000) {
                double secs = (t - last_report) / 1000.0;
                double kbps = (bytes_window * 8.0 / 1000.0) / secs;
                printf("[%7lldms]     alive: %.0f kbit/s, %llu bytes this conn\n",
                       t, kbps, bytes_conn);
                last_report = t; bytes_window = 0;
            }
        }

        long long dur = now_ms() - conn_ms;
        probe_close();

        if (ended) {
            closes++;
            sum_conn_dur += dur;
            if (first_close_ms < 0) first_close_ms = now_ms();
            if (last_close_ms >= 0)
                printf("            (interval since previous close: %.1fs)\n",
                       (now_ms() - last_close_ms) / 1000.0);
            last_close_ms = now_ms();
            printf("            --> connection #%d lasted %.1fs, %llu bytes\n\n",
                   connect_num - 1, dur / 1000.0, bytes_conn);
        }
    }

    printf("\n=== SUMMARY ===\n");
    printf("connections opened : %d\n", connect_num);
    printf("server closes      : %d\n", closes);
    if (closes > 0) {
        printf("avg connection life: %.1fs\n", (sum_conn_dur / 1000.0) / closes);
        printf("\n>>> The station CLOSED the connection %d time(s) during the run.\n", closes);
        printf(">>> This probe shares NONE of RE-MOCT's playback code (no decoder, ring,\n");
        printf(">>> or UI), so the drops are SERVER-SIDE — not something RE-MOCT introduced.\n");
        printf(">>> RE-MOCT's job is only how gracefully it masks them.\n");
    } else {
        printf("\n>>> No server closes observed; the socket stayed up the whole run.\n");
        printf(">>> If RE-MOCT still drops on this station, the cause is DOWNSTREAM of the\n");
        printf(">>> connection (decode / ring / underrun), which refocuses the hunt.\n");
    }
    return 0;
}
