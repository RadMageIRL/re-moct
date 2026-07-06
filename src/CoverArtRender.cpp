// CoverArtRender - see CoverArtRender.h. Decodes with the vendored stb_image
// (this is the ONE translation unit that compiles its implementation) and does a
// small box-filter downscale to the half-block grid. No curses here.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO           // we only decode from memory
#define STBI_NO_LINEAR          // no HDR float path
#define STBI_NO_HDR
#include "stb_image.h"

#include "CoverArtRender.h"
#include <algorithm>

namespace cover {

Rendered render(const std::vector<uint8_t>& bytes, int box_cols, int box_rows) {
    Rendered out;
    if (bytes.empty() || box_cols < 1 || box_rows < 1) return out;

    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(
        bytes.data(), (int)bytes.size(), &w, &h, &ch, 3);   // force RGB
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return out; }

    // Two vertical samples per cell -> the available sample grid is
    // box_cols x (box_rows*2). Fit the source into it preserving aspect (so a
    // square cover stays square; a non-square one letterboxes, never stretches).
    const int box_sy = box_rows * 2;
    const double s = std::min((double)box_cols / w, (double)box_sy / h);
    int gw  = std::clamp((int)(w * s + 0.5), 1, box_cols);   // cells wide
    int gsy = std::clamp((int)(h * s + 0.5), 1, box_sy);     // samples tall
    const int grows = (gsy + 1) / 2;                          // cells tall (round up)

    // Average the source region mapping to sample (gx, gy) - a plain box filter,
    // fine for downscaling a photo to this tiny grid.
    auto sampleAt = [&](int gx, int gy, int& r, int& g, int& b) {
        int x0 = gx * w / gw,  x1 = (gx + 1) * w / gw;   if (x1 <= x0) x1 = x0 + 1;
        int y0 = gy * h / gsy, y1 = (gy + 1) * h / gsy;  if (y1 <= y0) y1 = y0 + 1;
        long R = 0, G = 0, B = 0, n = 0;
        for (int yy = y0; yy < y1 && yy < h; ++yy)
            for (int xx = x0; xx < x1 && xx < w; ++xx) {
                const unsigned char* p = px + ((size_t)yy * w + xx) * 3;
                R += p[0]; G += p[1]; B += p[2]; ++n;
            }
        if (n == 0) n = 1;
        r = (int)(R / n); g = (int)(G / n); b = (int)(B / n);
    };

    out.cols = gw;
    out.rows = grows;
    out.cells.resize((size_t)gw * grows);
    for (int cy = 0; cy < grows; ++cy)
        for (int cx = 0; cx < gw; ++cx) {
            int rt, gt, bt, rb, gb, bb;
            sampleAt(cx, cy * 2, rt, gt, bt);
            const int by = cy * 2 + 1;
            if (by < gsy) sampleAt(cx, by, rb, gb, bb);
            else { rb = rt; gb = gt; bb = bt; }   // odd final row: bottom == top
            Cell& c = out.cells[(size_t)cy * gw + cx];
            c.r_top = (unsigned char)rt; c.g_top = (unsigned char)gt; c.b_top = (unsigned char)bt;
            c.r_bot = (unsigned char)rb; c.g_bot = (unsigned char)gb; c.b_bot = (unsigned char)bb;
        }

    stbi_image_free(px);
    out.ok = true;
    return out;
}

}  // namespace cover
