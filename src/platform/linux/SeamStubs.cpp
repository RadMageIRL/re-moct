// SeamStubs.cpp — Phase 3 slice 0: inert Linux placeholders for the platform
// seams still awaiting their real impls, so the Linux build has definitions for
// every core:: link-time bridge.
//
// Each seam's real implementation arrives as its OWN file in its OWN slice and
// removes its stub from here (this file shrinks to nothing and dies with slice 6):
//   slice 2 — HttpCurl.cpp: ✅ LANDED — the HTTP stub + core::http()/setHttp()
//             bridges moved there; libcurl now backs the seam for real.
//   slice 4 — IpcUnixSocket.cpp ($XDG_RUNTIME_DIR/<name> Unix domain socket)
//   slice 5 — NotifyLibnotify.cpp (notify-send/libnotify, app id "RE-MOCT")
//   slice 6 — CdIoSgIo.cpp      (SG_IO on /dev/srN: READ CD 0xBE, READ TOC 0x43,
//                                TEST UNIT READY, INQUIRY, SET CD SPEED 0xBB)
//
// Stub behavior is the CONTRACT'S failure mode, not an abort: IPC/CD
// connect/open return nullptr ("not listening" / "can't open"), notify is a
// best-effort no-op (the interface swallows failure by design). A consumer
// wired through a stub degrades exactly as it would with an absent Discord /
// missing drive — behavior the Windows consumers already handle.
//
// Namespace note: `platform::lnx`, not `platform::linux` — `linux` is a predefined
// macro under -std=gnu++ dialects; the tree builds with extensions off today, but
// the name must not break if that ever changes.
#ifdef __linux__

#include "core/IIpc.h"
#include "core/INotify.h"
#include "core/ICdIo.h"

namespace platform::lnx {

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

// (http()/setHttp() live in HttpCurl.cpp since slice 2.)

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
