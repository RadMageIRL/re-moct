#pragma once
// CoverArtRender - decode cover-art bytes and render them to a half-block cell
// grid for the Info pane. PURE: no curses, no globals, no I/O, never throws - the
// caller (UIManager) owns the curses colour allocation and drawing. Shared core,
// identical on Linux (ncursesw) and Windows (PDCurses/wingui): a half-block cell
// is just characters + colours, which wingui renders natively (no sixel/kitty).
#include <cstdint>
#include <vector>

namespace cover {

// One rendered cell = glyph U+2580 (UPPER HALF BLOCK) with the TOP pixel as the
// foreground colour and the BOTTOM pixel as the background colour, giving two
// vertical samples per character cell.
struct Cell {
    unsigned char r_top, g_top, b_top;
    unsigned char r_bot, g_bot, b_bot;
};

struct Rendered {
    bool ok   = false;   // false => no art (decode failed / empty bytes)
    int  cols = 0;       // filled grid width in cells (<= box_cols, aspect-fit)
    int  rows = 0;       // filled grid height in cells (<= box_rows, aspect-fit)
    std::vector<Cell> cells;   // rows*cols, row-major (index cy*cols + cx)
};

// Decode `bytes` (JPEG/PNG/BMP/...) and render to a half-block grid that FITS
// within box_cols x box_rows CELLS, preserving the source aspect ratio
// (letterboxed, never stretched - a stretched cover reads as obviously wrong).
// Returns ok=false on any failure; never throws.
Rendered render(const std::vector<uint8_t>& bytes, int box_cols, int box_rows);

}  // namespace cover
