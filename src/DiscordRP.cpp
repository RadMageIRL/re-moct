#include "DiscordRP.h"
#ifdef _WIN32
#include <vector>
#include <cstring>
#include <ctime>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // GetCurrentProcessId only (activity payload pid) — the pipe
                       // transport lives behind core::IIpc since slice 6

// Discord IPC framing: [opcode u32 LE][length u32 LE][payload bytes].
// Opcodes: 0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG.

bool DiscordRP::ensureConnected() {
    if (connected()) return true;
    if (!channel_) {
        // Discord listens on one of ten slots — probe them in order (protocol
        // knowledge, so it stays here; the seam maps the name to the pipe path).
        for (int i = 0; i <= 9; ++i) {
            channel_ = ipc_->connect("discord-ipc-" + std::to_string(i));
            if (channel_) break;
        }
        if (!channel_) return false;   // Discord not running
    }
    if (!handshaked_) {
        std::string hs = "{\"v\":1,\"client_id\":\"" + app_id_ + "\"}";
        if (!writeFrame(0, hs)) { disconnect(); return false; }
        if (!drainOneFrame())  { disconnect(); return false; }  // READY (or CLOSE)
        handshaked_ = true;
    }
    return true;
}

bool DiscordRP::writeFrame(uint32_t opcode, const std::string& payload) {
    if (!channel_) return false;
    // Header + payload must go out as a single write or the pipe breaks.
    std::vector<char> buf(8 + payload.size());
    uint32_t len = (uint32_t)payload.size();
    std::memcpy(buf.data() + 0, &opcode, 4);
    std::memcpy(buf.data() + 4, &len,    4);
    std::memcpy(buf.data() + 8, payload.data(), payload.size());
    if (!channel_->send(buf.data(), buf.size())) {
        disconnect();
        return false;
    }
    return true;
}

bool DiscordRP::drainOneFrame() {
    // Bounded wait for a full header so a wedged Discord client can never block
    // the UI thread on a read (the seam peeks; nothing is consumed until it's there).
    if (!channel_ || !channel_->waitReadable(8, 1000)) { disconnect(); return false; }
    char hdr[8]; std::size_t got = 0;
    if (!channel_->recvSome(hdr, 8, got) || got != 8) { disconnect(); return false; }
    uint32_t op, len;
    std::memcpy(&op,  hdr + 0, 4);
    std::memcpy(&len, hdr + 4, 4);
    // Sanity cap: Discord IPC responses are a few KB. A garbage/huge length from
    // a process squatting the discord-ipc pipe would otherwise resize() up to 4 GB.
    if (len > (1u << 20)) { disconnect(); return false; }
    std::string body; body.resize(len);
    std::size_t total = 0;
    while (total < len) {
        if (!channel_->recvSome(&body[total], len - total, got) || got == 0) {
            disconnect();
            return false;
        }
        total += got;
    }
    if (op == 2) { disconnect(); return false; }  // CLOSE -> rejected (e.g. logged out)
    return true;
}

std::string DiscordRP::jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

void DiscordRP::setActivity(const std::string& details, const std::string& state,
                            long start_epoch, const std::string& large_image,
                            const std::string& large_text) {
    if (!ensureConnected()) return;

    std::string a = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    a += std::to_string((unsigned long)GetCurrentProcessId());
    a += ",\"activity\":{";
    bool comma = false;
    auto field = [&](const std::string& body) {
        if (comma) a += ",";
        a += body;
        comma = true;
    };
    if (!details.empty()) field("\"details\":\"" + jsonEscape(details) + "\"");
    if (!state.empty())   field("\"state\":\""   + jsonEscape(state)   + "\"");
    if (start_epoch > 0)  field("\"timestamps\":{\"start\":" + std::to_string(start_epoch) + "}");
    if (!large_image.empty()) {
        std::string assets = "\"assets\":{\"large_image\":\"" + jsonEscape(large_image) + "\"";
        if (!large_text.empty()) assets += ",\"large_text\":\"" + jsonEscape(large_text) + "\"";
        assets += "}";
        field(assets);
    }
    a += "}},\"nonce\":\"" + std::to_string((long)time(nullptr)) + "\"}";

    if (writeFrame(1, a)) drainOneFrame();   // best-effort: consume the response
}

void DiscordRP::clearActivity() {
    if (!connected()) return;
    std::string a = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    a += std::to_string((unsigned long)GetCurrentProcessId());
    a += ",\"activity\":null},\"nonce\":\"" + std::to_string((long)time(nullptr)) + "\"}";
    if (writeFrame(1, a)) drainOneFrame();
}

void DiscordRP::disconnect() {
    channel_.reset();      // closes the pipe; no CLOSE frame sent (baseline behavior)
    handshaked_ = false;
}

#endif // _WIN32
