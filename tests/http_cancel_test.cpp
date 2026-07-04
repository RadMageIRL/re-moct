// http_cancel_test — REAL-socket tests of the slice-4 IHttp capabilities that a
// FakeHttp cannot prove, PORTABLE since Phase 3 slice 2 (one file, both matrix
// jobs — a platform shim below swaps winsock/POSIX plumbing; the scenarios and
// assertions are identical against WinINet on Windows and libcurl on Linux):
//   (A) an in-flight fetch aborts PROMPTLY when the cancel atomic flips —
//       chunk-granularity, well under the receive timeout and nowhere near
//       body-completion time (the "stop mid-segment must not hang 8s" gate);
//   (B) a session reuses ONE TCP connection across fetches (keep-alive actually
//       works — WinINet: held InternetOpen handle + KEEP_CONNECTION; libcurl:
//       the CURLSH share-handle connection pool, proven here, not assumed);
//   (C) a pre-cancelled fetch returns immediately and never touches the network.
// Fixture: a plain-HTTP server on 127.0.0.1 (ephemeral port) driven per scenario.
// Plain HTTP is deliberate — urlIsSecureScheme() is false, no TLS machinery.
#include "core/IHttp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// ── platform shim ────────────────────────────────────────────────────────────
#ifdef _WIN32
using SockT = SOCKET;
static constexpr SockT kBadSock = INVALID_SOCKET;
static void closeSock(SockT s)  { closesocket(s); }
static bool initNet()           { WSADATA w; return WSAStartup(MAKEWORD(2,2), &w) == 0; }
static void cleanupNet()        { WSACleanup(); }
static int  sendFlags()         { return 0; }
using SockLenT = int;
static void capSocketWaits(SockT c, unsigned ms = 2000) {
    DWORD v = ms;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
}
#else
using SockT = int;
static constexpr SockT kBadSock = -1;
static void closeSock(SockT s)  { ::close(s); }
static bool initNet()           { signal(SIGPIPE, SIG_IGN); return true; }  // a fixture send
                                  // to a dead client must not kill the test process
static void cleanupNet()        {}
static int  sendFlags()         { return MSG_NOSIGNAL; }   // belt and braces with ^
using SockLenT = socklen_t;
static void capSocketWaits(SockT c, unsigned ms = 2000) {
    struct timeval tv { (time_t)(ms / 1000u), (suseconds_t)((ms % 1000u) * 1000u) };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

static void sleepMs(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Stop a LISTENING socket so a thread blocked in accept() on it wakes up.
// Windows: closesocket() aborts a blocked accept (WSAEINTR) — the baseline
// shape. Linux: close() does NOT wake a blocked accept() (the fd stays
// referenced by the sleeping syscall — first Linux run hung forever in
// server.join()); shutdown() DOES (accept returns EINVAL), so shut down
// first, then close.
static void stopListener(SockT s) {
#ifndef _WIN32
    ::shutdown(s, SHUT_RDWR);
#endif
    closeSock(s);
}

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

using Clock = std::chrono::steady_clock;
static long msSince(Clock::time_point t0) {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// ── fixture ──────────────────────────────────────────────────────────────────
struct Listener {
    SockT sock = kBadSock;
    int   port = 0;
    bool openLocal() {
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kBadSock) return false;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::bind(sock, (sockaddr*)&a, sizeof(a)) != 0) return false;
        if (::listen(sock, 4) != 0) return false;
        SockLenT alen = sizeof(a);
        if (getsockname(sock, (sockaddr*)&a, &alen) != 0) return false;
        port = ntohs(a.sin_port);
        return true;
    }
    ~Listener() { if (sock != kBadSock) closeSock(sock); }
};

// Read from `c` until a blank line terminates the request headers (or the peer dies).
static bool readRequest(SockT c) {
    std::string req; char b[1024];
    for (;;) {
        int n = (int)::recv(c, b, sizeof(b), 0);
        if (n <= 0) return false;
        req.append(b, n);
        if (req.find("\r\n\r\n") != std::string::npos) return true;
    }
}

static bool sendAll(SockT c, const char* p, int len) {
    while (len > 0) {
        int n = (int)::send(c, p, len, sendFlags());
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

int main() {
    if (!initNet()) { std::printf("FAIL initNet\n"); return 1; }

    core::IHttp& http = core::http();

    // ---- (A) prompt mid-body cancel ----------------------------------------
    // Server sends headers for a 10 MB body, then dribbles 1 KB every 30 ms
    // (~300 s to complete). Cancel flips at ~300 ms. A pass proves the abort is
    // chunk-granular: elapsed must be far below the 8 s receive timeout and
    // astronomically below body completion.
    {
        Listener lis; CHECK(lis.openLocal());
        std::atomic<bool> server_done{false};
        std::thread server([&] {
            SockT c = ::accept(lis.sock, nullptr, nullptr);
            if (c == kBadSock) return;
            capSocketWaits(c);
            if (readRequest(c)) {
                const char* hdr = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/octet-stream\r\n"
                                  "Content-Length: 10485760\r\n"
                                  "Connection: keep-alive\r\n\r\n";
                sendAll(c, hdr, (int)strlen(hdr));
                char chunk[1024]; memset(chunk, 'x', sizeof(chunk));
                while (!server_done.load() && sendAll(c, chunk, sizeof(chunk)))
                    sleepMs(30);
            }
            closeSock(c);
        });

        core::HttpSessionConfig cfg; cfg.timeout_ms = 8000;   // mirror the hls session
        auto session = http.openSession(cfg);
        CHECK(session != nullptr);

        // The cancel flag is a plain int32 (the ABI cancel type) accessed via
        // std::atomic_ref on both sides — the conforming way to share a POD int
        // across the (future) C boundary; no std::atomic over the same bytes.
        int32_t stop{0};
        std::thread canceller([&] {
            sleepMs(300);
            std::atomic_ref<int32_t>(stop).store(1, std::memory_order_release);
        });

        core::HttpRequest req;
        req.url    = "http://127.0.0.1:" + std::to_string(lis.port) + "/seg1.aac";
        req.cancel = &stop;
        auto t0  = Clock::now();
        auto res = session->fetch(req);
        long ms  = msSince(t0);

        CHECK(res.cancelled);
        CHECK(res.ok == false);
        CHECK(res.body.empty());          // partial segment cleared, never surfaced
        CHECK(ms < 3000);                 // prompt: not 8 s (timeout), not ~300 s (body)
        std::printf("cancel-mid-body: aborted in %ld ms\n", ms);

        server_done.store(true);
        canceller.join();
        session.reset();                  // close the session -> pooled conn drops -> server unblocks
        server.join();
    }

    // ---- (B) keep-alive: one session, two fetches, ONE connection ----------
    {
        Listener lis; CHECK(lis.openLocal());
        std::atomic<int> connections{0};
        std::atomic<bool> quit{false};
        std::thread server([&] {
            while (!quit.load()) {
                SockT c = ::accept(lis.sock, nullptr, nullptr);
                if (c == kBadSock) break;            // listener closed -> exit
                ++connections;
                capSocketWaits(c);
                // serve any number of sequential requests on this connection
                while (readRequest(c)) {
                    const char* rsp = "HTTP/1.1 200 OK\r\n"
                                      "Content-Type: text/plain\r\n"
                                      "Content-Length: 5\r\n"
                                      "Connection: keep-alive\r\n\r\nhello";
                    if (!sendAll(c, rsp, (int)strlen(rsp))) break;
                }
                closeSock(c);
            }
        });

        core::HttpSessionConfig cfg; cfg.timeout_ms = 8000;
        auto session = http.openSession(cfg);
        CHECK(session != nullptr);

        core::HttpRequest r1; r1.url = "http://127.0.0.1:" + std::to_string(lis.port) + "/a";
        core::HttpRequest r2; r2.url = "http://127.0.0.1:" + std::to_string(lis.port) + "/b";
        auto a = session->fetch(r1);
        auto b = session->fetch(r2);
        CHECK(a.ok && a.status == 200 && a.body == "hello");
        CHECK(b.ok && b.status == 200 && b.body == "hello");
        CHECK(connections.load() == 1);   // the whole point of the session
        std::printf("keep-alive: 2 fetches over %d connection(s)\n", connections.load());

        quit.store(true);
        session.reset();                  // drop the pooled conn -> recv sees EOF
        stopListener(lis.sock); lis.sock = kBadSock;   // unblock accept()
        server.join();
    }

    // ---- (C) pre-cancelled fetch: immediate, zero network ------------------
    {
        Listener lis; CHECK(lis.openLocal());
        std::atomic<int> connections{0};
        std::atomic<bool> quit{false};
        std::thread server([&] {
            while (!quit.load()) {
                SockT c = ::accept(lis.sock, nullptr, nullptr);
                if (c == kBadSock) break;
                ++connections;
                closeSock(c);
            }
        });

        core::HttpSessionConfig cfg; cfg.timeout_ms = 8000;
        auto session = http.openSession(cfg);
        CHECK(session != nullptr);

        int32_t stop{1};                  // already stopped before the call
        core::HttpRequest req;
        req.url    = "http://127.0.0.1:" + std::to_string(lis.port) + "/never";
        req.cancel = &stop;
        auto t0  = Clock::now();
        auto res = session->fetch(req);
        long ms  = msSince(t0);

        CHECK(res.cancelled && !res.ok && res.body.empty());
        CHECK(ms < 500);                  // never blocked on the network
        sleepMs(100);                     // give a phantom connection time to show up
        CHECK(connections.load() == 0);   // the network was never touched

        quit.store(true);
        stopListener(lis.sock); lis.sock = kBadSock;
        server.join();
    }

    cleanupNet();
    if (!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
