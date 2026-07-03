# RE-MOCT architecture & strategy

> **The internal Source interface is COMPLETE** (Phase 2, closed 2026-07-03):
> `core::ISource` at `include/core/ISource.h` — readFrames / caps(seekable,
> finite, live) / positionSec / durationSec / seekTo / close — implemented by
> all three real source families: StreamSource (iHeart + ICY behind one class,
> slice A), CDSource (slice A), and LocalFileSource (slice B — extracted from
> AudioManager; the audio-thread track swap is a pointer move + retirement
> under the unchanged release/acquire protocol). Sequencing steps 1 and 2 below
> are done: compile-time, statically linked, deliberately NOT the plugin ABI.
> `open()` and metadata are excluded by decision (roadmap Decisions log) —
> construction stays concrete, and the capability flags declare divergence
> instead of faking uniformity. **The audio callback's dispatch is deliberately
> CONCRETE** (slice C declined): the file branch runs two live sources during a
> crossfade, so a single active-`ISource*` would misrepresent the machinery;
> the per-mode branches encode real semantics. Plugin-layer dispatch is Phase
> 4's own design, at the host↔plugin registry boundary — not a retrofit here.

## Plugin / Source interface (the endgame, designed but not yet built as a PLUGIN boundary)
The plugin contract is a **PCM-centric, transport-agnostic Source**:
`open / readFrames(float*) / seek? / metadata / capabilities / close`. *How* a
plugin produces frames — decode in-process, or manage an out-of-process sidecar and
pipe PCM over IPC — is its own private business. The host only sees frames +
metadata + capability flags (seekable? live? track-list? duration?).

Why transport-agnostic: it has to span very different models — live, unseekable,
ad-stitched radio (iHeart) vs. on-demand seekable account services. Designing the
contract around frames + capabilities (not around any one source's quirks) is what
keeps it general. Local file and CD likely stay **built-in**, not plugins; plugins
are for streaming sources.

**Sequencing discipline (do NOT carve the ABI first):**
1. Internal compile-time C++ interface (statically linked).
2. Refactor existing sources (file, iHeart, ICY, CD) to it — proves the shape with
   zero new deps, testable under the Phase 0 harness.
3. Only then harden into a loadable C-ABI `.dll`/`.so`; iHeart becomes the first
   real plugin.

**Host-provided transport:** the host should hand plugins an `http_get` (and IPC,
notify) service rather than each plugin carrying its own WinINet. This keeps plugins
platform-neutral and quietly does the Phase 1 HTTP consolidation at the same time.

> **The HTTP seam now exists** (Phase 1 slice 1; since slice 5 at
> `include/core/IHttp.h`): `core::IHttp` is
> the interface the host will hand plugins — the DI target that makes the above real.
> Its `platform::win::WinInetHttp` impl is the first concrete transport. The current
> process-wide `core::http()` accessor is a **transitional** migration seam ONLY;
> the endgame is injection (pass the `IHttp&` in), NOT a global. Don't enshrine it.
> A `core::setHttp(IHttp*)` injection hook (slice 2) now exists — a concrete step toward
> that DI endgame: it swaps the transport process-wide (used today by tests to inject a
> fake; tomorrow by the host to hand plugins the service).
> The neutral surface grew where a real consumer needed it (slice 3): a `RedirectPolicy`
> enum (FollowAll/None/SameScheme) and pure, platform-neutral helpers `urlIsSecureScheme()`
> + `finalizeBody()` (cap/reject) live in the interface header — testable off-platform, no
> WinINet leak. That's the seam doing its job: platform behavior expressed as neutral policy.
>
> **Slice 4 added the two capabilities Phase 2/4 will lean on** (HTTP consolidation is
> now 8/8 — every WinINet site outside the live audio read loop goes through the seam):
> - **`IHttpSession` / `openSession(HttpSessionConfig)`** — the session shape a Source
>   (Phase 2) or plugin (Phase 4) holds for its open→close lifetime, with the host handing
>   in `IHttp` as the factory. Explicit object, caller-managed lifetime, NO hidden pooling
>   global (that alternative was rejected as anti-DI). The default forwarding impl on
>   `IHttp` means fakes keep working unchanged; `WinInetHttp` overrides it with a real
>   keep-alive session (libcurl later: a held easy handle).
> - **The cancel token** (`HttpRequest::cancel`, a polled `std::atomic<bool>*`) — the
>   abort mechanism for any in-flight fetch; this is how a plugin's stop/close will abort
>   its network work promptly (libcurl: the progress callback polls the same atomic).

> Spotify (dropped for now): if ever revisited, the realistic path is a **sidecar** —
> run librespot / go-librespot as a separate process, talk control + audio over IPC
> (reuse the Discord named-pipe muscle). Constraints: Premium-only (always), ToS gray
> area ("probably forbidden, use at your own risk"), Vorbis ≤320kbps (no lossless),
> and it breaks when Spotify changes their side — which is itself the best argument
> for the loadable-plugin endgame. The transport-agnostic Source contract keeps this
> option open without planning for it now.

## Platform abstraction — seams, not #ifdefs
```
src/core/            platform-neutral: playback, UI, decode, metadata, plugin host
src/platform/        the interfaces: http_get, ipc, notify, cd_io
src/platform/win/    WinINet, named-pipe, PowerShell toast, IOCTL
src/platform/linux/  libcurl, Unix socket, libnotify/notify-send, SG_IO
```
CMake compiles `platform/win/*` only on Windows and `platform/linux/*` only on
Linux (`if(WIN32)`/`if(UNIX)` selecting target sources + libs), so whole files are
excluded per-OS and `#ifdef`s shrink to tiny inline divergences. **`core/` never
includes a platform header** — it only talks to the interfaces. The test: if `core/`
compiles on Linux, nothing leaked. Nothing platform-specific belongs in an interface.

> **The boundary now exists on disk** (Phase 1 slice 5): `include/core/IHttp.h` +
> `src/platform/win/HttpWinInet.cpp`, seeded with the finished HTTP seam. Consumers
> write `#include "core/IHttp.h"` — the layer is visible at the include site (that's
> the audit: grep core files for platform includes). Remaining seams (notify, cd_io)
> get built into this structure directly. `src/platform/linux/` is created when
> Phase 3 lands the libcurl/Unix-socket/libnotify/SG_IO impls, not before.
>
> **The IPC seam is in** (slice 6, the first built-into-the-boundary seam):
> `core::IIpc`/`IIpcChannel` (`include/core/IIpc.h`) + `platform::win::WinPipeIpc`
> (`src/platform/win/IpcWinPipe.cpp`). A local bidirectional byte channel — three
> primitives (send/waitReadable/recvSome) shaped by the one real consumer
> (DiscordRP); protocol (framing, endpoint probe, handshake, reconnect) stays in
> the consumer, exactly the IHttp split. **DI precedent set:** DiscordRP takes its
> `IIpc*` by constructor injection (production default `core::ipc()`, a link-time
> bridge only — no `setIpc()` global). This is the endgame shape arriving early:
> single-consumer seams get injected directly; the http()/setHttp() transitional
> pattern is reserved for many-consumer migrations. Linux sibling = Unix domain
> socket over the same interface (Phase 3).
>
> **The notifications seam is in** (slice 7, the third seam in the boundary):
> `core::INotify` (`include/core/INotify.h`) + `platform::win::WinToastNotify`
> (`src/platform/win/NotifyWinToast.cpp`, the PowerShell-toast transport). One
> primitive — `notify(title, body)` — and content stays consumer-side in Toast.h,
> a header-only adapter holding the track/status → title/body mapping (the same
> content/transport line HTTP and IPC held). Ctor DI again: UIManager takes an
> `INotify*` (production default `core::notifier()`, link-time bridge only — no
> setNotify global) — the second consumer to get the endgame shape directly.
> Linux sibling = libnotify/notify-send (Phase 3), whose native surface is exactly
> (title, body).
>
> **The CD-I/O seam is in** (slice 8 — the LAST seam; the leak map is CLEAR):
> `core::ICdIo`/`ICdDevice` (`include/core/ICdIo.h`) + `platform::win::WinCdIo`
> (`src/platform/win/CdIoWin.cpp`). Raw optical-drive transport — factory open →
> device object; readToc (raw MSF — LBA math stays consumer-side), last-session
> query, readRaw (explicit `want_c2` + bytes-returned), setSpeed, mediaPresent,
> model. Every impl method is one baseline DeviceIoControl moved parameter-
> identical; ALL rip/AR/Enhanced-CD logic stayed in CDRipper/CDSource — the same
> protocol/transport split as HTTP/IPC/notify. Ctor DI ×2 (CDSource + CDRipper,
> production default `core::cdio()`). Proven by the heaviest gate in the project:
> a byte-identical AccurateRip rip (12/12 v2 conf 200, every CRC matching the
> pre-migration baseline log). Linux sibling = SG_IO on /dev/srN (Phase 3): READ
> CD 0xBE (readRaw ± C2 via CDB bits), READ TOC 0x43 (TIME=1 → MSF; format 01 →
> session), TEST UNIT READY, INQUIRY, SET CD SPEED 0xBB — one-to-one.
> **Phase 1's seam work is complete: core/ no longer touches WinINet, named
> pipes, PowerShell, or IOCTLs directly.**

## Linux port
- WSL2 on 7of9 = fast local inner loop; CI on real Ubuntu = source of truth.
- The port is the forcing function that proves the boundary is clean.

## GitHub strategy
- **One repo, one `main`, trunk-based.** Platform difference handled at *build time*
  via CMake (above) — NEVER as `windows`/`linux` branches (they drift, merging is
  misery). Branches are for features/releases only.
- **CI matrix** (GitHub Actions): `windows-latest` (via `msys2/setup-msys2`) +
  `ubuntu-latest`, on every push. This is the highest-leverage move — Dos develops on
  Windows, so the Linux job is what catches a Windows-ism leaking into `core/` the
  moment it appears. Wire the Phase 0 harness into CI.
- **Releases:** tag `vX.Y.Z` → CI builds both → upload `remoct-vX-win.zip` and
  `remoct-vX-linux.tar.gz` (AppImage later) to one Release. One version, two artifacts.
- **Dependencies:** MSYS2/UCRT64 packages on Windows, distro packages on Linux, both
  documented in the README. Reach for vcpkg/Conan only if reproducibility bites.
- **Plugins on GitHub:** keep them in the monorepo (`plugins/iheart/`) while the ABI
  is still churning — one CI, atomic host+plugin commits. Extract to their own repo
  only once the ABI freezes (that's when the "ship without rebuilding host" payoff is
  real). SDK headers: start as a folder in the host repo; split to `re-moct-sdk` only
  if an external author ever needs them.

## Repo reorg (git hygiene)
When restructuring into the `src/core` + `src/platform` layout: use `git mv` (one
logical move per commit) so per-file history/blame survives — the AccurateRip/iHeart
archaeology is worth keeping. Do it on a branch; keep `main` buildable until 7of9
confirms it compiles. Rewire `CMakeLists.txt` as a reviewable diff, not a blind
rewrite. Before classifying files into `core/`, grep the tree for `windows.h`,
`wininet`, named-pipe, and IOCTL calls to build a leak map first.
