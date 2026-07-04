// ipc_echo_test — REAL-transport test of the core::IIpc seam (Phase 3 slice 4).
//
// discord_ipc_test proves the PROTOCOL through an injected FakeIpc; this test
// proves the TRANSPORT: a real local server (Windows: CreateNamedPipe fixture;
// Linux: a Unix-domain-socket listener) driven through the REAL platform impl
// via core::ipc(). One file, both platforms — identical scenarios and
// assertions against both transports IS the parity check (the http_cancel_test
// pattern). Run green against the UNTOUCHED Windows impl BEFORE the Linux twin
// landed (baseline-first, the slice-3 pattern).
//
// Scenarios (each on a fresh endpoint/connection — fixture state is owned by
// the fixture, never by the object the client discards):
//   S0  absent endpoint            -> connect() == nullptr
//   S1  16-byte round-trip echo    -> byte-identical
//   S2  64 KiB round-trip echo     -> whole-buffer send + multi-recvSome drain
//                                     with exact byte accounting
//   S3  split delivery             -> waitReadable(min_bytes) semantics: header
//                                     alone does NOT satisfy a body-sized wait;
//                                     the full body does
//   S4  silent server              -> waitReadable times out (bounded, false)
//   S5  buffered-then-close        -> queued bytes count toward min_bytes and
//                                     drain after peer close; then EOF -> false
//   S6  send to a dead peer        -> false WITH THE PROCESS ALIVE (on Linux
//                                     this is the MSG_NOSIGNAL regression case:
//                                     without the flag, SIGPIPE kills us here)
//   S7  (Linux only) discovery     -> flatpak/snap subdir candidates + the
//                                     TMPDIR env fallback resolve
#include "core/IIpc.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cerrno>
#include <cstdlib>
#include <unistd.h>
#endif

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

static void msleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── per-platform server fixture ─────────────────────────────────────────────
// listen() creates the endpoint BEFORE the client connects; run() accepts one
// connection on a fixture thread and hands it to the scenario script. The
// script talks raw OS handles — the CLIENT side is what goes through the seam.
struct ServerConn {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool readN(void* buf, std::size_t n) {          // loop until n bytes read
        std::size_t total = 0;
        while (total < n) {
#ifdef _WIN32
            DWORD got = 0;
            if (!ReadFile(h, (char*)buf + total, (DWORD)(n - total), &got, nullptr) || got == 0)
                return false;
#else
            ssize_t got = ::read(fd, (char*)buf + total, n - total);
            if (got <= 0) { if (got < 0 && errno == EINTR) continue; return false; }
#endif
            total += (std::size_t)got;
        }
        return true;
    }
    bool writeAll(const void* buf, std::size_t n) {
#ifdef _WIN32
        DWORD wrote = 0;
        return WriteFile(h, buf, (DWORD)n, &wrote, nullptr) && wrote == n;
#else
        std::size_t total = 0;
        while (total < n) {
            ssize_t w = ::send(fd, (const char*)buf + total, n - total, MSG_NOSIGNAL);
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; return false; }
            total += (std::size_t)w;
        }
        return true;
#endif
    }
    void closeConn() {
#ifdef _WIN32
        // CloseHandle WITHOUT DisconnectNamedPipe: Disconnect discards bytes the
        // client hasn't read yet, and S5 asserts exactly those bytes survive.
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
#else
        if (fd >= 0) { ::close(fd); fd = -1; }
#endif
    }
};

struct EchoServer {
    std::string name;                                // logical endpoint name
    std::thread th;
#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
#else
    int         listen_fd = -1;
    std::string path;                                // bound socket path (for unlink)
#endif

    // Create the endpoint under `dir` (Linux; ignored on Windows — pipes have
    // one namespace). Must succeed before the client's connect().
    bool listen(const std::string& dir) {
#ifdef _WIN32
        (void)dir;
        std::string p = "\\\\.\\pipe\\" + name;
        pipe = CreateNamedPipeA(p.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                1, 1 << 16, 1 << 16, 0, nullptr);
        return pipe != INVALID_HANDLE_VALUE;
#else
        path = dir + "/" + name;
        ::unlink(path.c_str());
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (path.size() >= sizeof(sa.sun_path)) return false;
        std::strcpy(sa.sun_path, path.c_str());
        if (::bind(listen_fd, (sockaddr*)&sa, sizeof(sa)) != 0) return false;
        return ::listen(listen_fd, 1) == 0;
#endif
    }

    void run(std::function<void(ServerConn&)> script) {
        th = std::thread([this, script]() {
            ServerConn c;
#ifdef _WIN32
            if (!ConnectNamedPipe(pipe, nullptr) &&
                GetLastError() != ERROR_PIPE_CONNECTED) return;
            c.h = pipe;
            script(c);
            c.closeConn();                 // closes the pipe handle itself
            pipe = INVALID_HANDLE_VALUE;
#else
            c.fd = ::accept(listen_fd, nullptr, nullptr);
            if (c.fd < 0) return;
            script(c);
            c.closeConn();
#endif
        });
    }

    void finish() {                        // join + tear the endpoint down
        if (th.joinable()) th.join();
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; }
#else
        if (listen_fd >= 0) { ::close(listen_fd); listen_fd = -1; }
        if (!path.empty()) ::unlink(path.c_str());
#endif
    }
};

static std::string pattern(std::size_t n) {          // deterministic test bytes
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = (char)('A' + (i * 7 + i / 251) % 23);
    return s;
}

static std::string uniq(const char* tag, int n) {
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    return "remoct-echo-" + std::to_string(pid) + "-" + tag + "-" + std::to_string(n);
}

// Drain exactly `n` bytes through recvSome (the consumer's body-loop shape —
// DiscordRP drives partial reads exactly like this) and count the calls.
static bool drainExact(core::IIpcChannel& ch, std::string& out, std::size_t n, int& calls) {
    out.assign(n, '\0');
    std::size_t total = 0;
    calls = 0;
    while (total < n) {
        std::size_t got = 0;
        if (!ch.recvSome(&out[total], n - total, got) || got == 0) return false;
        total += got;
        ++calls;
    }
    return true;
}

int main() {
    std::string dir;                                  // Linux: the discovery base dir
#ifndef _WIN32
    char tmpl[] = "/tmp/remoct-ipc-test-XXXXXX";
    if (!mkdtemp(tmpl)) { std::printf("FAIL: mkdtemp\n"); return 1; }
    dir = tmpl;
    // The impl resolves endpoints against the env per connect(); point the
    // whole discovery chain at our sandbox.
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
#endif
    int scenario = 0;                                 // unique endpoint per scenario

    // ---- S0: absent endpoint -> nullptr ("Discord not on this slot") ----
    {
        auto ch = core::ipc().connect(uniq("absent", ++scenario));
        CHECK(ch == nullptr);
    }

    // ---- S1: 16-byte round-trip echo, byte-identical ----
    {
        EchoServer srv; srv.name = uniq("echo16", ++scenario);
        CHECK(srv.listen(dir));
        const std::string msg = pattern(16);
        srv.run([&](ServerConn& c) {
            std::string buf(16, '\0');
            if (c.readN(&buf[0], 16)) c.writeAll(buf.data(), 16);
        });
        auto ch = core::ipc().connect(srv.name);
        CHECK(ch != nullptr);
        if (ch) {
            CHECK(ch->send(msg.data(), msg.size()));
            CHECK(ch->waitReadable(16, 2000));
            std::string back; int calls = 0;
            CHECK(drainExact(*ch, back, 16, calls));
            CHECK(back == msg);
        }
        srv.finish();
    }

    // ---- S2: 64 KiB round-trip — whole-buffer send, multi-recvSome drain ----
    {
        const std::size_t N = 64 * 1024;
        EchoServer srv; srv.name = uniq("echo64k", ++scenario);
        CHECK(srv.listen(dir));
        const std::string msg = pattern(N);
        srv.run([&](ServerConn& c) {
            std::string buf(N, '\0');
            if (c.readN(&buf[0], N)) c.writeAll(buf.data(), N);   // read ALL, then echo
        });
        auto ch = core::ipc().connect(srv.name);
        CHECK(ch != nullptr);
        if (ch) {
            CHECK(ch->send(msg.data(), msg.size()));   // one send() for the whole buffer
            std::string back; int calls = 0;
            CHECK(drainExact(*ch, back, N, calls));    // blocking recvSome loop, exact bytes
            CHECK(back == msg);
            CHECK(calls >= 1);                         // partial reads are legal; total must be exact
        }
        srv.finish();
    }

    // ---- S3: split delivery — min_bytes semantics ----
    // Header (8) lands now; body (32) lands ~400ms later. A body-sized wait with
    // only the header queued must return false (avail < min_bytes); the header-
    // sized wait succeeds immediately; the body wait succeeds once it arrives.
    {
        EchoServer srv; srv.name = uniq("split", ++scenario);
        CHECK(srv.listen(dir));
        const std::string hdr = pattern(8), body = pattern(32);
        srv.run([&](ServerConn& c) {
            c.writeAll(hdr.data(), 8);
            msleep(400);
            c.writeAll(body.data(), 32);
            msleep(200);                    // let the client drain before close
        });
        auto ch = core::ipc().connect(srv.name);
        CHECK(ch != nullptr);
        if (ch) {
            CHECK(ch->waitReadable(8, 2000));            // header is enough for 8
            CHECK(!ch->waitReadable(8 + 32, 100));       // 8 queued < 40 wanted -> false
            std::string h; int calls = 0;
            CHECK(drainExact(*ch, h, 8, calls) && h == hdr);
            CHECK(ch->waitReadable(32, 5000));           // body arrives
            std::string b;
            CHECK(drainExact(*ch, b, 32, calls) && b == body);
        }
        srv.finish();
    }

    // ---- S4: silent server — waitReadable timeout is bounded and false ----
    {
        EchoServer srv; srv.name = uniq("silent", ++scenario);
        CHECK(srv.listen(dir));
        srv.run([&](ServerConn&) { msleep(800); });      // says nothing, then closes
        auto ch = core::ipc().connect(srv.name);
        CHECK(ch != nullptr);
        if (ch) {
            long long t0 = nowMs();
            CHECK(!ch->waitReadable(8, 300));
            long long dt = nowMs() - t0;
            CHECK(dt >= 250);                            // actually waited (10ms granularity slack)
            CHECK(dt < 2500);                            // ...and returned, not hung
        }
        srv.finish();
    }

    // ---- S5: buffered-then-close — queued bytes survive peer death, then EOF ----
    {
        EchoServer srv; srv.name = uniq("bufclose", ++scenario);
        CHECK(srv.listen(dir));
        const std::string msg = pattern(8);
        srv.run([&](ServerConn& c) {
            c.writeAll(msg.data(), 8);
            // closeConn() runs when the script returns — bytes stay queued
        });
        auto ch = core::ipc().connect(srv.name);
        CHECK(ch != nullptr);
        srv.finish();                                    // server is GONE before we read
        msleep(100);
        if (ch) {
            CHECK(ch->waitReadable(8, 1000));            // queued data counts toward min
            std::string back; int calls = 0;
            CHECK(drainExact(*ch, back, 8, calls) && back == msg);
            CHECK(!ch->waitReadable(1, 200));            // drained + peer dead -> false
            std::size_t got = 123;
            char b[4];
            CHECK(!ch->recvSome(b, sizeof b, got));      // EOF maps to false
            CHECK(got == 0);
        }
    }

    // ---- S6: send to a dead peer -> false, process alive ----
    // THE MSG_NOSIGNAL case on Linux: without the flag the first (or second)
    // send raises SIGPIPE and this test dies instead of failing an assert.
    {
        EchoServer srv; srv.name = uniq("deadpeer", ++scenario);
        CHECK(srv.listen(dir));
        srv.run([&](ServerConn&) {});                    // accept, close immediately
        auto ch = core::ipc().connect(srv.name);
        CHECK(ch != nullptr);
        srv.finish();
        msleep(100);
        if (ch) {
            // A first send can land in OS buffers after the peer closed; the
            // failure must surface within a few attempts — as `false`, not a signal.
            bool failed = false;
            const std::string msg = pattern(64);
            for (int i = 0; i < 20 && !failed; ++i) {
                if (!ch->send(msg.data(), msg.size())) failed = true;
                else msleep(25);
            }
            CHECK(failed);
        }
        std::printf("S6: survived dead-peer send (no SIGPIPE)\n");
    }

#ifndef _WIN32
    // ---- S7 (Linux only): endpoint discovery — sandbox subdirs + env fallback ----
    {
        // flatpak: <base>/app/com.discordapp.Discord/<name>
        std::string fp = dir + "/app/com.discordapp.Discord";
        ::mkdir((dir + "/app").c_str(), 0700);
        ::mkdir(fp.c_str(), 0700);
        EchoServer srv; srv.name = uniq("flatpak", ++scenario);
        CHECK(srv.listen(fp));
        srv.run([&](ServerConn&) { msleep(50); });
        auto ch = core::ipc().connect(srv.name);         // plain path absent -> flatpak hit
        CHECK(ch != nullptr);
        ch.reset();
        srv.finish();

        // snap: <base>/snap.discord/<name>
        std::string sp = dir + "/snap.discord";
        ::mkdir(sp.c_str(), 0700);
        EchoServer srv2; srv2.name = uniq("snap", ++scenario);
        CHECK(srv2.listen(sp));
        srv2.run([&](ServerConn&) { msleep(50); });
        auto ch2 = core::ipc().connect(srv2.name);
        CHECK(ch2 != nullptr);
        ch2.reset();
        srv2.finish();

        // env fallback: XDG_RUNTIME_DIR unset -> TMPDIR is the base
        unsetenv("XDG_RUNTIME_DIR");
        setenv("TMPDIR", dir.c_str(), 1);
        EchoServer srv3; srv3.name = uniq("tmpdir", ++scenario);
        CHECK(srv3.listen(dir));
        srv3.run([&](ServerConn&) { msleep(50); });
        auto ch3 = core::ipc().connect(srv3.name);
        CHECK(ch3 != nullptr);
        ch3.reset();
        srv3.finish();
        setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);       // restore
    }
#endif

    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("ipc_echo_test: all checks passed\n");
    return 0;
}
