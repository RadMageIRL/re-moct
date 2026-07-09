# Changelog

All notable changes to RE-MOCT are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.2] - 2026-07-09

Fix release: two follow-playing / playlist interaction bugs found in real use.

### Fixed

- Moving a playlist track (**K** / **J**) across the playing track's position no
  longer steals the cursor onto the playing track mid-move; deleting a row above
  the playing track no longer snaps the cursor either. Moving the playing track
  itself still carries the cursor along.
- The chapter list (**;**) now opens for a highlighted book in the playlist, not
  only the currently-playing file - browse a book's chapters before playing it.
  **Enter** on a chapter of a browsed book starts playing that book at that
  chapter; the title bar and **,** / **.** chapter jumps stay keyed to the
  playing track's own chapters.

[1.1.2]: https://github.com/RadMageIRL/re-moct/releases/tag/1.1.2

## [1.1.1] - 2026-07-08

Fix release: the playlist keymap change (move-track is now **K** / **J**), a
follow-the-playing-track toggle (**F3**), and a batch of fixes - a stopped CD no
longer unloads, the cursor follows the playing track across every source, tag
saves are honest about failure, and cover art keeps its colours across theme
changes.

### Changed

- **Move-track keys are now K / J** (was u / D); **u**, **U**, and **D** are no
  longer bound. **d** still deletes. Frees the lowercase keys and matches the
  j/k movement family.
- The title-bar **[CD]** tag and album name now persist while a disc is loaded
  but stopped - a stopped disc is still a loaded disc.

### Added

- **F3 - follow the playing track** (default on): the playlist cursor tracks the
  now-playing row on every track change - manual, auto-advance, file, CD, or
  radio. Turn it off to browse freely while music plays; announcements and
  scrobbling are unaffected by the toggle.

### Fixed

- **Stopping a CD no longer unloads it.** The track list and MusicBrainz titles
  survive a stop (they used to vanish ~6 seconds later); play, **Ctrl+R** lookup,
  and **Ctrl+Y** rip all work on a stopped disc by reopening the drive on demand.
  The drive is left untouched while stopped, so it never spins up idle; a
  transient drive hiccup on reopen retries instead of unloading the disc, and a
  disc ejected while stopped is detected (and the list cleared) on the next
  action.
- **The playlist cursor and lit-row indicator now follow the playing track across
  every source transition** - song->radio, CD->radio, file<->CD, and both
  auto-advance paths. Previously the indicator could stick to (or the cursor
  snap back to) a stale row after switching source; three related bugs, one
  root cause, all fixed.
- **Cover art keeps its colours.** Art no longer discolours after a theme change
  or reload (Ctrl+T / ~ / F7 / F8), and on covers too busy for the colour-pair
  budget the overflow now picks the nearest existing colour instead of a wrong
  one.
- **Tag saves are honest.** A failed save (e.g. the file is locked by playback)
  now warns and leaves the UI unchanged instead of showing success while the
  disk kept the old tags.
- The **Ctrl+Y** rip and **Ctrl+F** search overlays are themed in Awesome mode
  (they rendered unthemed/plain before).
- The playlist cursor can no longer end up highlighted but scrolled offscreen
  (radio-add, CD-eject, and similar paths); **d**-delete on an unrelated row no
  longer stops a playing stream.

[1.1.1]: https://github.com/RadMageIRL/re-moct/releases/tag/1.1.1

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
