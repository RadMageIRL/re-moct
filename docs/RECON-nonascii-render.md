# RECON: non-ASCII text rendering — C-R1..C-R5

> This proposal does not touch AR_PREGAP, `ar_crc.*`, or the read addressing.

**Status:** recon only. Zero repo files modified, nothing implemented, nothing committed.
UNTRACKED until a slice is greenlit.

**Tree:** `experimental/win-pdcurses` at `457e12d` (1.6.0, tracked tree clean).
**Symptom:** *Final Fantasy XI [2 Disc]* / 水田直志 renders `????` in browser and playlist rows;
the directory header line renders 水田直志 correctly in the same session.

Probes were built and run from the scratchpad against `include/StringUtils.h` on both toolchains.

---

# THE ANSWER — one function, and it is not a codepage conversion

**`sanitizeForDisplay` (`include/StringUtils.h:163-215`) folds every 3-byte and 4-byte UTF-8
sequence it does not have an explicit `case` for to a literal `'?'`, one per character.**

`include/StringUtils.h:206-210`:

```cpp
default:
    if (cp >= 0x20 && cp < 0x80) out += (char)cp;
    else if (n == 2) { out += s[i]; out += s[i+1]; }   // 2-byte: passes through INTACT
    else out += '?';                                   // 3- and 4-byte: one '?' each
    break;
```

水田直志 is four codepoints in the U+4E00–U+9FFF block, three bytes each, none of them matching a
`case`. Four characters in, four `?` out. **The `????` is exact, not approximate.**

**Measured, both toolchains:**

| input | `sanitizeForDisplay` output | why |
|---|---|---|
| `水田直志` (U+6C34 U+7530 U+76F4 U+5FD7) | `????` | 3-byte, `default` → `'?'` |
| `Final Fantasy XI オリジナル` | `Final Fantasy XI ?????` | katakana is 3-byte |
| `Björk` (U+00F6) | `Björk` | **2-byte → passes through verbatim** |
| `🎵` (U+1F3B5) | `?` | 4-byte → `'?'` |

Windows UCRT64 GCC 15.2 and Linux GCC (WSL2 Trixie) produce byte-identical output. The function is
platform-independent — no `#ifdef` anywhere in it.

**The 2-byte pass-through is why this has never surfaced before.** Every accented Latin name, all
Cyrillic, all Greek is 2-byte UTF-8 and renders correctly. Everything CJK, all kana, all emoji, and
every 3-byte symbol without an explicit `case` is destroyed. The repo has four albums of
byte-comparison history and none of it has non-Latin text.

---

# C-R1 — Which paths differ

All three paths start from correct UTF-8 and end at the same wide-character curses call. **The only
difference is whether `sanitizeForDisplay` is on the path.** It is not a rendering difference at
all.

## 1. Directory header line — WORKS

Two candidates, both unsanitized, so both work regardless of which one is being observed.

**The cwd line (`>> path`), `UIManager::drawCwd`:**

```
current_dir_  (raw std::string, UTF-8, never folded)
  → UIManager.cpp:2967   std::string path = current_dir_;
  → UIManager.cpp:2991   truncateToWidthRight(display, maxw)   // column-aware, codepoint-aligned
  → UIManager.cpp:2996   utf8_to_wide(padToWidth(line, avail))
  → UIManager.cpp:2999   mvwaddnwstr(win_cwd_, 0, cwx, ...)     // WIDE
```

**The browser pane header (`Dir: <leaf>`), `UIManager::drawDirBrowser`:**

```
UIManager.cpp:3108   std::string leaf = fs::path(current_dir_).filename().string();   // RAW
UIManager.cpp:3113   hdr = " Dir: " + leaf + ...                                       // RAW
  Classic → UIManager.cpp:3122   mvwaddnstr(win_dir_, 0, 0, bar.c_str(), cols)         // NARROW
  Awesome → UIManager.cpp:3255   panelFrame(win_dir_, hdr, ...)
             → UIManager.cpp:976 mvwaddnstr(w, 0, tx, t.c_str(), (int)t.size())        // NARROW
```

The narrow variant still renders correctly under PDCursesMod: `pdcurses/addstr.c:83` calls
`PDC_mbtowc`, which under `PDC_FORCE_UTF8` (`pdcurses/util.c:386`) decodes UTF-8 unconditionally,
independent of locale, and feeds `waddch` one decoded `wchar_t` at a time.

**Neither header path calls `sanitizeForDisplay`. That is the entire reason they survive.**

## 2. Browser row — FAILS

```
fs::directory_iterator entry
  → UIManager.cpp:10011   std::string nm = de.path().filename().string();   // correct UTF-8
  → UIManager.cpp:10027   dir_entries_.push_back(nm);                       // RAW — identity
  → UIManager.cpp:10028   dir_display_.push_back(sanitizeForDisplay(nm));   // ◄── DESTROYED HERE
  → UIManager.cpp:3130    display = dir_display_[idx]
  → UIManager.cpp:3239    scrollToWidth(prefix + display, avail, ...)
  → UIManager.cpp:3241    utf8_to_wide(padToWidth(d, cw))
  → UIManager.cpp:3242    mvwaddnwstr(win_dir_, i+1, cx, ...)               // WIDE
```

**`UIManager.cpp:10028` is the divergence.** `dir_entries_` (identity — used for `fs::path`
composition, sorting, opening) keeps the real bytes; `dir_display_` (what the row draws) is folded.
The same folder name reaches line 3113 raw and line 10028 folded.

Every other browser section funnels through the same `dir_display_` with the same fold — library
artists (`10658`), albums (`10700`), tracks (`10749`), genres (`10897`), search rows (`11013`),
radio (`8117`, `10310`), podcasts (`11348`), recent/favs/books (`9327`, `9340`, `9361`).

## 3. Playlist row — FAILS

```
TagLib tag->artist()/title()
  → PlaylistManager.cpp:99-100   tag->artist().to8Bit(true)                 // true = UTF-8, intact
  → PlaylistManager.cpp:81-82    sanitizeForDisplay(title) / (artist)       // ◄── DESTROYED HERE
  → PlaylistManager.cpp:83       return a + " - " + t;                      // → entry.display_title
  → UIManager.cpp:3442           scrollToWidth(e.display_title, nw, ...)
  → UIManager.cpp:3445           utf8_to_wide(padToWidth(line, cw))
  → UIManager.cpp:3446           mvwaddnwstr(win_playlist_, i+1, cx, ...)   // WIDE
```

**`PlaylistManager.cpp:81-82` (`displayTitleFor`) is the divergence.** The CD path folds at the same
depth: `UIManager.cpp:2527` and `:2535` (`applyReleaseTitles`) fold the MusicBrainz title before
`setDisplayTitle`, so a CD of this album shows `????` rows even after `Ctrl+R`.

## Summary of C-R1

| path | folds? | site | draw call |
|---|---|---|---|
| cwd line | no | — | `mvwaddnwstr` (wide) |
| browser pane header | no | — | `mvwaddnstr` (narrow, PDC decodes) |
| browser row | **yes** | `UIManager.cpp:10028` | `mvwaddnwstr` (wide) |
| playlist row (file) | **yes** | `PlaylistManager.cpp:81-82` | `mvwaddnwstr` (wide) |
| playlist row (CD/MB) | **yes** | `UIManager.cpp:2527, 2535` | `mvwaddnwstr` (wide) |

The draw layer is identical and correct on every row. The difference is entirely upstream.

---

# C-R2 — Where the conversion happens

**It is not a codepage conversion.** No `WideCharToMultiByte`, no `wcstombs`, no locale, no
`CP_ACP`, no environment input is involved. The `?` is a hardcoded literal in RE-MOCT's own code.

- **Which call:** `sanitizeForDisplay`, `include/StringUtils.h:163`, statement at line **209**.
- **From:** UTF-8 (decoded inline to a codepoint by the loop at lines 170-176).
- **To:** ASCII, with a hand-written fold table for ~40 specific codepoints (smart quotes, dash
  variants, ellipsis, bullet, space variants, zero-width, æ/œ, and a partial set of accented
  vowels). Everything else 3-byte or wider becomes `'?'`.
- **Fixed or environmental:** **fixed.** Compile-time constant behaviour, no runtime input.

Every `WideCharToMultiByte`/`MultiByteToWideChar` call in the tree uses `CP_UTF8` — verified across
`include/PortUtil.h`, `include/StringUtils.h`, `src/CDRipper.cpp`, `src/Mp4Chapters.cpp`,
`src/platform/win/{HttpWinInet,MediaControlSmtc,PluginLoaderWin}.cpp`, `src/UIManager.cpp:184`.
`src/main.cpp:150-151` sets `SetConsoleOutputCP/SetConsoleCP(CP_UTF8)`. **No ANSI-codepage
conversion exists anywhere in the tree.** The brief's hypothesis was reasonable from the symptom and
is not what is happening.

**Call-site count:** ~95 across `src/`, `include/`, `plugins/stream/`. This is not a one-line site.

## The function is doing what it was written to do

`sanitizeForDisplay` predates the column-aware UTF-8 pipeline. Its `'?'` catch-all was the correct
answer when the draw path could not render non-ASCII at all. `include/StringUtils.h:222-223` already
records the split that has since opened:

> They do NOT fold to ASCII — `sanitizeForDisplay` is the lossy fallback; these preserve the
> codepoints.

**The behaviour is pinned by a test.** `tests/playlist_encoding_test.cpp:84-92` asserts the fold as
"the honest limit," and `:82` asserts `Don\xE2\x80\x99t → Don't` — a 3-byte codepoint that must
keep folding. Any change here is a test change, deliberately.

Two headers also depend on the current contract in writing: `include/LibraryIndex.h:101-103`
(index stores tag text raw *because* the draw path folds) and `include/PodcastChapters.h:71-83`
(composes with the fold, and notes it passes ASCII control bytes through).

---

# C-R3 — What the seam does

**`CursesSeam.h` is not implicated. Nothing here requires changing it, and I am proposing
nothing about it.**

The seam is correct as written. `NCURSES_WIDECHAR` is defined before the include on every platform
(`CursesSeam.h:33-35`); under `REMOCT_PDCURSES` it defines `PDC_WIDE` and `PDC_FORCE_UTF8` before
`<curses.h>` (`:44-50`). The wide API is requested, available, and used.

## Output functions per path

| path | call | variant | wide available? |
|---|---|---|---|
| cwd line | `mvwaddnwstr` (`UIManager.cpp:2999`) | wide | yes, used |
| pane header, Classic | `mvwaddnstr` (`:3122`) | narrow | yes; narrow decodes UTF-8 under `PDC_FORCE_UTF8` |
| pane header, Awesome | `mvwaddnstr` (`:976`, via `panelFrame`) | narrow | same |
| browser row | `mvwaddnwstr` (`:3242`) | wide | yes, used |
| browser row icon overlay | `setcchar` + `mvwadd_wch` (`:3246-3247`) | wide | yes, used |
| playlist row | `mvwaddnwstr` (`:3446`) | wide | yes, used |

**Nothing is compiled out.** Both failing paths already use the wide variant. They would render
水田直志 correctly today if the string reaching them still contained it.

## PDCursesMod build configuration (Windows)

Vendored at `lib/pdcursesmod/`, pinned `d9a5983` (`VENDOR.md`), built by
`CMakeLists.txt:289-308` as static lib `pdcurses_wingui` from `pdcurses/*.c` + the 7 `wingui/*.c`,
with `target_compile_definitions(pdcurses_wingui PRIVATE PDC_WIDE PDC_FORCE_UTF8)`
(`CMakeLists.txt:304`). `remoct` gets `REMOCT_PDCURSES` at `:337`.

Relevant consequences:

- **`chtype` is 64-bit.** No `CHTYPE_16`/`CHTYPE_32` is defined, so `curses.h:145-152` takes the
  `uint64_t` branch and, because `PDC_WIDE` is set, defines `USING_COMBINING_CHARACTER_SCHEME`.
- **`MAX_UNICODE = 0x110000`** (`curspriv.h:133`) — full Unicode range, not the 0xFFFF fallback.
- **`PDC_wcwidth` is PDCursesMod's own Kuhn-derived table** (`pdcurses/addch.c:209+`), *not* the
  system `wcwidth` — `HAVE_WCWIDTH` is not defined by our build, so the `:136` branch is dead.
  The table was updated to Unicode 17.0.0 upstream and has 101 fullwidth ranges (`addch.c:346`).
- **Fullwidth cells are handled**: `addch.c:686-690` appends `DUMMY_CHAR_NEXT_TO_FULLWIDTH` after a
  width-2 character, and `wingui/pdcdisp.c:482-483` widens the GDI clip rect by one cell for it.
- **UTF-8 on the narrow path**: `pdcurses/util.c:386` — `PDC_mbtowc` decodes UTF-8 directly under
  `PDC_FORCE_UTF8`, so `mvwaddnstr` is UTF-8-correct, locale-independent.

**ncursesw (Linux):** `CMakeLists.txt:310-314` links `ncursesw` (+ `panelw`). `NCURSES_WIDECHAR 1`
from the seam. `wchar_t` is 32-bit, so `utf8_to_wide` (`StringUtils.h:321-327`) yields one whole
codepoint per `wchar_t` and width comes from glibc `wcwidth`, which is 2 for CJK.

**Both curses builds are fully capable of rendering this text.** Neither is the problem.

---

# C-R4 — Linux

**No. Linux fails identically. This is not a Windows or PDCursesMod issue.**

Measured — the same probe, built and run under WSL2 Trixie:

```
linux sanitized=[????] dispWidth=8 sizeof(wchar_t)=4 wide_len=4
```

`sanitizeForDisplay` sits in `include/StringUtils.h` with no platform gate, is compiled into both
builds from the same source, and both call sites (`UIManager.cpp:10028`,
`PlaylistManager.cpp:81-82`) are outside every `#ifdef`. Windows and Linux produce byte-identical
folded output.

**This does not localise to the Windows build.** Whatever is decided here lands on both platforms
at once — which is the good news: one fix, one behaviour, no twin.

---

# C-R5 — Column width

**Already correct, and already agreed on by both renderers. This is not a second problem waiting.**

## What the layout does today

`include/StringUtils.h:331-363`, `cpWidth`, returns **2** for: Hangul Jamo, U+2E80–U+303E,
U+3041–U+33FF (kana through CJK compat), CJK Ext-A, **U+4E00–U+9FFF (CJK Unified — where 水田直志
lives)**, Yi, Hangul syllables, CJK compat ideographs, vertical/small/fullwidth forms, emoji, and
CJK Ext-B+. Zero for combining marks and zero-width. One otherwise.

Measured: `dispWidth("水田直志") == 8` on both toolchains — four glyphs, two columns each. Correct.

Every layout helper is built on it and is codepoint-aligned, never byte-wise:
`truncateToWidth` (`:382`), `truncateToWidthRight` (`:398`), `padToWidth` (`:415`), `scrollToWidth`
(`:427`, which even renders a space when a wide glyph would straddle the right edge so the field
width stays exact). Both failing row paths already use them — `UIManager.cpp:3239, 3241` and
`:3442, 3445`.

## Does the renderer agree?

- **PDCursesMod:** yes. Own `PDC_wcwidth` table returns 2 for CJK; `addch.c:686-690` reserves the
  second cell; `pdcdisp.c:482` gives GDI the extra pixel column.
- **ncursesw:** yes. glibc `wcwidth` returns 2 for CJK.

The `n` passed to `mvwaddnwstr` is a `wchar_t` count while the layout budget is columns — these
stay consistent only because `padToWidth` guarantees the string is exactly `cw` columns before
widening. That invariant holds at both sites today.

**Conclusion: once the characters survive to the draw call, the rows lay out correctly.** No column
work is required by this. Two residual risks, neither triggered by this album:

1. **Astral characters on Windows.** `utf8_to_wide` (`StringUtils.h:100-107`) is UTF-16, so a
   codepoint above U+FFFF becomes a surrogate *pair* — two `wchar_t` where `cpWidth` counted one
   glyph of 2 columns. Already documented as an accepted limit (`StringUtils.h:95-96`, lessons.md).
   All CJK in scope here is BMP, so it is untouched by this. Emoji in tags would hit it.
2. **Table drift.** `cpWidth` is hand-rolled and independent of `PDC_wcwidth`/glibc by design
   ("keeps the Linux port byte-identical", `StringUtils.h:218-221`). Any range where the two
   disagree shifts a row by one cell. They agree on CJK Unified, kana and Hangul, which is what this
   album needs.

---

# Noticed in passing

- **Ripping is unaffected.** `CDRipper` reads the raw `MBRelease` (`CDRipper.cpp:173-176, 753,
  1763, 2428-2430`), never the sanitized display titles, and `sanitizePathComponent`
  (`StringUtils.h:31-38`) only rewrites bytes `< 0x32` and the nine Windows-illegal ASCII
  characters — every UTF-8 byte of 水田直志 is ≥ 0x80 and passes through. **Files and tags from a
  Japanese-titled disc are written with the real text.** The damage is display-only.
- **Two header paths measure columns in bytes.** `UIManager.cpp:3121` (`bar.resize((size_t)cols,
  ' ')`) and `:974` (`t.substr(0, cols - tx - 2)`) treat a byte count as a column count and can cut
  a multi-byte sequence mid-character. Both are on the *header* path — the one that currently
  works — so a long CJK folder name is where they would show. Pre-existing, unrelated to the fold.
- **`UIManager.cpp:961-962` carries a stale comment** — "the narrow path on this build doesn't
  decode UTF-8". True for the ncursesw build it was written against; under `PDC_FORCE_UTF8` the
  narrow path does decode (`pdcurses/util.c:386`). The wide-API code it justifies is correct
  regardless.
- **`sanitizeForDisplay` passes ASCII control bytes through**, pinned deliberately at
  `tests/playlist_encoding_test.cpp:90-92` and worked around in `PodcastChapters.h:71-83`.
