# Changelog

All notable changes to RE-MOCT are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.0] - UNRELEASED

Feature release: Opus and WavPack playback.

### Added

- **Opus playback** (`.opus`): full decode/seek via libopus + libopusfile.
  Opus files were already browsable but silently failed to play; they now
  play, seek, show correct duration, and read tags. Opus ReplayGain
  (`R128_TRACK_GAIN`, including the -23 to -18 LUFS reference rebase) is
  applied correctly - previously a tagged Opus file would have played muted
  with ReplayGain enabled.
- **WavPack playback** (`.wv`): full decode/seek via libwavpack - 16/24/32-bit
  integer and float files, at any sample rate, with a **WV** file-type column
  label. Hybrid (lossy) `.wv` files play at their encoded quality; `.wvc`
  correction sidecars are not read (planned).
- **BPM detection** now works for `.opus` and `.wv` files too.
- **Choose rip output formats**: the rip dialog now has a format list -
  toggle FLAC and MP3 with the number keys (1, 2) before picking a rip mode.
  The header shows a live "Out:" summary, lossless formats carry a "*"
  master marker, and deselecting everything disables the rip keys until at
  least one format is checked. The selection lasts for the session; set the
  startup default with the new "rip_formats" config key.
- **WavPack rip output**: the fifth and final rip format - lossless
  compressed audio with a "*" master marker (toggle with 5, config token
  "wavpack", new "wavpack_mode" key: fast/normal/high/very_high, default
  normal). Fully tagged in APEv2 including cover art and standard
  ReplayGain. With this the rip dialog reaches its full form: five formats,
  any combination, all written from a single verified disc read.
- **Opus rip output**: the rip dialog gains a fourth format - Opus at a
  configurable VBR bitrate (toggle with 4, config token "opus", new
  "opus_bitrate" key, default 128 kbps). Fully tagged including cover art,
  with ReplayGain written in Opus's native R128 dialect - the exact inverse
  of the player's R128 read, so ripped Opus and FLAC land at the same
  playback loudness.
- **WAV rip output**: the rip dialog gains a third format - untagged
  bit-exact 16-bit PCM (toggle with 3, config token "wav"). WAV carries no
  tags, cover art, or ReplayGain by format; the dialog marks the row
  "(untagged)". Lossless, so it carries the "*" master marker.
- **Rip quality is configurable**: new "flac_level" (FLAC compression 0-8,
  default 5) and "mp3" (LAME VBR quality V0-V9, default V0) config keys.
  Defaults match the previous fixed settings exactly, and the dialog shows
  the active values next to each format.
- **Ogg Vorbis playback** (`.ogg`): full decode/seek via libvorbis +
  libvorbisfile. Like Opus, `.ogg` files were already browsable but silently
  failed to play; they now play, seek, show correct duration, and read tags
  (including standard ReplayGain). BPM detection works for `.ogg` too.

### Changed

- **Opus decode is now native float** (matching the new Vorbis backend): the
  decoder hands float samples straight through instead of quantizing to
  16-bit first. ReplayGain behavior is unchanged.
- New library dependencies: libopus, libopusfile, libwavpack, libvorbis (all
  BSD-3-Clause; see THIRD-PARTY-NOTICES.md).

## [1.2.0] - 2026-07-10

Feature release: the MOC-parity milestone - list page navigation, a playlist
cursor-position readout, an optional file-type column, and a rewritten
spectrum visualizer. With this release RE-MOCT matches and goes beyond MOC's
feature set.

### Added

- **List page navigation** (**PgUp** / **PgDn** / **Home** / **End**): move by
  a page or jump to the top/bottom of the browser and playlist. Cursor-only -
  nothing plays or reorders.
- **Cursor position in the playlist header**: **[3/12]** shows which row of
  how many the cursor is on.
- **File-type column** (**Shift+F**): toggles a per-row FLAC/MP3/etc. column
  in the playlist, MOC-style. Off by default, remembered across runs.
  (Shift+F rather than a bare F-key: Linux terminals grab F11 for fullscreen.)

### Changed

- **Rewritten spectrum visualizer**: a real FFT replaces the old approximate
  transform, which aliased everything above ~5.5 kHz - the top bars were
  noise, not signal. The spectrum is now accurate across the full range and
  far cheaper to compute, with per-band normalization and a perceptual tilt
  so quiet ranges stay readable and each track's real mastering character
  shows.

[1.2.0]: https://github.com/RadMageIRL/re-moct/releases/tag/1.2.0

## [1.1.3] - 2026-07-09

Feature release: playlist search, CD eject from the TUI, drive-list refresh,
and a fix for CD drives that held the tray locked after playback.

### Added

- **Playlist search** (**\\**): type a query and jump to a matching track.
  Matches artist + title as shown, case-insensitive. A single match jumps
  directly; multiple matches open a pick list (**Enter** to jump, **Esc** to
  close). Cursor-jump only - the playlist is never filtered or reordered.
- **CD eject from the TUI** (**Shift+E**): in **[Drives]**, a highlighted CD
  drive with a disc shows a ⏏ hint; **Shift+E** ejects it - stopping playback
  and cleaning up its playlist rows first if that disc is loaded. Ejecting
  during an active rip is refused. Drives whose firmware refuses software
  eject report so honestly (stop playback and use the drive button).
- **F12 refreshes the drive list**: hot-plugged USB/optical drives appear
  without a restart; the cursor stays on the entry it was on.

### Fixed

- CD drives that hold a soft media-removal lock after reads (the
  HL-DT-ST/LG/HLDS family) now release the tray for physical-button eject
  after stop, instead of needing repeated presses - the lock is explicitly
  cleared before every drive-handle close.

[1.1.3]: https://github.com/RadMageIRL/re-moct/releases/tag/1.1.3

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
