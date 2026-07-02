#pragma once
// Minimal Discord Rich Presence over the local IPC named pipe. Best-effort:
// every call silently no-ops if Discord isn't running or the client is logged
// out. Synchronous by design — the pipe is local (sub-millisecond) and updates
// fire only on track change, so there's nothing to background; a short read
// timeout guarantees a hung client can't stall the caller.
//
// Transport goes through the core::IIpc seam (slice 6). Discord's PROTOCOL stays
// here, consumer-side: the [opcode u32][len u32][payload] framing, the
// discord-ipc-0..9 endpoint probe, the handshake, the 1 MB response cap, and the
// lazy-reconnect policy — the same protocol/transport split as the IHttp seam.
#ifdef _WIN32
#include "core/IIpc.h"
#include <cstdint>
#include <memory>
#include <string>

class DiscordRP {
public:
    // ipc == nullptr -> the production platform transport (core::ipc()); tests
    // inject a FakeIpc here (constructor injection — no setIpc global).
    explicit DiscordRP(std::string app_id, core::IIpc* ipc = nullptr)
        : app_id_(std::move(app_id)), ipc_(ipc ? ipc : &core::ipc()) {}
    ~DiscordRP() { disconnect(); }

    DiscordRP(const DiscordRP&) = delete;
    DiscordRP& operator=(const DiscordRP&) = delete;

    // Push a now-playing activity. large_image may be a Rich Presence asset key
    // (e.g. "remoct_logo") OR an external image URL; large_text is its tooltip.
    // start_epoch == 0 omits the elapsed timer. Connects/handshakes lazily.
    void setActivity(const std::string& details,
                     const std::string& state,
                     long start_epoch,
                     const std::string& large_image,
                     const std::string& large_text);

    // Clear the activity (stop / toggle off) but keep the connection.
    void clearActivity();

    // Drop the pipe entirely (toggle off / shutdown).
    void disconnect();

    bool connected() const { return channel_ != nullptr && handshaked_; }

private:
    bool ensureConnected();
    bool writeFrame(uint32_t opcode, const std::string& payload);
    bool drainOneFrame();                 // read + discard one response frame
    static std::string jsonEscape(const std::string& s);

    std::string app_id_;
    core::IIpc* ipc_;                     // injected fake in tests, core::ipc() in prod
    std::unique_ptr<core::IIpcChannel> channel_;
    bool        handshaked_ = false;
};

#endif // _WIN32
