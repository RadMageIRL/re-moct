// NotifyNotifySend.cpp — Linux notify-send implementation of core::INotify
// (Phase 3 slice 5; design ratified in docs/phase3-slice5-design.md).
//
// The platform::win::WinToastNotify sibling. Same shape as the frozen Windows
// impl (NotifyWinToast.cpp): spawn the OS notifier as an external process,
// DETACHED, fire-and-forget, reap on a throwaway thread so the UI thread never
// blocks. Where Windows runs `powershell -EncodedCommand <base64 UTF-16LE>`,
// Linux runs `notify-send -a RE-MOCT -- <title> <body>` via fork()+execvp().
//
// Injection safety — the reason this is fork+execvp and NOT std::system():
//   * NO shell. title/body are argv slots handed straight to notify-send;
//     quotes/apostrophes/semicolons/accents in track metadata are inert data,
//     never interpreted. This is the Linux analog of the Windows base64
//     -EncodedCommand fix (no outer command line for metadata to break out of),
//     solved one level lower — there is simply no shell to escape for.
//   * The leading "--" (see NotifyArgv.h) stops a dash-leading title being read
//     as a notify-send option (the argv-level twin of the quote-injection case).
// The argv construction lives in NotifyArgv.h so notify_argv_test can prove it
// headless; only the spawn lives here (transport, live-gate-proven like Windows).
//
// Daemon availability (documented contract): a rendered toast needs a running
// notification daemon owning org.freedesktop.Notifications (dunst, GNOME, KDE,
// xfce4-notifyd). Headless WSL2/CI/SSH has none — notify-send exits non-zero and
// nothing appears. We SWALLOW that (INotify is best-effort, no error reporting —
// the same contract the Windows impl honors when CreateProcess fails); no crash,
// no hang (we only reap a fast-failing child, we never wait on the daemon).
#ifdef __linux__

#include "core/INotify.h"
#include "NotifyArgv.h"

#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace platform::lnx {

// Fire-and-forget spawn of `notify-send -a RE-MOCT -- <title> <body>`, detached
// so the UI thread returns immediately (the WaitForSingleObject-on-a-detached-
// thread twin). fork()+execvp(): the child, between fork and exec, calls only
// execvp/_exit — the only safe acts after forking a multithreaded process;
// everything non-trivial (building argv) happens BEFORE the fork. The detached
// thread waitpid()s its own child so no zombie survives.
static void spawnNotify(const std::string& title, const std::string& body) {
    std::thread([title, body]() {
        std::vector<std::string> args = buildNotifyArgv(title, body);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        pid_t pid = ::fork();
        if (pid == 0) {
            // Child: replace with notify-send. execvp searches PATH. On any
            // failure (notify-send not installed) _exit immediately — the parent
            // thread reaps it and the notification is silently dropped, exactly
            // the best-effort contract.
            ::execvp(argv[0], argv.data());
            _exit(127);
        }
        if (pid > 0) {
            int status = 0;
            ::waitpid(pid, &status, 0);   // reap; result swallowed by design
        }
        // pid < 0 (fork failed): nothing to reap, notification dropped. Contract.
    }).detach();
}

// Thin adapter: the seam surface over the spawn above. Stateless — each notify()
// copies everything its detached thread needs.
class NotifySendNotify final : public core::INotify {
public:
    void notify(const std::string& title, const std::string& body) override {
        spawnNotify(title, body);
    }
};

} // namespace platform::lnx

namespace core {

// Production default notifier (see INotify.h — reached by name because core code
// can't include this TU's headers). Function-local static: thread-safe init;
// NotifySendNotify is stateless, each notify() spawns its own detached child.
INotify& notifier() {
    static platform::lnx::NotifySendNotify instance;
    return instance;
}

} // namespace core

#endif // __linux__
