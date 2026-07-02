# RE-MOCT architecture & strategy

## Plugin / Source interface (the endgame, designed but not yet built)
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

> **The HTTP seam now exists** (Phase 1 slice 1, `include/IHttp.h`): `core::IHttp` is
> the interface the host will hand plugins — the DI target that makes the above real.
> Its `platform::win::WinInetHttp` impl is the first concrete transport. The current
> process-wide `core::http()` accessor is a **transitional** migration seam ONLY;
> the endgame is injection (pass the `IHttp&` in), NOT a global. Don't enshrine it.
> A `core::setHttp(IHttp*)` injection hook (slice 2) now exists — a concrete step toward
> that DI endgame: it swaps the transport process-wide (used today by tests to inject a
> fake; tomorrow by the host to hand plugins the service).

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
