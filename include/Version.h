#ifndef REMOCT_VERSION_H
#define REMOCT_VERSION_H

// ─── Single source of truth for the RE-MOCT version ──────────────────────────
// Bump this ONE definition to change the version everywhere. It feeds the UI
// (status line + About panel), the CD-rip tags (TENC / ENCODER / .cue comment),
// every HTTP User-Agent (CoverArt, CDRipper/CTDB, the WinINet + libcurl seams),
// and the scrobble client-version field.
//
// This is the copyright/product version only. The plugin C-ABI version
// (REMOCT_ABI_VERSION in include/core/remoct_plugin.h) is deliberately separate.
#define REMOCT_VERSION "1.4.0"

// Wide (UTF-16) form for the Win32 seam, which speaks wchar_t literals. Two-level
// indirection so the macro argument is expanded before the L## paste.
#define REMOCT_WIDEN_(x) L##x
#define REMOCT_WIDEN(x)  REMOCT_WIDEN_(x)
#define REMOCT_VERSION_W REMOCT_WIDEN(REMOCT_VERSION)

#endif // REMOCT_VERSION_H
