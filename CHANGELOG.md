# Changelog

All notable changes to RE-MOCT are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-07-05

Feature release: cover art in the Info pane, a segmented "80s LED" spectrum mode,
an optional PDCursesMod wingui/GDI render backend for Windows (truecolor,
borderless fullscreen), more Awesome themes, and a batch of input-latency and
navigation fixes.

### Added

**Interface**
- Cover art in the Track Info (i) pane, rendered as half-block cells: embedded art
  for local files, and for radio the station-supplied cover (iHeart) or an
  iTunes/Deezer song lookup, falling back to the RE-MOCT logo. Decoded off the UI
  thread and cached, so opening the pane and toggling tag-edit never stall.
- KITT scanner: a Knight-Rider sweep across the idle gap in the radio status bar,
  theme-coloured via the visualizer roles.
- Spectrum styles (**F2**): the classic solid bars, or an 80s graphic-EQ "LED" look
  (stacked half-block segments coloured by height). The spectrum now fills the full
  width with uniform single-column bars at any terminal size.
- Awesome themes: 18 named truecolor palettes (adds **Matrix**, digital-rain green);
  **F7** / **F8** cycle to the previous / next palette.

**Windows (PDCursesMod wingui build)**
- Optional GDI render backend (`-DREMOCT_PDCURSES=ON`) that draws the TUI in its own
  window with guaranteed truecolor for Awesome mode, an OS-matched dark title bar,
  and process-private bundled-font loading. See [BUILD.md](BUILD.md).
- **Alt+Enter** toggles borderless fullscreen (covers the taskbar); the window
  remembers its size across launches.

### Changed

- The radio station label now shows in the top bar on Linux too (was Windows-only).

### Fixed

- Input latency: a keypress now takes effect in the same frame instead of waiting up
  to ~80ms for the next input poll, so pane switches and navigation feel immediate.
- Esc no longer stalls ~1s before closing a pane on Linux (ncurses `ESCDELAY`).
- The Info pane no longer blocks the UI thread while decoding cover art (async + cached).

[1.1.0]: https://github.com/RadMageIRL/re-moct/releases/tag/1.1.0

## [1.0.0] - 2026-07-04

Initial public release. RE-MOCT is a terminal music player, CD ripper, and
internet-radio client for **Windows and Linux**, built on a clean core/platform
boundary with a **loadable plugin architecture**.

### Added

**Local playback**
- MP3, FLAC, OGG, WAV, AAC/HE-AAC, and `.m4b` audiobooks (chapters + `[Books]` nav)
- Gapless playback, configurable crossfade, and varispeed
- Repeat (track/all), shuffle, seek, volume, 10-band equalizer
- ReplayGain tag support; per-track play-count and last-played tracking
- LRC lyrics, tag editor (read/write via TagLib), queue, bookmarks, favorites, goto bar

**CD playback & ripping**
- Red Book CD playback with MusicBrainz disc lookup
- Ripping in three modes - AccurateRip (network CRC verify + drive-offset), CUETools
  Database (offset-immune CRC32), and Local - producing FLAC + MP3 with embedded cover
  art, EBU R128 ReplayGain tags, C2 error-pointer detection, and per-rip logs
- AccurateRip CRCv1/v2 computed per the whipper/CUETools reference; the 150-sector
  disc preamble is honored as designed

**Internet radio & streaming**
- RadioBrowser (radio-browser.info) station search and playback
- ICY/SHOUTcast streaming with live StreamTitle metadata
- iHeartRadio via HLS, with now-playing reconciliation and a digital (web-player)
  rendition path

**Scrobbling & presence**
- Last.fm and ListenBrainz scrobbling + now-playing
- Discord Rich Presence (album art via iTunes) over local IPC

**Interface**
- Two modes: **Classic** (a faithful MOC homage) and **Awesome** (Ctrl+T - comet
  progress bar, sub-cell visualizer, breathing animations)
- Column-aware UTF-8 rendering via the ncurses wide API; theming (`theme.conf`)

**Cross-platform & architecture**
- Runs on Windows (MSYS2 UCRT64) and Linux (Debian Trixie); every platform call is
  behind a seam (HTTP, IPC, notifications, CD access, plugin loading) with a
  Windows + Linux implementation
- The streaming source is a real loadable plugin (`remoct_stream.{so,dll}`) driven
  through a frozen C ABI - *"fix iHeart and ship without rebuilding the host"* - with
  a deterministic byte-identity test proving the loaded plugin matches compiled-in

### Diagnostics

- Opt-in NDJSON deep-analysis log for iHeart streaming (**Ctrl+A**)
- Experimental iHeart minted-profileId A/B probe (**Ctrl+P**), off by default; when
  armed it evaluates an anonymous digital-handshake identity in the deep log. The
  shipped anonymous path is unchanged when the probe is not armed.

[1.0.0]: https://github.com/RadMageIRL/re-moct/releases/tag/1.0.0
