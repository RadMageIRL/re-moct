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

In `pdcurses/refresh.c` (`PDC_transform_line_sliced`):
- **The leading-dummy repair runs once instead of per chunk. This one is an
  UPSTREAM DEFECT, not a local preference** - the only patch here that is.
  Upstream repairs a refresh span that begins on `DUMMY_CHAR_NEXT_TO_FULLWIDTH`
  (the placeholder in a fullwidth glyph's second cell) by backing up one cell onto
  the glyph's first half. That repair sat ABOVE the `while( len)` loop, so it ran
  for the first chunk only. The loop then cuts the span into `MAX_PACKET_LEN - 1`
  = **89**-cell chunks, and every `srcp += i` can land the next chunk on a dummy
  whose first half is in the chunk just drawn. When that happened the packet loop
  computed `i == 1` with `ch == MAX_UNICODE` and
  `assert( i > 1 || ch != MAX_UNICODE)` **aborted the process**. The same boundary
  produces the sibling abort in `wingui/pdcdisp.c`
  (`assert( (srcp[len-1] & A_CHARTEXT) != MAX_UNICODE)`) when the dummy lands at
  the END of a chunk rather than the start - two faces of one defect, and the
  patch fixes both:
  1. **Leading dummy.** The repair moves inside the loop so every chunk gets it,
     with the `x == 0` arm the original lacked: at column 0 there is no cell to
     back onto, because the glyph's first half is off-line to the left, so the
     cell is skipped (`x++, srcp++, len--`) as there is nothing to draw.
  2. **Trailing dummy.** The trim read `ch`, which is STALE when the inner loop
     exits on its FIRST condition (`i` reaching `MAX_PACKET_LEN - 1`): that exit
     never evaluates `ch` for the current `i`, so `ch` holds `srcp[i-2]` and
     `srcp[i-1]` is never examined at all. The patch tests the packet's actual
     last cell. Behaviour is identical to upstream everywhere else - when the loop
     exits BECAUSE `srcp[i-1]` is a dummy, `ch` is that same cell and both forms
     trim to `i-1` - and `x`/`len`/`srcp` still advance by `i`, so the dummy is
     consumed exactly as before.
  The cursor-split recursion at the top of the function needs nothing: it recurses
  back through the same loop, so both repairs cover it.
  Upstream knows the condition is reachable: the line immediately above the assert
  is an `fprintf(stderr, "line %d, x=%d, len=%d\n", ...)` diagnostic. Left in place
  - still a useful canary if another path produces it.
  **Requires a line wider than 89 cells AND a fullwidth glyph straddling a chunk
  boundary**, which is why it only appears on wide windows showing CJK, and why
  hundreds of automated ASCII-only resizes never reproduced it.
  RE-MOCT only started hitting this at 1.6.1, when the display fold stopped
  replacing CJK with `?`: before that `PDC_wcwidth` never returned 2, no dummy cell
  ever existed, and the case was unreachable by construction. Reproduced as a hard
  abort on a border-drag with wide glyphs on screen.
  **Filed upstream as Bill-Gray/PDCursesMod issue #386. DROP THIS PATCH once it
  lands there** - check the issue before re-pinning; a patch upstream also carries
  is not ours to maintain. Report as filed: `docs/upstream-pdcursesmod-chunk-boundary.md`.

Re-apply these after re-pinning; grep the tree for `RE-MOCT patch` to find them.

## Updating the pin
Re-clone upstream at the new commit, copy the same subset, rebuild + re-probe on
7of9, and bump the commit hash above.
