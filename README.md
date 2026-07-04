# RE-MOCT - Music On Console Terminal

**RE-MOCT** is a terminal music player, CD ripper, and internet-radio client for
**Windows and Linux**, written in C++20 on ncurses, miniaudio, TagLib, libFLAC, LAME,
libebur128, and FDK-AAC.

It is a homage to [MOC](http://moc.daper.net/) (Music On Console) with a twist. In
**Classic mode** it stays faithful and minimal. Hit **Ctrl+T** for **Awesome mode** and
it becomes *RE-MOCT* - the remix: a comet progress bar, a sub-cell block visualizer, and
breathing animations. The mode toggle is the whole point.

```
┌─ RE-MOCT – Music On Console Terminal [REPEAT ALL] [SHUFFLE] [CD] [Tragic Kingdom (1995)] v1.0.0 ─┐
>> [Drives]
┌─────────────────────────────────┐  ┌─ Playlist [14]  46m 8s ──────────────────────────────────────────┐
│ [Recent]                        │  │ > No Doubt – Spiderwebs                                      4:28 │
│ [FAVs]                          │  │   No Doubt – Excuse Me Mr.                                   3:05 │
│ [Bookmarks]                     │  │   No Doubt – Just a Girl                                     3:29 │
│ C:\                             │  │   No Doubt – Happy Now?                                      3:43 │
│ F:\  ◄─ CD drive                │  │   No Doubt – Different People                                4:35 │
└─────────────────────────────────┘  └──────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────┐
│ ════════════════════════════════════░░░░░░░░░░░░░░░░  2:14 / 4:28   │
└──────────────────────────────────────────────────────────────────────┘
 Ripping track 5/14  [47%]  8.3x  C2  →  AR: [A v2 OK conf=84]
 SPC:pause  n/p:next/prev  [/]:seek  -/+:vol  Tab:focus  Enter:play  ?:help  ^Q:quit
```

## Screenshots

See [`docs/screenshots/`](docs/screenshots/) - the 256-color visualizer (Awesome mode)
and a playlist/rip view. The public feature guide is [`docs/index.html`](docs/index.html).

## Features

**Local playback**
- MP3, FLAC, OGG, WAV, AAC/HE-AAC, and `.m4b` audiobooks (chapter navigation)
- Gapless playback, configurable crossfade, varispeed
- Repeat (track/all), shuffle, seek, volume, 10-band equalizer
- ReplayGain tag support; per-track play counts
- LRC lyrics, tag editor (via TagLib), queue, bookmarks, favorites (star key), goto bar
- Split-pane UI: directory browser + playlist/views; full-text playlist search

**CD playback & ripping** (Ctrl+Y)
- Red Book CD playback + MusicBrainz disc lookup (Ctrl+R)
- Three rip modes:
  - **[A] AccurateRip** - network CRC verify against accuraterip.com + drive-offset correction
  - **[C] CUETools Database** - offset-immune whole-disc CRC32 (cue.tools/db)
  - **[Y] Local** - best-effort offline rip
- Every mode: FLAC + MP3 output, embedded cover art (Cover Art Archive), EBU R128
  ReplayGain tags, C2 error-pointer detection, dual-pass on mismatch, per-rip logs

**Internet radio & streaming**
- RadioBrowser (radio-browser.info) station search (Ctrl+U to add by URL)
- ICY/SHOUTcast streaming with live StreamTitle metadata
- iHeartRadio via HLS, with now-playing reconciliation and a digital (web-player) path

**Scrobbling & presence**
- Last.fm (Ctrl+G) and ListenBrainz (Ctrl+B) scrobbling + now-playing
- Discord Rich Presence with album art

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
plugin built beside it in `build/bin/plugins/`.

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

## Configuration

Config file:
- **Windows:** `%APPDATA%\re-moct\remoct.conf`
- **Linux:** `$XDG_CONFIG_HOME/re-moct/remoct.conf` (falls back to `~/.config/re-moct/`)

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
