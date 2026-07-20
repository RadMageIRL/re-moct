# Vendored: PDCursesMod (wingui / GDI port)

Upstream: https://github.com/Bill-Gray/PDCursesMod
Pinned commit: `d9a59832658294c797447747f1664832fc21136e`
Vendored: 2026-07-04, for RE-MOCT Option C (Windows single static exe, truecolor
Awesome mode). Windows-only; Linux stays on ncursesw.

## Why this fork
PDCursesMod is the maintained home of the Win32 GUI (wingui) port - the GDI
"flavor" that draws its own window and colours, giving guaranteed truecolor
independent of any console/terminfo ceiling. Same variant musikcube ships for
its Windows look.

## What was vendored (minimal subset, not the whole tree)
- `curses.h`, `curspriv.h`, `panel.h`, `term.h`  - the X/Open API headers
- `pdcurses/`  - the portable curses core (41 .c)
- `wingui/`    - the GDI platform port (7 .c + pdcwin.h). These `#include`
                 several `common/*.c` helpers directly (not separate objects):
                 pdccolor.c, winclip.c, mouse.c, blink.c, beep.c, acs_defs.h.
- `common/`    - the shared helpers the wingui sources pull in.
Dropped: every other port (wincon, sdl1/2, x11, gl, fb, vt, dos, os2, plan9),
plus demos/tests/docs.

## How it is built
Not via the upstream Makefiles/CMake. The repo's own CMakeLists.txt compiles
`pdcurses/*.c` + `wingui/*.c` into a static lib target `pdcurses_wingui` with
`-DPDC_WIDE -DPDC_FORCE_UTF8` and links `gdi32 comdlg32 winmm`. Gated behind
`-DREMOCT_PDCURSES=ON` (see the "Option C" block in the root CMakeLists.txt).
The TUI includes it through `include/CursesSeam.h`, never `<curses.h>` directly.

NOTE the seam header is `CursesSeam.h`, NOT `Curses.h`: on Windows's
case-insensitive filesystem a header named Curses.h would shadow this fork's
`curses.h` on `#include <curses.h>`.

## License
Core is Public Domain; small portions carry other free licenses (see the
per-directory README "Distribution Status" sections in this tree). We ship none
of the GPL build scripts (config.guess/config.sub/configure were not vendored).

## RE-MOCT patches (local diffs from upstream)
All tagged in-tree with `/* RE-MOCT patch: ... */`. In `wingui/pdcscrn.c`:
- `PDC_skip_size_snap` - suppress the WM_SIZE whole-cell snap during Alt+Enter
  borderless fullscreen so the window covers the exact monitor rect.
- `PDC_set_window_resized_callback` - fire an app callback from the WM_SIZE
  handler so the TUI repaints live during the modal resize-drag.
- `PDC_set_paint_tick_callback` + `TIMER_ID_FOR_MODAL_PAINT` - a WM_TIMER pumped
  inside the modal move/size loop (started on WM_ENTERSIZEMOVE, killed on
  WM_EXITSIZEMOVE, routed ahead of the blink/mouse ids in WM_TIMER) so the TUI
  keeps animating during a title-bar MOVE, which emits WM_MOVE not WM_SIZE and so
  never triggered the resize callback.

Re-apply these after re-pinning; grep the tree for `RE-MOCT patch` to find them.

## Updating the pin
Re-clone upstream at the new commit, copy the same subset, rebuild + re-probe on
7of9, and bump the commit hash above.
