// SeamStubs.cpp — Phase 3 slice 0: inert Linux placeholders for the four platform
// seams, so the Linux build has definitions for every core:: link-time bridge from
// day one and the CI matrix compiles this directory from day one.
//
// Each seam's real implementation arrives as its OWN file in its OWN slice and
// removes its stub from here (this file shrinks to nothing and dies with slice 6):
//   slice 2 — HttpCurl.cpp      (libcurl; sessions = held easy handle, cancel token
//                                polled from the progress callback)
//   slice 4 — IpcUnixSocket.cpp ($XDG_RUNTIME_DIR/<name> Unix domain socket)
//   slice 5 — NotifyLibnotify.cpp (notify-send/libnotify, app id "RE-MOCT")
//   slice 6 — CdIoSgIo.cpp      (SG_IO on /dev/srN: READ CD 0xBE, READ TOC 0x43,
//                                TEST UNIT READY, INQUIRY, SET CD SPEED 0xBB)
//
// Stub behavior is the CONTRACT'S failure mode, not an abort: HTTP fetch fails
// (ok=false), IPC/CD connect/open return nullptr ("not listening" / "can't open"),
// notify is a best-effort no-op (the interface swallows failure by design). A
// consumer wired through a stub degrades exactly as it would on a dead network /
// absent Discord / missing drive — behavior the Windows consumers already handle.
//
// Namespace note: `platform::lnx`, not `platform::linux` — `linux` is a predefined
// macro under -std=gnu++ dialects; the tree builds with extensions off today, but
// the name must not break if that ever changes.
#ifdef __linux__

#include "core/IHttp.h"
#include "core/IIpc.h"
#include "core/INotify.h"
#include "core/ICdIo.h"

namespace platform::lnx {

struct StubHttp final : core::IHttp {
    core::HttpResponse fetch(const core::HttpRequest&) override {
        return {};                       // ok=false, status 0 — transport failure
    }
    // openSession: inherited default forwarder is correct for a stub — it forwards
    // to fetch(), which fails, matching "session opened, network dead".
};

struct StubIpc final : core::IIpc {
    std::unique_ptr<core::IIpcChannel> connect(const std::string&) override {
        return nullptr;                  // endpoint not listening
    }
};

struct StubNotify final : core::INotify {
    void notify(const std::string&, const std::string&) override {}   // best-effort no-op
};

struct StubCdIo final : core::ICdIo {
    std::unique_ptr<core::ICdDevice> open(const std::string&) override {
        return nullptr;                  // drive can't be opened
    }
};

} // namespace platform::lnx

namespace core {

// Transitional injection pointer — the exact HttpWinInet.cpp shape, so tests that
// inject via setHttp() behave identically on both platforms.
static IHttp* g_override = nullptr;

void setHttp(IHttp* transport) { g_override = transport; }

IHttp& http() {
    if (g_override) return *g_override;
    static platform::lnx::StubHttp instance;
    return instance;
}

IIpc& ipc() {
    static platform::lnx::StubIpc instance;
    return instance;
}

INotify& notifier() {
    static platform::lnx::StubNotify instance;
    return instance;
}

ICdIo& cdio() {
    static platform::lnx::StubCdIo instance;
    return instance;
}

} // namespace core

#endif // __linux__
