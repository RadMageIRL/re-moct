// IIpc.h — platform-neutral local-IPC seam (Phase 1 slice 6).
//
// `core::IIpc` is the interface; `platform::win::WinPipeIpc`
// (src/platform/win/IpcWinPipe.cpp) is the Windows named-pipe implementation;
// `platform::lnx::UnixSocketIpc` (src/platform/linux/IpcUnixSocket.cpp, Phase 3
// slice 4) is the Linux Unix-domain-socket sibling.
//
// The abstraction is "a local bidirectional byte channel", modeled on what the one
// real consumer (DiscordRP) needs — NOT a general IPC framework. Protocol stays
// consumer-side: Discord's [opcode][length][payload] framing, the discord-ipc-0..9
// endpoint probe, the handshake, the 1 MB response cap, and reconnect policy all
// live in DiscordRP. Only the raw transport lives behind this seam — the same
// protocol/transport split as IHttp (signing/parsing stayed in consumers).
//
// This header must NOT include <windows.h> or any platform header — that is the
// "does core stay portable" seam test.
#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace core {

// One open bidirectional local IPC connection (Windows: the client end of a named
// pipe; Linux: a connected Unix-domain socket). Destroying the channel closes it —
// and nothing more: no goodbye message (DiscordRP's baseline never sent Discord a
// CLOSE frame on teardown; the seam must not get polite on its behalf).
// A false return from any method means the channel is dead or the data didn't go
// through — callers discard the channel and reconnect from scratch (DiscordRP's
// lazy-reconnect does exactly this).
class IIpcChannel {
public:
    virtual ~IIpcChannel() = default;
    // Write the ENTIRE buffer as one OS write. Atomicity is contract, not
    // optimization: Discord drops the connection when a frame's header and payload
    // arrive in separate writes.
    virtual bool send(const void* data, std::size_t len) = 0;
    // Bounded wait until at least min_bytes are readable (peek — consumes nothing).
    // false = timeout OR broken channel. Exists so a wedged peer can never hang the
    // caller on a blocking read: DiscordRP gates its 8-byte header read on this.
    virtual bool waitReadable(std::size_t min_bytes, int timeout_ms) = 0;
    // One OS read into out (up to max_len; may deliver fewer — got). BLOCKING with
    // no timeout, like the ReadFile/read() it wraps — bound it with waitReadable
    // first where hanging matters. (DiscordRP's body loop deliberately does NOT
    // bound it — baseline behavior, preserved.) false = broken channel.
    virtual bool recvSome(void* out, std::size_t max_len, std::size_t& got) = 0;
};

// Channel factory. connect() takes a LOGICAL endpoint name ("discord-ipc-0"); the
// platform impl owns the name→path mapping — \\.\pipe\<name> on Windows; on Linux
// a CANDIDATE LIST: <base>/<name> with base = $XDG_RUNTIME_DIR (env fallback
// TMPDIR/TMP/TEMP, then /tmp), plus the flatpak/snap install subdirs (see
// IpcUnixSocket.cpp for the named knowledge leak that list carries). The path
// prefix is exactly as platform-specific as the transport itself. Returns nullptr
// when the endpoint doesn't exist / isn't listening (e.g. Discord not running on
// that slot); probing multiple slots is the CONSUMER's protocol knowledge, not
// the seam's.
class IIpc {
public:
    virtual ~IIpc() = default;
    virtual std::unique_ptr<IIpcChannel> connect(const std::string& endpoint) = 0;
};

// Link-time bridge to the platform transport (defined in the impl TU): core code
// cannot #include a platform header, so the production default is reached by name.
// Deliberately NOT the http()/setHttp() transitional-global pattern — there is no
// setIpc(): the single consumer (DiscordRP) takes an IIpc* by CONSTRUCTOR
// INJECTION (tests pass a fake there), and this accessor only supplies its
// production default. Endgame unchanged: the host hands consumers/plugins their
// services (docs/architecture.md).
IIpc& ipc();

} // namespace core
