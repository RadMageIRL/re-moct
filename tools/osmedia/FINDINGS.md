# osmedia-phase0-probe - FINDINGS

Phase 0 feasibility, both probes RUN and PASSED at the machine-checkable level.
No production code, no seam, nothing committed. Probe artifacts retained here
(smtc_probe.cpp + build-smtc.sh; mpris_probe.c + drive-mpris.sh) - rebuildable
when the toolchain/deps/platform shift.

## 1. SMTC under UCRT64 GCC 15.2 - REACHABLE CLEANLY (yes)

The raw WinRT ABI path works with no cppwinrt and no WRL. mingw-w64's
`<windows.media.h>` exposes the full SMTC ABI in `ABI::Windows::Media::*` with
`__CRT_UUID_DECL`, so `__uuidof` resolves every interface and the parameterized
`ITypedEventHandler<...>` delegate under GCC.

- **Compiles + links** under `g++ 15.2.0` (UCRT64).
- **Every ABI call returned S_OK at runtime** (smtc_probe.exe): `RoInitialize`,
  `RoGetActivationFactory` for `ISystemMediaTransportControlsInterop`,
  `interop->GetForWindow(hwnd, __uuidof(ISystemMediaTransportControls), ...)`,
  the enable setters, `PlaybackStatus`, the `DisplayUpdater` music
  title/artist + `Update()`, and `add_ButtonPressed` registration.
- **Interactive-only remainder:** whether a media-key / lock-screen press
  actually fires the handler is a human keypress (the probe pumps messages 20 s
  and prints `>>> ButtonPressed: <name>` on delivery). Registration returning
  S_OK proves the callback plumbing is wired; on Windows 11 there is no classic
  on-screen "flyout" (that surface is gone since Win10-era) - the ground truth
  is media-key delivery to the handler + the now-playing showing in the volume
  OSD / lock screen, which is exactly what foobar2000-class Win32 apps get via
  this same GetForWindow path.

### Include / link set (exact)
- Includes: `<windows.h> <roapi.h> <winstring.h> <windows.media.h>
  <systemmediatransportcontrolsinterop.h>`
- Link: `-lruntimeobject -lole32 -luser32 -lgdi32`
  (runtimeobject supplies RoInitialize/RoGetActivationFactory/WindowsCreateString;
  **no `-lwindowsapp` needed**). Build as a plain `main()` - do NOT pass
  `-municode` (it forces a `wWinMain` entry and fails to link).

### ABI surface used (what the seam's win impl will hand-roll)
- `ISystemMediaTransportControlsInterop::GetForWindow` (the HWND bridge).
- `ISystemMediaTransportControls` (enable flags, PlaybackStatus, add/remove_ButtonPressed).
- `ISystemMediaTransportControlsDisplayUpdater` + `IMusicDisplayProperties` (now-playing).
- `ISystemMediaTransportControlsButtonPressedEventArgs::get_Button` (which key).
- One hand-rolled `ITypedEventHandler<SMTC*, ButtonPressedEventArgs*>`
  (IUnknown + IAgileObject + Invoke; ~35 lines). This is the only bespoke COM
  object; everything else is straight vtable calls. Cost: modest, not a hack.

## 2. HWND for the SMTC seam - REACHABLE

The wingui build's top-level GDI window is `extern "C" HWND PDC_hWnd;`
(PDCursesMod's pdcscrn.c), already referenced in
`src/UIManager.cpp` (the fullscreen toggle uses it). A seam reaches it the same
way. It is only defined in the `#ifdef PDCURSES` (wingui) build - the SMTC impl
is wingui-only, which matches (a console build has no persistent owner HWND).

## 3. MPRIS via sd-bus on WSL2 Debian - REACHABLE (sd-bus confirmed)

`libsystemd-dev` was not preinstalled but is in Debian Trixie's repos; installed
cleanly (libsystemd 257.13). `playerctl` (2.4.1) likewise for the drive test.
The WSL2 box has **no login session bus** (`/run/user/1000/bus` absent), so the
probe runs under `dbus-run-session` - the real app will use whatever session
bus the user's desktop provides; headless WSL is just the test env.

- **Built** with `cc -std=c11 ... $(pkg-config --cflags --libs libsystemd)`;
  link set is simply **`-lsystemd`** (sd-bus.h is on the default include path,
  pkg-config cflags empty).
- **Driven successfully** (drive-mpris.sh): `playerctl -l` lists `remoct`;
  `status` -> `Playing`; `metadata` returns the exported trackid/title/artist;
  `play-pause`, `next`, `previous`, `stop` all reached the stub handlers
  (logged). Both interfaces (`org.mpris.MediaPlayer2` + `.Player`) export with
  the requested properties via the sd-bus vtable API (`SD_BUS_METHOD` /
  `SD_BUS_PROPERTY`).

### Dispatch model (for the next brief's threading design)
sd-bus needs **no dedicated thread**. It exposes a single fd
(`sd_bus_get_fd`) plus an events mask (`sd_bus_get_events`) and a timeout
(`sd_bus_get_timeout`); the canonical loop is `sd_bus_process` (drain while it
returns > 0) then `poll(fd, events, timeout)` when idle. That fd folds directly
into an existing `poll()`/select set - so the bus can be serviced from the same
place the app already waits, and command handlers marshal to the UI thread from
there (no D-Bus call touches AudioManager directly). The probe demonstrates the
exact loop (with a 200 ms cap so signals stay prompt).

### Dep note (as the brief asked)
sd-bus is the right default: one link (`-lsystemd`), no GLib pull. GDBus/libdbus
were NOT needed and are not recommended over this. If a target ever lacks
libsystemd (e.g. a non-systemd distro), GDBus is the fallback at the cost of the
GLib dependency + its main-loop - flag at that point, do not pre-adopt.

## 4. Live-tree divergences hit while probing

None. `PDC_hWnd` is where the brief implied; no contradiction with live code.
No production files were read for symbols beyond confirming the HWND anchor.

## Bottom line for the design lock
Both hard dependencies are reachable with modest, proportional cost:
- Windows: raw WinRT ABI under GCC, one hand-rolled delegate, link
  `-lruntimeobject -lole32 -luser32 -lgdi32`, seam over `PDC_hWnd`.
- Linux: sd-bus, link `-lsystemd`, fd folds into the app's poll, MPRIS drives
  via playerctl.
The bidirectional command path (ButtonPressed / MPRIS methods) is proven
reachable on both; routing those commands (pause/stop/seek -> AudioManager;
next/prev -> UI track-switch; seek -> UIManager coalescer) and the UI-thread
marshal are DESIGN for the next brief.
