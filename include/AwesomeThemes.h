#pragma once
// Named truecolor palettes for Awesome mode (Ctrl+T selects Awesome; F8 cycles
// these). Portable, no platform code: UIManager::applyAwesomeTheme() consumes
// this table on both Linux and Windows. Classic mode is unaffected and keeps its
// theme.conf-driven 8-colour pairs.
//
// Fifteen semantic colours per theme, packed 0xRRGGBB. The semantic -> colour
// pair mapping lives in applyAwesomeTheme() (UIManager.cpp), matching the design
// spec's section 4.2. See section 4.1 for the source palettes.

struct AwesomeTheme {
    const char* name;
    // Packed 0xRRGGBB. Field order matches the section 4.1 semantic rows.
    unsigned base;      // solid pane background (Awesome-only)
    unsigned text;      // primary foreground text
    unsigned title;     // pane titles / headings
    unsigned focus;     // focused-row background accent
    unsigned accent;    // selected-row background accent
    unsigned selfg;     // foreground on focus/accent fills (== base per palette)
    unsigned prog;      // progress bar fill
    unsigned ok;        // status: ok
    unsigned err;       // status: error
    unsigned border;    // pane borders
    unsigned dim;       // dimmed/secondary text
    unsigned viz_low;   // visualizer band 1
    unsigned viz_mid;   // visualizer band 2
    unsigned viz_high;  // visualizer band 3
    unsigned viz_peak;  // visualizer band 4 / peak
};

// Order is also the F8 cycle order. Index 0 (Chiba) is the shipped default.
static constexpr AwesomeTheme kAwesomeThemes[] = {
    // name          base      text      title     focus     accent    selfg     prog      ok        err       border    dim       viz_low   viz_mid   viz_high  viz_peak
    { "Chiba",        0x1a1b26, 0xc0caf5, 0x7aa2f7, 0x7dcfff, 0xbb9af7, 0x1a1b26, 0x7aa2f7, 0x9ece6a, 0xf7768e, 0x3b4261, 0x565f89, 0x9ece6a, 0xe0af68, 0x7dcfff, 0xbb9af7 },
    { "Arrakis",      0x282828, 0xebdbb2, 0xfabd2f, 0x83a598, 0xd3869b, 0x282828, 0xfabd2f, 0xb8bb26, 0xfb4934, 0x504945, 0x928374, 0xb8bb26, 0xfabd2f, 0xfe8019, 0xfb4934 },
    { "Mauveine",     0x1e1e2e, 0xcdd6f4, 0xcba6f7, 0x89dceb, 0xf5c2e7, 0x1e1e2e, 0xcba6f7, 0xa6e3a1, 0xf38ba8, 0x45475a, 0x6c7086, 0xa6e3a1, 0xf9e2af, 0x89b4fa, 0xcba6f7 },
    { "Niflheim",     0x2e3440, 0xeceff4, 0x88c0d0, 0x8fbcbb, 0x81a1c1, 0x2e3440, 0x88c0d0, 0xa3be8c, 0xbf616a, 0x434c5e, 0x616e88, 0xa3be8c, 0xebcb8b, 0x81a1c1, 0xb48ead },
    { "Nyarlathotep", 0x282a36, 0xf8f8f2, 0xbd93f9, 0x8be9fd, 0xff79c6, 0x282a36, 0xbd93f9, 0x50fa7b, 0xff5555, 0x44475a, 0x6272a4, 0x50fa7b, 0xf1fa8c, 0x8be9fd, 0xff79c6 },
    { "Zero Cool",    0x0d0b06, 0xffcf7a, 0xffb000, 0xffe08a, 0xff8c1a, 0x0d0b06, 0xffb000, 0xc6d40a, 0xff4a3d, 0x4d3a12, 0x8a6a24, 0x6b4d0f, 0xc07f10, 0xffb000, 0xffe08a },
    // DOS: the original RE-MOCT look - authentic IBM PC / CGA-VGA text-mode 16
    // colours. Light-gray body on black, white titles, yellow-on-blue now-playing
    // selection, green/red status, cyan borders. Every value is an exact ANSI-16
    // slot, so the nearest-ANSI fallback renders it identically to truecolor.
    // name          base      text      title     focus     accent    selfg     prog      ok        err       border    dim       viz_low   viz_mid   viz_high  viz_peak
    { "DOS",          0x000000, 0xaaaaaa, 0xffffff, 0x0000aa, 0x0000aa, 0xffff55, 0x00aaaa, 0x55ff55, 0xff5555, 0x00aaaa, 0xaaaaaa, 0x0000aa, 0xaa5500, 0x5555ff, 0xffff55 },
};

static constexpr int kNumAwesomeThemes =
    (int)(sizeof(kAwesomeThemes) / sizeof(kAwesomeThemes[0]));
