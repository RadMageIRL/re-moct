// IpcUnixSocket.cpp — Linux Unix-domain-socket implementation of core::IIpc
// (Phase 3 slice 4; design ratified in docs/phase3-slice4-design.md).
//
// The platform::win::WinPipeIpc sibling — same three primitives, each mapped
// onto the call the interface was shaped for at slice 6:
//   send         — one ::send() for the whole buffer (WriteFile twin). Short
//                  write = failure: Discord's framing breaks on split writes.
//   waitReadable — the Windows peek-poll loop shape verbatim (10 ms
//                  granularity, immediate first check), with poll()+FIONREAD
//                  in PeekNamedPipe's slot.
//   recvSome     — one blocking ::recv(), no timeout (ReadFile twin); callers
//                  bound it with waitReadable where hanging matters.
//   teardown     — ::close() only; NO goodbye frame (baseline behavior).
// Protocol (framing, the discord-ipc-0..9 probe, handshake, reconnect) stays
// in DiscordRP — the same protocol/transport split as every seam.
//
// Named deltas vs the Windows impl (each argued in the design doc):
//   * MSG_NOSIGNAL on send is LOAD-BEARING: a dead peer must surface as
//     `false` (EPIPE) the way WriteFile returns FALSE — without the flag it
//     raises SIGPIPE and kills the process (ipc_echo_test S6 pins this).
//   * EINTR is retried where nothing was transferred: signals (e.g. ncurses'
//     SIGWINCH on a terminal resize) are a failure mode ReadFile/WriteFile
//     don't have; a signal is not a broken channel.
//   * The 10 ms wait slice is a poll(), which wakes EARLY on arriving data
//     where the twin Sleep(10)s blind — latency-only, same bound (the slice-3
//     "accepted-better" class).
#ifdef __linux__

#include "core/IIpc.h"

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace platform::lnx {

class UnixSocketChannel final : public core::IIpcChannel {
public:
    explicit UnixSocketChannel(int fd) : fd_(fd) {}
    ~UnixSocketChannel() override { ::close(fd_); }

    bool send(const void* data, std::size_t len) override {
        for (;;) {
            ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;   // nothing transferred: retry
            // Partial transfer = failure per the contract (caller discards the
            // channel and reconnects) — the WriteFile `wrote == len` twin.
            return n == (ssize_t)len;
        }
    }

    bool waitReadable(std::size_t min_bytes, int timeout_ms) override {
        const int step = 10;   // baseline poll granularity (the WinPipeIpc loop)
        for (int waited = 0; waited <= timeout_ms; waited += step) {
            pollfd p{};
            p.fd = fd_;
            p.events = POLLIN;
            int r = ::poll(&p, 1, 0);
            if (r < 0 && errno != EINTR) return false;
            if (r > 0) {
                if (p.revents & (POLLERR | POLLNVAL)) return false;   // broken channel
                int avail = 0;
                if (::ioctl(fd_, FIONREAD, &avail) != 0) return false;
                // Queued bytes count toward min_bytes even after peer close —
                // Windows peeks succeed until the pipe buffer drains (pinned by
                // ipc_echo_test S5 on the baseline).
                if (avail >= 0 && (std::size_t)avail >= min_bytes) return true;
                if (p.revents & POLLHUP) return false;   // below min and the rest
                                                         // never comes (Windows
                                                         // would spin to timeout —
                                                         // same result, sooner)
                if ((p.revents & POLLIN) && avail == 0) return false;   // EOF
            }
            p.revents = 0;
            (void)::poll(&p, 1, step);   // the Sleep(10) slot — wakes early on data
        }
        return false;
    }

    bool recvSome(void* out, std::size_t max_len, std::size_t& got) override {
        for (;;) {
            ssize_t n = ::recv(fd_, out, max_len, 0);
            if (n < 0 && errno == EINTR) continue;   // a signal is not a broken channel
            if (n <= 0) { got = 0; return false; }   // 0 = EOF -> the ReadFile
                                                     // broken-pipe FALSE twin
            got = (std::size_t)n;
            return true;
        }
    }

private:
    int fd_;
};

class UnixSocketIpc final : public core::IIpc {
public:
    std::unique_ptr<core::IIpcChannel> connect(const std::string& endpoint) override {
        // Logical name -> candidate paths, resolved per call (tests re-point
        // the env). Base dir = the canonical discord-rpc discovery order:
        // $XDG_RUNTIME_DIR -> $TMPDIR -> $TMP -> $TEMP -> /tmp (first SET
        // wins — a root WSL session may have XDG_RUNTIME_DIR unset). Under
        // the base, plain first (a native install wins), then the sandboxed
        // installs' subdirs.
        //
        // NAMED LEAK, accepted at the design review (doc §3): the flatpak/
        // snap subdir strings are Discord INSTALL knowledge riding in the
        // impl — the alternative pushes Unix path shapes into the platform-
        // neutral consumer, or makes Windows probe garbage pipe names.
        const char* base = nullptr;
        for (const char* env : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
            const char* v = std::getenv(env);
            if (v && *v) { base = v; break; }
        }
        const std::string b = base ? base : "/tmp";
        static const char* const kSubdirs[] =
            { "/", "/app/com.discordapp.Discord/", "/snap.discord/" };
        for (const char* sub : kSubdirs) {
            int fd = tryConnect(b + sub + endpoint);
            if (fd >= 0) return std::make_unique<UnixSocketChannel>(fd);
        }
        return nullptr;   // not running / not listening (CreateFileW's
                          // INVALID_HANDLE_VALUE twin) — the consumer probes on
    }

private:
    static int tryConnect(const std::string& path) {
        sockaddr_un sa{};
        if (path.size() >= sizeof(sa.sun_path)) return -1;   // over sun_path: skip
        sa.sun_family = AF_UNIX;
        std::strcpy(sa.sun_path, path.c_str());
        // SOCK_CLOEXEC IS parity: CreateFileW handles are non-inheritable by
        // default, so the channel must not leak into spawned children here either.
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        // Blocking connect — local and instant when a listener exists; ENOENT
        // (no socket file) and ECONNREFUSED (stale file, no listener) both mean
        // "not on this candidate", same outcome as an absent pipe.
        if (::connect(fd, (sockaddr*)&sa, sizeof(sa)) != 0) { ::close(fd); return -1; }
        return fd;
    }
};

} // namespace platform::lnx

namespace core {

// Production default transport (see IIpc.h — reached by name because core code
// can't include this TU's headers). Function-local static: thread-safe init;
// UnixSocketIpc is stateless, channels own their fds.
IIpc& ipc() {
    static platform::lnx::UnixSocketIpc instance;
    return instance;
}

} // namespace core

#endif // __linux__
