#pragma once
// ---------------------------------------------------------------------------
// Portable curses include seam.
//
// Every translation unit that needs curses includes THIS header, never
// <ncurses.h> / <ncursesw/ncurses.h> / <curses.h> directly. The X/Open curses
// API is shared across implementations, so this is a pure include substitution
// behind one boundary - no ICurses interface, no virtual dispatch.
//
// NAME: deliberately NOT "Curses.h". On Windows's case-insensitive filesystem,
// with include/ ahead of the PDCurses dir on the search path, `#include
// <curses.h>` would match this seam instead of PDCursesMod's curses.h and (with
// #pragma once) expand to nothing. Do not rename this back to Curses.h.
//
//   Linux            -> ncursesw (unchanged).
//   Windows, default -> the current ncursesw build (transitional).
//   Windows + REMOCT_PDCURSES -> PDCursesMod wingui/GDI (Option C single-exe,
//                                guaranteed truecolor for Awesome mode).
//
// The REMOCT_PDCURSES switch lets this seam land and be verified against the
// existing ncursesw build on BOTH platforms before PDCursesMod is vendored;
// the Windows-only build then defines REMOCT_PDCURSES to flip to PDCurses.
//
// NOTE: include <windows.h> (when a TU needs it) BEFORE this header - PDCurses
// and windows.h share some symbol names and expect that order. This header
// itself pulls in NO windows.h, so including it from public headers does not
// leak windows.h across the tree.
// ---------------------------------------------------------------------------

// Wide-character API must be requested before the header on every platform:
// without it this build's narrow draw functions pass UTF-8 bytes undecoded and
// box-drawing glyphs render as mojibake. (ncurses macro; PDCurses ignores it.)
#ifndef NCURSES_WIDECHAR
#  define NCURSES_WIDECHAR 1
#endif

#ifdef _WIN32
#  ifndef NCURSES_STATIC
#    define NCURSES_STATIC
#  endif
#  if defined(REMOCT_PDCURSES)
// PDCursesMod wingui: wide build + forced UTF-8 so box-drawing / viz glyphs are
// single-column wide chars. These are no-ops for a non-PDCurses header.
#    ifndef PDC_WIDE
#      define PDC_WIDE
#    endif
#    ifndef PDC_FORCE_UTF8
#      define PDC_FORCE_UTF8
#    endif
#    include <curses.h>
#  elif defined(__has_include) && __has_include(<ncursesw/ncurses.h>)
#    include <ncursesw/ncurses.h>
#  else
#    include <ncurses.h>
#  endif
#else
#  if defined(__has_include) && __has_include(<ncursesw/ncurses.h>)
#    include <ncursesw/ncurses.h>
#  else
#    include <ncurses.h>
#  endif
#endif
