// IpcWinPipe.cpp — Windows named-pipe implementation of core::IIpc.
//
// Lives in src/platform/win/ (header in include/core/IIpc.h) — built INTO the
// core/platform boundary established at slice 5, not relocated later. Compiled only
// on Windows (guarded in CMakeLists via if(WIN32)).
//
// Faithful transport-only extraction of DiscordRP's baseline pipe code (slice 6):
//   connect  — CreateFileW on \\.\pipe\<endpoint>, GENERIC_READ|WRITE, no sharing,
//              OPEN_EXISTING; INVALID_HANDLE_VALUE → nullptr (endpoint absent).
//   send     — one WriteFile for the whole buffer; short write = failure (Discord's
//              framing breaks if header and payload arrive in separate writes).
//   waitReadable — PeekNamedPipe polled every 10 ms (the baseline's granularity)
//              until >= min_bytes are available or the timeout elapses.
//   recvSome — one ReadFile; partial reads are the caller's loop to drive.
//   teardown — CloseHandle only; NO goodbye frame (baseline never sent Discord a
//              CLOSE opcode — stay faithful, not polite).
// Protocol (framing, endpoint probing, handshake, reconnect) stays in DiscordRP.
#ifdef _WIN32

#include "core/IIpc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace platform::win {

class WinPipeChannel final : public core::IIpcChannel {
public:
    explicit WinPipeChannel(HANDLE h) : h_(h) {}
    ~WinPipeChannel() override { CloseHandle(h_); }

    bool send(const void* data, std::size_t len) override {
        DWORD wrote = 0;
        return WriteFile(h_, data, (DWORD)len, &wrote, nullptr) && wrote == len;
    }

    bool waitReadable(std::size_t min_bytes, int timeout_ms) override {
        const int step = 10;   // baseline poll granularity (DiscordRP::waitReadable)
        for (int waited = 0; waited <= timeout_ms; waited += step) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr))
                return false;                    // pipe closed/broken
            if (avail >= min_bytes) return true;
            Sleep(step);
        }
        return false;
    }

    bool recvSome(void* out, std::size_t max_len, std::size_t& got) override {
        DWORD g = 0;
        if (!ReadFile(h_, out, (DWORD)max_len, &g, nullptr)) { got = 0; return false; }
        got = g;
        return true;
    }

private:
    HANDLE h_;
};

class WinPipeIpc final : public core::IIpc {
public:
    std::unique_ptr<core::IIpcChannel> connect(const std::string& endpoint) override {
        // Logical name -> \\.\pipe\<name>. Endpoint names in play are ASCII
        // ("discord-ipc-N"); widen byte-wise.
        std::wstring path = L"\\\\.\\pipe\\";
        for (unsigned char c : endpoint) path += (wchar_t)c;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;   // not running / not listening
        return std::make_unique<WinPipeChannel>(h);
    }
};

} // namespace platform::win

namespace core {

// Production default transport (see IIpc.h — reached by name because core code
// can't include this TU's headers). Function-local static: thread-safe init;
// WinPipeIpc is stateless, channels own their handles.
IIpc& ipc() {
    static platform::win::WinPipeIpc instance;
    return instance;
}

} // namespace core

#endif // _WIN32
