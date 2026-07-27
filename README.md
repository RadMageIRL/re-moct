# RE-MOCT - Music On Console Terminal

**RE-MOCT** is a terminal music player, CD ripper, internet-radio client, and podcast
client for **Windows and Linux**, written in C++20 on ncurses, miniaudio, TagLib, libFLAC, LAME,
libebur128, FDK-AAC, libopus, libvorbis, and libwavpack.

It is a homage to [MOC](http://moc.daper.net/) (Music On Console) with a twist. In
**Classic mode** it stays faithful and minimal. Hit **Ctrl+T** for **Awesome mode** and
it becomes *RE-MOCT* - the remix: a comet progress bar, a sub-cell block visualizer, and
breathing animations. The mode toggle is the whole point.

<br>
<img width="1204" height="603" alt="image" src="https://github.com/user-attachments/assets/4ca8d52c-2ed9-4956-96d5-d7bf2763a535" />
<br><br>
<img width="1913" height="1057" alt="image" src="https://github.com/user-attachments/assets/2d133216-930e-4d65-9fce-bee538153927" />
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
- Split-pane UI: directory browser + playlist/views; focus-aware list search (`\`) that
  jumps to a match in whichever pane has focus - the playlist, or any browser view
  (directories, feeds, episodes, radio, books, favorites, recent, drives)
- Page navigation (`PgUp`/`PgDn`/`Home`/`End`), cursor position readout in the playlist
  header (`[3/12]`), optional per-row file-type column (`Shift+F`)

**Library**
- A `[Library]` section that lists every artist in your music folder regardless of how the
  folders are arranged. RE-MOCT stays a folder player - the directory browser is unchanged
  and still where everything starts - and this sits beside it for when you know the artist
  and not the folder
- Artist -> album -> track, with track numbers and lengths; the first open reads tags and
  shows its progress, after which it opens instantly and only re-reads files that changed
  (`F12` rescans, `Esc` cancels a running scan)
- Watches more than one folder (`@` adds one), so music on a second drive is one list with
  the rest. A folder that is offline is skipped, not emptied
- Search the whole collection with `|` - artists, albums and tracks narrow as you type
- Browse by genre with `g`; genres written two ways (`Post Punk`, `Post-Punk`) are one genre
- Two views on what you have actually played, with `%`

**Convert & library**
- Convert files to another format (`x`) - a single file, every file in a folder,
  or a marked set (`u` marks, `U` clears); reuses the rip encoders and carries the
  source tags plus embedded cover art to the new file
- Batch ReplayGain over a folder (`Ctrl+O`): compute and write track gain for every
  supported file, using the same loudness math as the CD ripper
- Transcode whole playlists from the convert pop-up (`x`): [4] converts every
  file in the playlist pane and [5] converts every file a focused playlist file
  references, through the same encoder and format picker as single files; output
  lands beside each source, stream/CD entries are skipped
- Save the playlist pane to a container (`Shift+S`): the format follows the
  name's extension (.m3u / .m3u8 / .pls / .xspf); with the browser cursor on a
  playlist file, `Shift+S` opens a pop-up to reformat that file into M3U8/PLS/XSPF
  (never overwriting), with the plain save still reachable; stream/CD entries are skipped

**CD playback & ripping** (Ctrl+Y)
- Red Book CD playback + MusicBrainz disc lookup (Ctrl+R)
- Eject from the TUI (Shift+E) and drive-list refresh for hot-plugged drives (F12)
- **Rips are byte-exact.** A RE-MOCT rip of a disc is byte-for-byte identical to the same
  disc ripped by dBpoweramp - verified across three albums, forty tracks, zero differing
  samples. The AccurateRip checksum is computed over the audio actually written to the
  file, not over a separate read, so a verification pass and the file it verifies cannot
  disagree
- **Rip only the tracks you want**: with a CD open, mark tracks in the playlist with `u`
  (`U` clears). Mark nothing and the whole disc is ripped exactly as before. Artifacts that
  describe the disc as a whole rather than the tracks you took - the cue sheet, album
  volume level, whole-disc CUETools verification - are left out of a partial rip, and the
  log says which and why
- Four rip modes:
  - **[A] AccurateRip** - network CRC verify against accuraterip.com + drive-offset correction
  - **[C] CUETools Database** - offset-immune whole-disc CRC32 (cue.tools/db), an online
    lookup; the verdict and the disc ID it was checked under are written into every
    track's tags, so a rip can be re-checked later without reading the disc again
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
  `timed` is the legacy duration-only escape, and `live-edge` follows the live edge
  the way the web player does (drift-triggered, always current - including ads; rides
  closer to the edge, so it is twitchier on a laggy connection, while the other modes
  keep more buffer and tolerate drift). Independent of the `Ctrl+K` feed
  toggle (web-player vs raw broadcast); pressing either confirms the new mode
  in yellow on the bottom status row for a few seconds
- Record the playing stream to disk (`Ctrl+E`): re-encode to Opus, MP3, or M4A, or
  an as-broadcast copy mode (no re-encode - the better choice for an AAC broadcast);
  per-song split from the station's metadata, a pulsing `[REC]` badge, cover-art per
  cut, a split-hold that keeps outros, and ad-aware routing; recording continues
  gaplessly through a playback pause

**Podcasts**
- Find new shows by searching the Podcast Index (`/`, with your own free API
  credentials), or subscribe directly by pasting a feed URL (`a`) when you already
  have one
- Two levels: subscribed feeds, then a feed's episodes, with per-episode state (new,
  part-played with position, played)
- Play an episode, or download it for offline (`Shift+D`) through a queue with a
  progress readout and retry; `d`/`Del` removes a feed, or deletes a download
- Resume where you left off; mark an episode played or unplayed by hand (`y`)
- Chapters from the episode file or published by the feed (`;` to browse, `,` / `.` to
  jump), with show or episode art in the Track Info pane and on the OS media card
- A client, not a platform: no refresh daemon, no gpodder sync, no OPML, no
  auto-download of new episodes, no auto-advance into your music, and podcasts never
  scrobble

**Scrobbling & presence**
- Last.fm (Ctrl+G) and ListenBrainz (Ctrl+B) scrobbling + now-playing
- Discord Rich Presence with album art
- OS media controls (on by default): the now-playing title, artist, and cover appear
  on the operating system's own media surface - Windows SMTC (volume/lock-screen
  overlay, media keys, scrubber) and Linux MPRIS (playerctl, desktop widgets),
  identified as RE-MOCT rather than a generic app name

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

On Linux, `./install.sh` builds and installs to a prefix (default `/usr/local`). It builds
the player and its streaming plugin only; `--with-tests` builds the test tools alongside
them, and `--tests-only` builds just the test tools and installs nothing. A plain
`cmake` build still includes the tests.

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
| `PgUp` / `PgDn` / `Home` / `End` | Page / top-bottom jump | `\` | Focus-aware list search |
| `Shift+F` | File-type column toggle | `Shift+E` | Eject CD drive (in `[Drives]`) |
| `F12` | Refresh drive list | `;` / `,` / `.` | Chapter list (books, files, episodes) / previous / next |
| `/` / `a` | Search Podcast Index / add feed by URL (in `[Podcasts]`) | `Shift+D` / `y` | Download episode / mark played-unplayed |
| `x` / `u` / `U` | Convert / mark / clear marks | `Ctrl+E` | Record playing stream |
| `Ctrl+O` | Batch ReplayGain (normalize folder) | `Alt+Enter` | Fullscreen (Windows wingui) |
| `Ctrl+N` | Nerd Font title icons toggle | `F6` | iHeart re-pin mode: off / ad-escape / hybrid / timed / live-edge |
| `\|` / `g` / `%` | Library: collection search / genres / play-stat views | `@` | Add a folder to the library |
| `~` | Reload `theme.conf` colours (live) | `F12` | Rescan library (in `[Library]`) |

## Configuration

Config file:
- **Windows:** `%APPDATA%\RE-MOCT\remoct.conf`
- **Linux:** `~/.config/RE-MOCT/remoct.conf`

> ⚠ **`remoct.conf` protects its sensitive fields at rest.** The Last.fm secret, session
> key, and pending auth token, the ListenBrainz token, and the Podcast Index secret are
> encrypted with DPAPI on Windows (bound to your user account) and scrambled with a
> machine-keyed XOR on Linux - obfuscation rather than encryption, and named that way
> deliberately, because it stops a casual read of the file and nothing stronger. The
> Last.fm API key and all usernames stay plaintext. The file is machine-local; keep it
> private and do not commit it - it is gitignored. RE-MOCT transmits no audio and stores
> no third-party data beyond what a request needs.

The file is written automatically on exit - you do not create it by hand. A trimmed
slice of a real one, with the playback, appearance, and rip keys people actually edit
(internal state like `last_dir` and `playlist_current` is left out here, and the
credential keys are covered above):

```ini
# RE-MOCT configuration - auto-generated
volume=0.25
crossfade=2
repeat_mode=1
shuffle=0
awesome_mode=1
awesome_theme=0
eq_enabled=0
nerd_icons=1
follow_playing=1
rip_formats=flac,mp3
flac_level=5
mp3=V2
opus_bitrate=128000
aac_vbr=1
aac_vbr_level=4
bookmark=D:\Music
fav=c:\users\david\Music\re-moct\Charli xcx - BRAT (2024)\11 - Apple.opus
```

The values that are not obvious: `volume` is a fraction from 0 to 1, not a percentage
or a step count. `repeat_mode` is `0` off, `1` one track, `2` all. `crossfade` is in
seconds and `0` turns it off. `awesome_theme` is an index into the palette list that
`F7`/`F8` cycles. `rip_formats` is a comma-separated list, and each format's quality
lives in its own key (`flac_level`, `mp3`, `opus_bitrate`, and so on), with the `rec_`
prefixed twins holding the same settings for stream recording.

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
