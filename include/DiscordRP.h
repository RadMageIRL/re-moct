#pragma once
// Minimal Discord Rich Presence over the local IPC named pipe. Best-effort:
// every call silently no-ops if Discord isn't running or the client is logged
// out. Synchronous by design — the pipe is local (sub-millisecond) and updates
// fire only on track change, so there's nothing to background; a short read
// timeout guarantees a hung client can't stall the caller.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdint>

class DiscordRP {
public:
    explicit DiscordRP(std::string app_id) : app_id_(std::move(app_id)) {}
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

    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE && handshaked_; }

private:
    bool ensureConnected();
    bool writeFrame(uint32_t opcode, const std::string& payload);
    bool drainOneFrame();                 // read + discard one response frame
    bool waitReadable(int timeout_ms);    // bounded wait so we never hang the UI
    static std::string jsonEscape(const std::string& s);

    std::string app_id_;
    HANDLE      pipe_       = INVALID_HANDLE_VALUE;
    bool        handshaked_ = false;
};

#endif // _WIN32
