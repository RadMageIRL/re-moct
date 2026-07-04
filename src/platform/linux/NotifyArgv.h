// NotifyArgv.h — the notify-send argv builder for the Linux INotify impl
// (Phase 3 slice 5; design in docs/phase3-slice5-design.md).
//
// Pure, header-only, and DELIBERATELY under src/platform/linux/ (not
// include/core/): the notify-send command shape is TRANSPORT knowledge, and the
// content/transport split keeps it out of the platform-neutral core headers —
// exactly as the Discord framing stayed in DiscordRP and the WinINet flags
// stayed in HttpWinInet. Split out from NotifyNotifySend.cpp only so
// notify_argv_test can assert the argv in the net (headless, no daemon, no
// spawn): the argv IS the injection-safety surface. Building an argv vector —
// never a shell string — is what makes track metadata (quotes, apostrophes,
// accents) inert; the leading "--" terminator stops a dash-leading title
// ("-h", "--version") being read as an option. This is the Linux analog of the
// Windows -EncodedCommand no-outer-quoting fix (NotifyWinToast.cpp), solved one
// level lower: there is no shell to escape for.
#pragma once
#include <string>
#include <vector>

namespace platform::lnx {

// The exact notify-send command line, as an argv payload (pre-c_str):
//   notify-send -a RE-MOCT -- <title> <body>
// -a RE-MOCT = app-name attribution (the CreateToastNotifier('RE-MOCT') twin).
// -- terminates option parsing so title/body are ALWAYS the positional
// summary/body, never mis-read as flags. title/body are inserted verbatim — no
// escaping, because execvp() hands them to notify-send as argv slots, not to a
// shell.
inline std::vector<std::string> buildNotifyArgv(const std::string& title,
                                                const std::string& body) {
    return { "notify-send", "-a", "RE-MOCT", "--", title, body };
}

} // namespace platform::lnx
