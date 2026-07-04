# Phase 3 slice 4 - IPC: Unix-domain-socket `core::IIpc` twin (design, ratified before code)

Ratified by Dos 2026-07-03 (both open calls decided: (a) flatpak/snap subdir
probing lives in the impl; (b) the npiperelay bridge is attempted as the live
Discord gate, with the honest-documentation fallback + a live Debian 13 desktop
verification by Dos to close the slice at 100%).

Scope: implement `core::IIpc` on Linux via a Unix domain socket -
`src/platform/linux/IpcUnixSocket.cpp`, `platform::lnx::{UnixSocketIpc,
UnixSocketChannel}` - the sibling of `platform::win::WinPipeIpc`. This is the
Discord Rich Presence transport. NOT sacred territory: DiscordRP runs on the UI
thread; the live audio loops are untouched by construction.

## 1. Survey - the Windows mapping being twinned (file:line)

`src/platform/win/IpcWinPipe.cpp`:

| Primitive | Windows | Notes |
|---|---|---|
| `connect(name)` | `CreateFileW(\\.\pipe\<name>)`, :65-70 | `INVALID_HANDLE_VALUE` → nullptr ("not running / not listening") |
| `send` | one `WriteFile`, `wrote==len`, :32-35 | short write = failure (atomicity is contract) |
| `waitReadable(min,timeout)` | `PeekNamedPipe` per 10 ms, :37-47 | `for (waited=0; waited<=timeout; waited+=10)` - immediate first check (timeout 0 = one peek); peek failure = broken → false NOW; `Sleep(10)` between |
| `recvSome` | one `ReadFile`, blocking, no timeout, :49-54 | failure → got=0,false; caller loops |
| teardown | `CloseHandle` only, :30 | no goodbye frame |

Consumer (`DiscordRP`) confirmed clean and **zero-diff for this slice**: ctor
injection `DiscordRP(app_id, core::IIpc* = nullptr)` → `core::ipc()`
(DiscordRP.h:21-22); protocol all consumer-side and platform-neutral - slot
probe 0..9 (DiscordRP.cpp:26-29), single-buffer framing (:41-54), bounded
header wait `waitReadable(8,1000)` (:59), deliberately UNbounded body loop
with byte accounting (:70-76), 1 MB cap, CLOSE→reject, lazy reconnect;
`rpPid()` got its getpid twin at slice 1 (:13-16).

Linux today: `StubIpc` returns nullptr (SeamStubs.cpp:31-35). This slice
replaces it, per the slice-0 plan (SeamStubs.cpp:9 names this file).

## 2. The Linux mapping (parity table - every delta named)

| Primitive | Linux | Deltas vs Windows |
|---|---|---|
| `connect` | `socket(AF_UNIX, SOCK_STREAM\|SOCK_CLOEXEC)` + blocking `::connect()` per candidate path; ENOENT/ECONNREFUSED → next candidate; all fail → nullptr | `SOCK_CLOEXEC` IS parity (CreateFileW handles are non-inheritable by default). Candidates over sun_path (108) are skipped. Stale socket file with no listener = ECONNREFUSED → nullptr, same outcome as an absent pipe. |
| `send` | one `::send(fd,data,len,MSG_NOSIGNAL)`; `n==len` | **MSG_NOSIGNAL is load-bearing**: without it a dead peer raises SIGPIPE and kills the process where WriteFile returns FALSE; the flag turns peer-death into EPIPE → false. EINTR before any byte → retry (a failure mode ReadFile/WriteFile don't have; retry preserves "whole write or failure"); partial-transfer short count → false → consumer discards + reconnects (the contract's failure mode). |
| `waitReadable` | Windows loop shape verbatim - `for (waited=0; waited<=timeout; waited+=10)`: `poll(fd,POLLIN,0)` + `ioctl(FIONREAD)`; `avail>=min` → true; wait slice = `poll(fd,POLLIN,10)` | (1) Broken detection: POLLERR/POLLNVAL, or readable-but-FIONREAD==0 (EOF) → false - same outcome as PeekNamedPipe failing (dead peer → false now, not timeout-late). (2) Buffered-then-closed data counts toward min_bytes on both platforms; POLLHUP with avail>=min is still true. POLLHUP with 0<avail<min → false early where Windows would spin to timeout and return false anyway - same result, faster (named accepted-better). (3) The 10 ms wait slice wakes EARLY on arriving data vs blind Sleep(10) - latency-only, same bound/granularity (accepted-better, slice-3 class). timeout==0 = one immediate check, both platforms. |
| `recvSome` | one `::recv(fd,out,max_len,0)`, blocking, no timeout; n>0 → got=n,true; n==0 (EOF) → false; n<0: EINTR → retry, else false | EOF maps to ReadFile's broken-pipe FALSE. **EINTR retry required**: ncurses' SIGWINCH (terminal resize mid-read) would otherwise masquerade as a broken channel. Asymmetric shape preserved exactly - no timeout invented (the lessons.md rule). |
| teardown | dtor `::close(fd)` only | CloseHandle parity; no shutdown, no goodbye frame. |

## 3. Endpoint discovery (decision (a): subdir list in the impl)

Canonical order (official discord-rpc `connection_unix.c`; pypresence/arrpc
concur):

1. Base dir = `$XDG_RUNTIME_DIR` → `$TMPDIR` → `$TMP` → `$TEMP` → `/tmp`
   (first SET wins; matters in practice - a root WSL2 session may have
   XDG_RUNTIME_DIR unset).
2. Sandboxed installs socket under a subdir of the base:
   flatpak = `app/com.discordapp.Discord/`, snap = `snap.discord/`.
3. Slots 0–9 - the consumer's loop, unchanged.

`connect(name)` therefore tries, in order: `<base>/<name>`,
`<base>/app/com.discordapp.Discord/<name>`, `<base>/snap.discord/<name>`
(plain first, so a native install wins).

**Named tension, accepted:** the flatpak/snap subdir strings are Discord
INSTALL knowledge riding in the impl, where the seam doctrine says
Discord-specific knowledge is the consumer's. Accepted because the
alternative pushes Unix path shapes into the platform-neutral consumer
(an `#ifdef` endpoint list in DiscordRP, or Windows probing 30 garbage pipe
names per reconnect attempt), and because it buys DiscordRP **zero diff** -
Windows behavior identical by construction. The impl comment names the leak.
IIpc.h's two `$XDG_RUNTIME_DIR/<name>` doc comments get updated to describe
the candidate list (doc-only; the header stays platform-clean).

## 4. Wiring

- NEW `src/platform/linux/IpcUnixSocket.cpp` (impl + the `core::ipc()`
  link-time bridge - the HttpCurl.cpp pattern).
- SeamStubs.cpp: StubIpc + its ipc() bridge removed (file shrinks on
  schedule; notify/CD stubs remain for slices 5/6).
- CMakeLists.txt: IpcUnixSocket.cpp appended to the `if(UNIX)` sources.
- IpcWinPipe.cpp / DiscordRP.cpp / DiscordRP.h: **zero diff**.
- UIManager.cpp Discord gate sweep (the slice-2 promise "^D = Discord IPC
  (slice 4)"; CoverArt portable since group (c), so the art thread goes too):
  1. dtor art-thread join (:164-166) - the adjacent mb/cd cancel gate STAYS
     (slice 6);
  2. `startDiscordArtLookup` whole-function gate (:3057/3079);
  3. `discord_presence` term in updateScrobbler's early return (:3127-3131);
  4. stopped-state activity clear (:3137-3140);
  5. the main RP block (:3197/3272);
  6. the ^D key case (:3871/3885) + the per-case-gate comment updated.
  UIManager.h needs nothing (discord_ members already ungated). End-of-slice
  ritual: `grep -n "#ifdef _WIN32"` audit over UIManager.cpp + DiscordRP.cpp.
  Windows held at the slice-3 standard: preprocessed-TU diff on UIManager.cpp
  (gate removal only ADDS code to the Linux build).

## 5. Test + gate plan

Headless (both platforms):
- **discord_ipc_test goes portable** - pure protocol via injected FakeIpc;
  Windows-only today only because it links IpcWinPipe.cpp to satisfy
  `core::ipc()`. An `IPC_IMPL` variable (the HTTP_IMPL pattern) links the
  right impl per platform; test body unchanged.
- **NEW ipc_echo_test** - the transport proof: a REAL local server through
  the REAL impl (Windows: CreateNamedPipe fixture thread; Linux: unix-socket
  listener, XDG_RUNTIME_DIR pointed at a temp dir so discovery resolves to
  the fixture). Cases: absent endpoint → nullptr; round-trip echo
  byte-identical; split delivery (header, pause, body - waitReadable(8)
  gates, byte-accounting drain); waitReadable timeout bound on a silent
  server; partial-below-min stays false; buffered-then-close drains then
  EOFs; **send-to-dead-peer returns false with the process alive** (the
  MSG_NOSIGNAL regression case); recv-after-close → false. Baseline-first:
  green against the UNTOUCHED Windows impl before the Linux twin exists.
  Fixture owns observation state (the discord_ipc_test dangling-fake lesson).
- Expected counts: Windows 16/16, Linux 13/13.

Live gates (Linux):
- **socat echo probe** (the handoff's named gate): socat UNIX-LISTEN on
  `$XDG_RUNTIME_DIR/discord-ipc-0` + hexdump; real remoct TUI, ^D on →
  the handshake frame `[op=0][len]{"v":1,"client_id":…}` observed at socat.
  tmux gate scripted with per-keystroke pane captures (slice-3 lesson).
- **Live Discord RP (decision (b))**: attempt the npiperelay + socat bridge -
  Windows Discord's `\\.\pipe\discord-ipc-0` bridged into WSL2 as
  `$XDG_RUNTIME_DIR/discord-ipc-0`. A REAL Discord through a GENUINE Linux
  unix socket: full slice-6 gate (RP title/artist + art, track change,
  Discord-restart reconnect). Honest caveat: socat creates the socket, so
  the flatpak/snap candidates stay unit-fixture-proven only. Optional
  second attempt: WSLg native Discord. **If all reasonable attempts fail:**
  notify, commit/push, and Dos runs the live gate on a real Debian 13
  desktop with a live Discord account - the slice closes 100% on that
  report. No faked Discord, no overclaiming.
- Windows non-event: baseline ctest BEFORE edits, 16/16 after,
  preprocessed-TU diff, quick live ^D spot check.
