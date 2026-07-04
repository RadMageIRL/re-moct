# Changelog

All notable changes to RE-MOCT are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
