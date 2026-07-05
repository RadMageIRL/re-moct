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
// Indices 0..6 are unchanged from the original set so a saved awesome_theme
// index keeps pointing at the same theme; new palettes are appended only.
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

    // ---- The other ten vocabularies (appended; F8 cycles through these after DOS) ----
    // Each is one name drawn from an unused theme vocabulary, colours matched to
    // its world. selfg == base per palette (dark text on the bright focus/accent
    // fills), same convention as above. Truecolor renders exact; nearest-256/16
    // fallback approximates.
    // name          base      text      title     focus     accent    selfg     prog      ok        err       border    dim       viz_low   viz_mid   viz_high  viz_peak
    { "Tiamat",       0x0f0a0e, 0xe8d9c0, 0xd4af37, 0xc0392b, 0x7d3c98, 0x0f0a0e, 0xd4af37, 0x2ecc71, 0xe74c3c, 0x4a3b2a, 0x8a7a5c, 0x2980b9, 0x2ecc71, 0xd4af37, 0xc0392b }, // D&D - chromatic dragon, jewel tones
    { "LCARS",        0x000000, 0xffcc99, 0xff9900, 0xcc99cc, 0x9999ff, 0x000000, 0xff9900, 0x66cc88, 0xcc6666, 0xcc6633, 0x886688, 0x9999ff, 0xcc99cc, 0xffcc66, 0xff9900 }, // Star Trek - Okudagram, orange on black
    { "Mustafar",     0x0a0503, 0xe8b298, 0xff4500, 0xff6b1a, 0xb71c1c, 0x0a0503, 0xff4500, 0x9aae10, 0xff2200, 0x4a1508, 0x7a3520, 0x7a1010, 0xb71c1c, 0xff4500, 0xffb020 }, // Star Wars - molten Sith on obsidian
    { "Tux",          0x1c1c1c, 0xe6e6e6, 0xe9820c, 0xf0a020, 0xe9820c, 0x1c1c1c, 0xe9820c, 0x7cb342, 0xe53935, 0x3a3a3a, 0x7a7a7a, 0x7cb342, 0xcddc39, 0xf0a020, 0xffffff }, // Linux Kernel - penguin charcoal/orange
    { "Synergy",      0x1b2431, 0xc5ced9, 0x2f81f7, 0x4a9eff, 0x1f6feb, 0x1b2431, 0x2f81f7, 0x3fb950, 0xf85149, 0x30363d, 0x6e7681, 0x1f6feb, 0x2f81f7, 0x58a6ff, 0x79c0ff }, // Corp Speak - enterprise blue, deliberately soulless
    { "Tortuga",      0x0d1512, 0xe8d8b0, 0xd4a017, 0x1abc9c, 0x16a085, 0x0d1512, 0xd4a017, 0x2a9d5c, 0xc0392b, 0x4a3b28, 0x8a7a55, 0x16a085, 0x1abc9c, 0xd4a017, 0xf0c040 }, // Pirate - doubloon gold + sea teal
    { "Firelink",     0x14110d, 0xb8ac96, 0xd97a2b, 0xe8944a, 0xa85422, 0x14110d, 0xd97a2b, 0x8a9a5b, 0xa13b2e, 0x3a352c, 0x6b6455, 0x5a5040, 0x8a6a3a, 0xd97a2b, 0xf0b060 }, // Dark Souls - ash-grey + bonfire ember
    { "Tyrian",       0x12080f, 0xe6d3b3, 0xd4af37, 0x9b2d6f, 0x6a1b9a, 0x12080f, 0xd4af37, 0x6b8e23, 0xb71c1c, 0x4a2b3a, 0x8a6b7a, 0x6a1b9a, 0x9b2d6f, 0xc04070, 0xd4af37 }, // Latin - imperial purple + gold
    { "Lothlorien",   0x0f1410, 0xdce8cf, 0xd4c26a, 0xa3c586, 0xc9b458, 0x0f1410, 0xd4c26a, 0x7fb069, 0xc07b5a, 0x3a4a32, 0x7a8a6a, 0x7fb069, 0xa3c586, 0xc9b458, 0xe8dca0 }, // Tolkien - elvish gold-green, mallorn
    { "Camelot",      0x0b1020, 0xd8dce8, 0xd4af37, 0x3f6fd4, 0xc0392b, 0x0b1020, 0xd4af37, 0x4a9e6a, 0xd13b3b, 0x2a3a5a, 0x6a7a9a, 0x3f6fd4, 0x5a8ae0, 0xd4af37, 0xc0392b }, // Arthurian - heraldic royal blue + gold
};

static constexpr int kNumAwesomeThemes =
    (int)(sizeof(kAwesomeThemes) / sizeof(kAwesomeThemes[0]));
