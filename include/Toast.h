#pragma once
#include <string>
#include "core/INotify.h"

// Consumer-side CONTENT adapter for track/status toasts (Phase 1 slice 7).
// Maps the track shape (title/artist/album) — and the (msg, detail, "") status
// shape most call sites use — onto the seam's (title, body) surface, exactly as
// the baseline Toast.cpp did: artist PREPENDS the title, and an empty album
// falls back to "RE-MOCT" as the body. Delivery (PowerShell toast today,
// libnotify at Phase 3) lives behind core::INotify — this header stays
// platform-free. `out` is the caller's injected notifier (UIManager passes its
// ctor-injected member; tests pass a fake and assert the (title, body) pair
// without spawning anything).
inline void showTrackToast(const std::string& title,
                           const std::string& artist,
                           const std::string& album,
                           core::INotify& out) {
    // Build display strings
    std::string top  = artist.empty() ? title : artist + " - " + title;
    std::string body = album.empty()  ? "RE-MOCT" : album;
    out.notify(top, body);
}
