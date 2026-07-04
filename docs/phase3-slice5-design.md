# Phase 3 slice 5 — notify: `notify-send` `core::INotify` twin (design, ratified before code)

Ratified by Dos 2026-07-03 (three decisions taken up-front): (1) mechanism =
**notify-send subprocess**, not linked libnotify; (2) the interim Linux cmdline
toast echo (slice-2 `4f0b240`) is **kept** as the graceful-degradation surface;
(3) a Linux-only **`notify_argv_test`** proves argv-safety in the net.

Scope: implement `core::INotify` on Linux via `notify-send` —
`src/platform/linux/NotifyNotifySend.cpp`, `platform::lnx::NotifySendNotify` —
the sibling of `platform::win::WinToastNotify`. The lightest seam in Phase 3:
one method, one new impl file, no relocation. NOT sacred territory — the call
sites are on the UI thread, the live audio loops are untouched by construction.

## 1. Survey — the seam being twinned (file:line)

`core::INotify` (`include/core/INotify.h:29-33`): ONE primitive
`notify(title, body)`. The contract, verbatim from the header: fire-and-forget,
**MUST NOT block the caller** (call sites on the UI thread), best-effort with
**NO error reporting** (the baseline swallows spawn failure), callable from any
thread (stateless per call). No icon/duration — the baseline never set them.

Windows impl `src/platform/win/NotifyWinToast.cpp`:

| Aspect | Windows | Notes |
|---|---|---|
| mechanism | `powershell -NoProfile -NonInteractive -EncodedCommand <b64>` (:87) | spawns the OS notifier as an external process |
| spawn | `CreateProcessA` on a DETACHED `std::thread` (:90-104) | fire-and-forget; UI thread returns immediately |
| reap | `WaitForSingleObject(5000)` + `CloseHandle` (:100-102) | on the throwaway thread, not the UI thread |
| injection safety | UTF-16LE→base64 `-EncodedCommand`: NO outer command line for metadata to break out of (:78-87) | the frozen fix — a stray `"`/CRLF in a network stream tag can't inject |
| failure | `if (pi.hProcess)` guard; nothing else (:99) | swallowed — best-effort contract |

Consumer stays consumer-side and **zero-diff for this slice**: `Toast.h:14-22`
maps track/status → (title, body) (artist PREPENDS title; empty album →
"RE-MOCT"); `UIManager.cpp:142-155` is the member wrapper → adapter →
`notify_->notify(top, body)`; `notify_` is ctor-injected (`UIManager.cpp:95`,
default `&core::notifier()`). Content mapping is platform-neutral and OUT of
scope — only the delivery mechanism moves.

Linux today: `StubNotify` no-ops (`SeamStubs.cpp:31-33`, bridge `:48-51`). This
slice replaces it, per the slice-0 plan (`SeamStubs.cpp:11` names the file).

**The anti-pattern deliberately NOT used:** the xdg-open twin at
`UIManager.cpp:3389` shells out via `std::system("xdg-open '…' &")` — safe only
because auth URLs are clean (its comment says so). Track metadata is NOT clean
(`L'Artist`, `Don't "Stop"`, accents). A `std::system` notify-send would
reintroduce exactly the injection class the Windows `-EncodedCommand` block
fixed. This slice uses `fork()+execvp()` with an argv array — no shell at all.

## 2. Mechanism decision — notify-send (subprocess), NOT linked libnotify

| | notify-send (fork+execvp) — CHOSEN | libnotify (linked) — rejected |
|---|---|---|
| parity w/ Windows | exact twin: spawn the OS notifier, detached, argv-safe | in-process C API — a different paradigm |
| dependencies | ZERO link dep; one CMake source line | `find_package`/pkg-config + glib in the address space |
| blocking | fork+exec+reap on a detached thread | `notify_notification_show()` does a SYNCHRONOUS D-Bus round-trip → STILL needs the detached thread |
| escaping | argv + `--` terminator (no shell) | data args to a C fn |
| headless | notify-send exits non-zero; swallowed | show() returns error; swallowed |

Reasons (each an established project doctrine):
1. **It is the byte-for-byte doctrine twin of the frozen Windows pattern** —
   "spawn an external notifier, detached, argv-safe." Every prior slice kept the
   baseline shape verbatim (slice 3's pull-read loop, slice 4's peek-poll loop);
   this keeps the shell-out shape.
2. **Zero dependency cost**, matching the minimize-deps line already walked
   (vendored single-file MD5 over a crypto lib; module-mode curl over config
   churn). The slice is one `.cpp` + one CMake line, exactly like `IpcUnixSocket`.
3. **libnotify's "more robust" edge is thin** — both require a running daemon
   owning `org.freedesktop.Notifications`; both no-op headless; libnotify STILL
   needs the detached thread because show() blocks. Cost without the saving.
4. **Process-per-notification is already the accepted cost** — Windows spawns a
   whole PowerShell per toast; notify-send is a tiny C binary, strictly cheaper.

## 3. The Linux mapping (parity table — every delta named)

| Aspect | Linux | Deltas vs Windows |
|---|---|---|
| mechanism | `notify-send -a RE-MOCT -- <title> <body>` via `execvp` | `-a RE-MOCT` = the `CreateToastNotifier('RE-MOCT')` app-name attribution (platform attribution, not content — stays impl-side, as the Windows `RE-MOCT` id does). |
| spawn | `fork()` on a detached `std::thread`; child `execvp()`s | direct `CreateProcessA`-on-a-detached-thread twin. argv is built BEFORE the fork; the child calls only `execvp`/`_exit` (the only safe acts after forking a multithreaded process). |
| reap | detached thread `waitpid()`s its own child | the `WaitForSingleObject`+`CloseHandle` twin — no zombie survives. No 5 s bound needed (notify-send exits fast even on failure; the thread is detached, so blocking there is harmless). |
| injection safety | argv array — title/body are argv slots, never a shell string; leading `--` terminates option parsing | the Linux analog of `-EncodedCommand`, solved ONE LEVEL LOWER: there is no shell to escape for. `--` stops a dash-leading title (`-h`, `--version`) being read as a notify-send option — the argv-level twin of the quote-injection case. |
| failure | `execvp` fail → child `_exit(127)`; fork fail → nothing to reap | notify-send absent / no daemon → notification silently dropped. Swallowed — the same best-effort contract the Windows `if (pi.hProcess)` guard honors. |

**Named consideration (surfaced, accepted):** POSIX lists `execve` (not
`execvp`) as async-signal-safe; `execvp`'s PATH search is the reason. Mitigated
by building argv before the fork (no allocation in the child) — and glibc's
`execvp` does its PATH search on the stack, not via malloc, so the
malloc-lock-in-child hazard is effectively nil here. `posix_spawnp` is the
guaranteed-safe alternative if ever wanted; fork+execvp is chosen for being the
transparent `CreateProcessA` parallel (Dos ratified).

## 4. Injection safety detail (the crux) — argv, never a shell

The argv construction is factored into `src/platform/linux/NotifyArgv.h`
(`platform::lnx::buildNotifyArgv`) — DELIBERATELY under `src/platform/linux/`,
not `include/core/`: the notify-send command shape is TRANSPORT knowledge, kept
out of the platform-neutral core headers exactly as Discord framing stayed in
DiscordRP and WinINet flags stayed in HttpWinInet. It exists as a separate pure
function only so `notify_argv_test` can assert the argv headless (no daemon, no
spawn) — the argv IS the injection-safety surface:

```
notify-send -a RE-MOCT -- <title> <body>
```

- **Shell injection: impossible** — `execvp` hands the slots straight to
  notify-send; quotes/apostrophes/semicolons/accents in metadata are inert data.
- **Option injection: closed by `--`** — everything after `--` is positional
  (summary, body); `-h`/`--version` as a title is a summary, not a flag.

## 5. Interim cmdline echo — KEPT (decision 2)

The slice-2 `4f0b240` fallback (`UIManager.cpp:145-154`, members
`UIManager.h:246-253`) mirrors the toast's title into the cmdline bar for a few
seconds on Linux. **Kept**, reframed from "interim until slice 5" to a permanent
graceful-degradation surface: the toast and the cmdline bar serve DIFFERENT
surfaces (desktop vs terminal-inline); toast-only status (`^B` "logged in as",
`^G` "approve in browser", theme toggles) is genuinely useful inline in a TUI;
and we cannot cheaply/synchronously know at call time whether a daemon will
render the toast. Accepted cost: on a desktop WITH a daemon a user sees both a
toast and a ~5 s cmdline line (a small divergence from Windows' toast-only).
Change is comment-only (retitle the block); the behavior already ships.

## 6. Wiring

- NEW `src/platform/linux/NotifyArgv.h` — the pure argv builder (transport-side).
- NEW `src/platform/linux/NotifyNotifySend.cpp` — impl + the `core::notifier()`
  link-time bridge (the HttpCurl.cpp / IpcUnixSocket.cpp pattern).
- `SeamStubs.cpp`: `StubNotify` + its `notifier()` bridge + the `INotify.h`
  include removed (file shrinks on schedule — only the CD stub remains for
  slice 6). Header comment updated (slice 5 → LANDED; filename corrected from the
  placeholder `NotifyLibnotify.cpp` to `NotifyNotifySend.cpp`).
- `CMakeLists.txt`: `NotifyNotifySend.cpp` appended to the `if(UNIX)` sources AND
  to the `remoct_linux_seams` OBJECT lib (the platform-clean standalone check).
- `INotify.h`, `Toast.h`, `NotifyWinToast.cpp`, `DiscordRP*`: **zero diff.**
- `UIManager.cpp` / `UIManager.h`: comment-only retitle of the kept cmdline echo.

## 7. Test + gate plan

Headless:
- **`notify_toast_test` unchanged** — already proves the consumer requests the
  right (title, body) through the real Toast.h adapter, incl. the `L'Artist` /
  `Don't "Stop"` quote case (#7) and Björk accents (#2). Both platforms.
- **NEW Linux-only `notify_argv_test`** (compiled under `if(UNIX)`, as the CD
  tests are Windows-only): asserts `buildNotifyArgv` puts title/body in argv
  VERBATIM, `--` precedes the positionals (the option-injection guard),
  `-a RE-MOCT` present, accents byte-for-byte, shell metachars inert. Tests the
  transport safety in the net — BETTER than Windows, whose `-EncodedCommand`
  escaping is only live-gate-proven (asserting it would need a UTF-16LE/base64
  decoder). Expected counts: Windows 16/16 (unchanged), Linux 13→14.

Live gates:
- **Primary (authoritative close): Dos's Debian 13 XFCE box** (`xfce4-notifyd`) —
  a real toast appears with correct title/body on a track change / status event;
  confirm the accented case (Björk-class UTF-8) and the `^B`/`^G` toasts that
  were no-ops until now.
- **Secondary (Claude-runnable on 7of9): dunst under WSLg** — attempt a real
  toast in the WSL2 session (start `dunst`, drive the TUI, observe render),
  mirroring the slice-4 rhythm (Claude via the available surface; Dos closes
  100% native). If WSLg's bus won't cooperate, it degrades to the documented
  headless no-op and the desktop is the close.
- **Windows non-event:** new `platform::linux` file only; `NotifyWinToast.cpp`
  untouched. ctest count unchanged + preprocessed-TU non-event (a formality).

## 8. Daemon-availability (the honest limit, documented)

A rendered toast needs a running notification daemon owning
`org.freedesktop.Notifications` (dunst / GNOME Shell / KDE / xfce4-notifyd).
Headless WSL2 / CI / SSH has none → notify-send exits non-zero and nothing
appears. The impl SWALLOWS that (INotify is best-effort, no error reporting —
the same contract the Windows impl honors when CreateProcess fails); no crash,
no hang (we only reap a fast-failing child; we never wait on the daemon). This
is the slice-4 split exactly: the unit test proves the call is BUILT correctly
(headless, both platforms); the real-toast-appears proof needs a desktop daemon.
