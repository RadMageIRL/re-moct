# Vendored: stb_image.h

Upstream: https://github.com/nothings/stb (raw: stb_image.h)
Version: v2.30 (see the banner at the top of stb_image.h)
Vendored: 2026-07-04, for the cover-art render in the Info (i) pane.
License: public domain (dual MIT / Unlicense - see the end of stb_image.h).

Single-header image decoder (`stbi_load_from_memory` -> RGB) used by
`src/CoverArtRender.cpp` to decode embedded JPEG/PNG cover art. Folds into the
static exe (no DLL). We do NOT vendor stb_image_resize2 - CoverArtRender does its
own small box-filter downscale to the tiny half-block grid.

The implementation is compiled exactly once, in CoverArtRender.cpp
(`#define STB_IMAGE_IMPLEMENTATION`); everywhere else it's header-only.

Update: re-fetch the raw header at the new version and bump the note.
