# RE-MOCT - Music On Console Terminal

**RE-MOCT** is a terminal music player, CD ripper, and internet-radio client for
**Windows and Linux**, written in C++20 on ncurses, miniaudio, TagLib, libFLAC, LAME,
libebur128, FDK-AAC, libopus, libvorbis, and libwavpack.

It is a homage to [MOC](http://moc.daper.net/) (Music On Console) with a twist. In
**Classic mode** it stays faithful and minimal. Hit **Ctrl+T** for **Awesome mode** and
it becomes *RE-MOCT* - the remix: a comet progress bar, a sub-cell block visualizer, and
breathing animations. The mode toggle is the whole point.

<br>
<img width="1913" height="1057" alt="image" src="https://github.com/user-attachments/assets/2d133216-930e-4d65-9fce-bee538153927" />
<br><br>
<img width="1919" height="1077" alt="image" src="https://github.com/user-attachments/assets/336ef899-7e2e-4cf3-bf34-0fbf4e4ce6e6" />
<br>

## Screenshots

See [`docs/screenshots/`](docs/screenshots/) - the 256-color visualizer (Awesome mode)
and a playlist/rip view. The public feature guide is [`docs/index.html`](docs/index.html).

## Features

**Local playback**
- MP3, FLAC, Opus, Ogg Vorbis, WavPack, WAV, AAC/HE-AAC, and `.m4b` audiobooks
  (chapter navigation)
- Gapless playback, configurable crossfade, varispeed
- Repeat (track/all), shuffle, seek, volume, 10-band equalizer
- ReplayGain tag support (including Opus R128); per-track play counts
- LRC lyrics, tag editor (via TagLib), queue, bookmarks, favorites (star key), goto bar
- Split-pane UI: directory browser + playlist/views; full-text playlist search (`\`)
- Page navigation (`PgUp`/`PgDn`/`Home`/`End`), cursor position readout in the playlist
  header (`[3/12]`), optional per-row file-type column (`Shift+F`)

**Convert & library**
- Convert files to another format (`x`) - a single file, every file in a folder,
  or a marked set (`u` marks, `U` clears); reuses the rip encoders and carries the
  source tags plus embedded cover art to the new file
- Batch ReplayGain over a folder (`Ctrl+O`): compute and write track gain for every
  supported file, using the same loudness math as the CD ripper

**CD playback & ripping** (Ctrl+Y)
- Red Book CD playback + MusicBrainz disc lookup (Ctrl+R)
- Eject from the TUI (Shift+E) and drive-list refresh for hot-plugged drives (F12)
- Four rip modes:
  - **[A] AccurateRip** - network CRC verify against accuraterip.com + drive-offset correction
  - **[C] CUETools Database** - offset-immune whole-disc CRC32 (cue.tools/db)
  - **[Y] Local** - best-effort offline rip
  - **[B] Local 2-pass** - best-effort plus a read-twice determinism check
- Selectable output - **FLAC, MP3, WAV, Opus, WavPack, M4A** (AAC-LC), any
  combination - with per-format quality on a per-row editor (FLAC level; MP3
  V-scale or a CBR bitrate; Opus bitrate; AAC VBR 1-5 ladder or a CBR bitrate;
  CBR/VBR toggle). M4A uses the bundled FDK-AAC encoder, so no extra library
  ships. Every tagged format carries embedded cover art
  (Cover Art Archive) and EBU R128 ReplayGain tags. C2 error-pointer detection,
  dual-pass on mismatch, per-rip logs (with lossless-master vs lossy-derived notes)

**Internet radio & streaming**
- RadioBrowser (radio-browser.info) station search (Ctrl+U to add by URL)
- ICY/SHOUTcast streaming with live StreamTitle metadata
- iHeartRadio via HLS, with now-playing reconciliation and a digital (web-player) path
- iHeart ad re-pin control (`F6`, iHeart streams only): `off` never re-pins,
  `ad-escape` re-pins only on hard ad evidence (a paid spot id or spot churn),
  `hybrid` (default) re-pins on that evidence or when the stalled window actually
  contains ad segments - so a long talk show is ridden out instead of thrashed -
  and `timed` is the legacy duration-only escape. Independent of the `Ctrl+K` feed
  toggle (web-player vs raw broadcast); pressing either flashes the current
  `<feed> - <repin>` mode in the status bar for a few seconds
- Record the playing stream to disk (`Ctrl+E`): re-encode to Opus, MP3, or M4A, or
  an as-broadcast copy mode (no re-encode - the better choice for an AAC broadcast);
  per-song split from the station's metadata, a pulsing `[REC]` badge, cover-art per
  cut, a split-hold that keeps outros, and ad-aware routing; recording continues
  gaplessly through a playback pause

**Scrobbling & presence**
- Last.fm (Ctrl+G) and ListenBrainz (Ctrl+B) scrobbling + now-playing
- Discord Rich Presence with album art
- OS media controls (on by default): the now-playing title, artist, and cover appear
  on the operating system's own media surface - Windows SMTC (volume/lock-screen
  overlay, media keys, scrubber) and Linux MPRIS (playerctl, desktop widgets)

**Interface & visuals**
- Two modes: **Classic** (a faithful MOC homage) and **Awesome** (**Ctrl+T**) - comet
  progress bar, breathing animations, a full-width spectrum
- Spectrum styles (**F2**): classic solid bars, or an 80s graphic-EQ "LED" look; the
  bars fill the full width at any size
- The spectrum is a real FFT (since 1.2.0): accurate top-to-bottom with no aliasing,
  per-band normalization and a perceptual tilt - it shows each track's real mastering
- Cover art in the Track Info (**i**) pane for local files and radio (half-block render,
  station cover / iTunes-Deezer lookup / logo floor)
- 18 named truecolor Awesome palettes, cycled with **F7** / **F8**; a KITT scanner in
  the radio status bar
- Optional Windows GDI (wingui) build: truecolor window, remembered size, **Alt+Enter**
  borderless fullscreen (see [BUILD.md](BUILD.md))

**Cross-platform & plugin architecture**
- Runs on **Windows** (MSYS2 UCRT64) and **Linux** (Debian Trixie); every platform
  call sits behind a seam with a Windows and a Linux implementation
- The streaming source is a **real loadable plugin** (`remoct_stream.{so,dll}`) driven
  through a frozen C ABI - a streaming fix ships as a rebuilt plugin, no host rebuild

## Build

See **[BUILD.md](BUILD.md)** for full per-platform instructions (Windows/MSYS2 and
Debian/Trixie). In short, on an MSYS2 UCRT64 shell with the toolchain installed:

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

Binary: `build/bin/remoct.exe` (Windows) / `build/bin/remoct` (Linux), with the streaming
plugin built beside it in `build/bin/plugins/`. Windows has two render backends - the
console `ncursesw` build (default) and a GDI **wingui** build (`-DREMOCT_PDCURSES=ON`,
truecolor + Alt+Enter fullscreen); see [BUILD.md](BUILD.md).

## Keybindings (selection)

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `Tab` | Toggle DirBrowser ↔ Playlist | `Ctrl+T` | Toggle Classic / Awesome mode |
| `Enter` | Play file / enter directory | `Ctrl+U` | Add / play a stream by URL |
| `Space` | Pause / Resume | `Ctrl+R` | CD → MusicBrainz metadata |
| `n` / `p` | Next / Previous | `Ctrl+Y` | Open CD rip panel |
| `[` / `]` | Seek back / forward | `Ctrl+G` / `Ctrl+B` | Last.fm / ListenBrainz login |
| `-` / `+` | Volume down / up | `Ctrl+D` | Discord Rich Presence toggle |
| `*` | Star / unstar (FAVs) | `Ctrl+A` | iHeart deep-analysis log toggle |
| `?` | Help pane | `Ctrl+P` | iHeart minted-profileId probe (experimental, off by default) |
| `Ctrl+Q` | Quit | `Ctrl+K` | Stream mode: Web Player / Raw broadcast |
| `i` / `e` | Track info / 10-band EQ | `Shift+L` / `Shift+A` / `Shift+X` | Lyrics / About / Output device picker |
| `F2` | Spectrum: classic / 80s LED | `F7` / `F8` | Awesome theme: previous / next |
| `K` / `J` | Move track up / down | `F3` | Follow the playing track (default on) |
| `PgUp` / `PgDn` / `Home` / `End` | Page / top-bottom jump | `\` | Playlist search |
| `Shift+F` | File-type column toggle | `Shift+E` | Eject CD drive (in `[Drives]`) |
| `F12` | Refresh drive list | `;` | Audiobook chapter list |
| `x` / `u` / `U` | Convert / mark / clear marks | `Ctrl+E` | Record playing stream |
| `Ctrl+O` | Batch ReplayGain (normalize folder) | `Alt+Enter` | Fullscreen (Windows wingui) |
| `Ctrl+N` | Nerd Font title icons toggle | `F6` | iHeart re-pin mode: off / ad-escape / hybrid / timed |

## Configuration

Config file:
- **Windows:** `%APPDATA%\RE-MOCT\remoct.conf`
- **Linux:** `~/.config/RE-MOCT/remoct.conf`

> ⚠ **`remoct.conf` stores scrobbling credentials in plaintext** (Last.fm session key,
> ListenBrainz token). It is machine-local; keep it private and do not commit it - it is
> gitignored. RE-MOCT transmits no audio and stores no third-party data beyond what a
> request needs.

```ini
volume=5
repeat=all          # off / track / all
shuffle=0
crossfade=1
crossfade_ms=2000
theme=dark
fav=C:\Music\No Doubt
bookmark=C:\Music\Lossless
```

## Fonts and Nerd Font icons

Font selection works **differently on each platform**, because the two builds render
differently. The one thing to know: **on Windows RE-MOCT picks its own font (the
`wingui_font` config key); on Linux it uses your terminal emulator's font.** They are
not the same mechanism, and a terminal font set on Windows has no effect.

### Windows (wingui build)

RE-MOCT draws into its own GDI window and chooses its **own** font. Your PowerShell,
cmd, or Windows Terminal font setting does **not** affect it - changing the terminal
font does nothing.

- **Out of the box:** the default is a bundled JetBrains Mono Nerd Font, so the title
  icons, rounded panel corners, and visualizer blocks all render with no setup.
- **To use a different font:** set `wingui_font` in `%APPDATA%\RE-MOCT\remoct.conf` to
  the exact GDI face name (the key is written to the config by default, so you can find
  it there). Tested example:

  ```ini
  wingui_font=3270 Nerd Font Mono
  ```

  Use the **Mono** / **NFM** width variant so glyphs stay single-cell. An empty
  `wingui_font=` keeps the bundled JetBrains Mono default.
- **Get the exact face name** (the GDI family name, not the filename) from Settings >
  Fonts, or PowerShell:

  ```powershell
  Add-Type -AssemblyName System.Drawing
  (New-Object System.Drawing.Text.InstalledFontCollection).Families |
    Where-Object { $_.Name -like "*Nerd*" } | Select-Object Name
  ```

- If the font is not installed system-wide, drop its `.ttf`/`.otf` into a `fonts\`
  folder beside `remoct.exe` and RE-MOCT loads it privately (no install needed).
- **Relaunch RE-MOCT to apply** - the font is chosen before the screen opens, so a
  redraw is not enough.

### Linux (ncursesw build)

RE-MOCT renders through your **terminal emulator**, so it uses **the terminal's font**
(Alacritty, kitty, GNOME Terminal, and so on). `wingui_font` does nothing on Linux.

- Install a Nerd Font and set your terminal emulator to use it (the **Mono** variant is
  recommended). Any mainstream Nerd Font works.
- The terminal font must also carry **box-drawing and block glyphs**, or the panel
  borders and the visualizer render as empty boxes. This is a separate requirement from
  the optional Nerd icons - mainstream Nerd Fonts include all of it.

### Both platforms

- Any mainstream Nerd Font works: JetBrains Mono, Hack, FiraCode, Meslo, Cascadia Code.
  Unusual patches (for example 3270) can render icons poorly - that is the font, not
  RE-MOCT.
- `Ctrl+N` toggles the Nerd Font title icons. **Without a Nerd Font, toggle icons off
  with `Ctrl+N` and RE-MOCT displays fine** - the icons are optional and every one
  falls back to plain text when off.

## Documentation

- **[BUILD.md](BUILD.md)** - building on Windows and Linux
- **[CHANGELOG.md](CHANGELOG.md)** - release notes
- **[CONTRIBUTING.md](CONTRIBUTING.md)** - conventions and discipline
- **[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)** - dependency licenses & obligations
- **[docs/](docs/)** - architecture, AccurateRip pipeline, streaming internals, the
  iHeart case study ([docs/IHeartRadio/](docs/IHeartRadio/)), and reference rip logs
  ([docs/samples/](docs/samples/))
- **[tools/](tools/)** - standalone educational protocol/timing probes

## License

RE-MOCT is released under the **MIT License** - see [LICENSE](LICENSE). It links and
vendors third-party components under their own licenses; redistribution obligations
(notably FDK-AAC, TagLib, and LAME) are documented in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
