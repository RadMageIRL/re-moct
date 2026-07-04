// SeamStubs.cpp — Phase 3 slice 0: inert Linux placeholders for the platform
// seams still awaiting their real impls, so the Linux build has definitions for
// every core:: link-time bridge.
//
// Each seam's real implementation arrives as its OWN file in its OWN slice and
// removes its stub from here (this file shrinks to nothing and dies with slice 6):
//   slice 2 — HttpCurl.cpp: ✅ LANDED — the HTTP stub + core::http()/setHttp()
//             bridges moved there; libcurl now backs the seam for real.
//   slice 4 — IpcUnixSocket.cpp: ✅ LANDED — the IPC stub + core::ipc() bridge
//             moved there; a Unix domain socket backs the seam for real.
//   slice 5 — NotifyNotifySend.cpp: ✅ LANDED — the notify stub + core::notifier()
//             bridge moved there; notify-send (app id "RE-MOCT") backs it for real.
//   slice 6 — CdIoSgIo.cpp      (SG_IO on /dev/srN: READ CD 0xBE, READ TOC 0x43,
//                                TEST UNIT READY, INQUIRY, SET CD SPEED 0xBB)
//
// Stub behavior is the CONTRACT'S failure mode, not an abort: CD open returns
// nullptr ("can't open"). A consumer wired through a stub degrades exactly as it
// would with a missing drive — behavior the Windows consumers already handle.
//
// Namespace note: `platform::lnx`, not `platform::linux` — `linux` is a predefined
// macro under -std=gnu++ dialects; the tree builds with extensions off today, but
// the name must not break if that ever changes.
#ifdef __linux__

#include "core/ICdIo.h"

namespace platform::lnx {

struct StubCdIo final : core::ICdIo {
    std::unique_ptr<core::ICdDevice> open(const std::string&) override {
        return nullptr;                  // drive can't be opened
    }
};

} // namespace platform::lnx

namespace core {

// (http()/setHttp() live in HttpCurl.cpp since slice 2;
//  ipc() lives in IpcUnixSocket.cpp since slice 4;
//  notifier() lives in NotifyNotifySend.cpp since slice 5.)

ICdIo& cdio() {
    static platform::lnx::StubCdIo instance;
    return instance;
}

} // namespace core

#endif // __linux__
