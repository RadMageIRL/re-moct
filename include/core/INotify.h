// INotify.h — platform-neutral user-notification seam (Phase 1 slice 7).
//
// `core::INotify` is the interface; `platform::win::WinToastNotify`
// (src/platform/win/NotifyWinToast.cpp) is the Windows PowerShell-toast
// implementation. The Linux sibling (Phase 3, src/platform/linux/) is
// libnotify / notify-send — whose native surface is exactly (title, body).
//
// The abstraction is "show the user a transient notification" — NOT "run a
// PowerShell toast". Content stays consumer-side: the track/status → title/body
// mapping (artist-prepends-title, the "RE-MOCT" body fallback) lives in Toast.h,
// the consumer-side content adapter — the same content/transport split as IHttp
// (signing/parsing stayed in consumers) and IIpc (protocol stayed in DiscordRP).
// Only the delivery mechanism lives behind this seam.
//
// This header must NOT include <windows.h> or any platform header — that is the
// "does core stay portable" seam test.
#pragma once
#include <string>

namespace core {

// One transient user notification, title over body. Contract (the baseline's,
// preserved): fire-and-forget — MUST NOT block the caller (call sites are on the
// UI thread; the Windows impl spawns its PowerShell process from a detached
// thread), best-effort with NO error reporting (the baseline swallows spawn
// failure), callable from any thread (the impl is stateless per call). No icon
// or duration parameters: the baseline toast never set either — the interface
// models what the one real consumer does, not a notification framework.
class INotify {
public:
    virtual ~INotify() = default;
    virtual void notify(const std::string& title, const std::string& body) = 0;
};

// Link-time bridge to the platform impl (defined in the impl TU): core code
// cannot #include a platform header, so the production default is reached by
// name. Deliberately NOT the http()/setHttp() transitional-global pattern —
// there is no setNotify(): the single consumer (UIManager) takes an INotify*
// by CONSTRUCTOR INJECTION (tests pass a fake there), and this accessor only
// supplies its production default. Named notifier() — not notify() — to avoid
// the core::notify().notify(...) stutter. Endgame unchanged: the host hands
// consumers/plugins their services (docs/architecture.md).
INotify& notifier();

} // namespace core
