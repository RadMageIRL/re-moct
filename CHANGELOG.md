# Changelog

All notable changes to RE-MOCT are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.0] - Unreleased

### Added (podcasts)

- A new `[Podcasts]` section in the browser sidebar, alongside `[Radio]` and
  `[Books]`. Press `/` inside it to paste a podcast feed URL; RE-MOCT fetches and
  parses the feed and, if it is a valid podcast, subscribes to it and shows the
  show by title. Subscriptions are saved and persist across restarts.
- Open a subscribed feed with Enter to see its episodes - title, date, and length
  per row - newest first, with the usual PgUp/PgDn/Home/End paging. `[Back]`
  returns to the feed list. Remove a feed with `d` or `Del`, like a radio station.
- Feeds are fetched in the background, so subscribing to or opening even a very
  large show never freezes the interface. A feed that cannot be fetched or is not
  a real podcast reports the problem and changes nothing. (Playing an episode
  arrives in a later update.)

### Security

- The sensitive Last.fm and ListenBrainz credentials in `remoct.conf` (the shared
  secret, the session key, the in-flight auth token, and the ListenBrainz token)
  are no longer stored as plaintext. On Windows they are encrypted with DPAPI, tied
  to your Windows user account; on Linux they are obfuscated with a machine-derived
  key. The API key and usernames stay plaintext, so the file is still easy to read
  and diff. This protects against casual disclosure - synced folders, backups,
  pasted logs, screen shares - not against software running as your own user.
- Because the protection is bound to the machine and user, the four protected
  fields no longer carry over if you copy `remoct.conf` to another machine or user
  account; you re-authenticate there. An existing plaintext config keeps working
  and upgrades to the protected form on the next save.

## [1.3.1] - 2026-07-20

### Changed (iHeart re-pin - F6)

- The F6 re-pin modes are renamed by what makes them fire, and the re-pin now
  requires ad evidence instead of duration alone: `off` never re-pins, `ad-escape`
  fires only on hard ad evidence (a paid spot id or spot churn), `hybrid` (the new
  default) fires on that evidence or when the stalled window actually contains ad
  segments, and `timed` keeps the old duration-only behaviour. The old duration-only
  escape treated a long talk show like a stuck ad pod and silently re-joined the
  stream every ~3 minutes through entire shows - hybrid rides talk out and still
  escapes real ad pods.
- The immediate ad-onset re-pin is gated the same way, so a mid-show discontinuity
  marker no longer triggers a pointless re-join.
- Saved re-pin settings migrate automatically: the old `on` and `smart` modes both
  become `hybrid`. The old 35-second `on` floor is retired; all active modes share
  the ~2.5-minute floor.
- F6 now confirms the new mode in the status line (matching F2's style, drawn
  in yellow) and is listed in the `?` help pane. Ctrl+K confirms its feed
  switch the same way. The transient lower-left mode tag on the now-playing
  row is gone - it repeated what the status line already says.

### Added (iHeart re-pin - F6)

- New F6 mode `live-edge`: follow the live edge the way the web player does.
  Drift itself is the trigger - when playback falls behind the edge during ad-free
  programming (a countdown or long talk block), it re-anchors automatically instead
  of sitting on stale audio until a manual re-pin. No ad logic, always current
  (including ads at the edge); the tradeoff is a smaller effective buffer, so it is
  twitchier on a laggy connection than the escape modes. A healthy stream never
  triggers it.

### Added (convert)

- The convert pop-up (`x`) now transcodes whole playlists, not just single
  files and folders: [4] converts every file in the current playlist pane and
  [5] converts every file a focused playlist file references, both through the
  same audio encoder and format picker as [1]-[3]. Output lands next to each
  source file. Stream and CD entries are skipped (the row shows how many).
- Save the current playlist pane to a container with `Shift+S`: type a `.m3u`,
  `.m3u8`, `.pls`, or `.xspf` name and the format is chosen from the extension.
  When the browser cursor is on a playlist file, `Shift+S` opens a small pop-up
  to reformat that file into M3U8, PLS, or XSPF (written next to the source,
  auto-suffixed so it never overwrites); the plain pane save is still one press
  away with `[S]` inside the pop-up. Stream and CD entries are skipped - they do
  not belong in a portable playlist file.

### Fixed

- On Windows, dragging the window by its title bar no longer freezes the display
  until you let go - the spectrum and marquee keep animating during the move,
  matching how a resize drag already behaved.
- The now-playing marquee and the spectrum keep animating while a pop-up is open
  (convert, playlist save, rip confirm, rec panel, MusicBrainz search) instead of
  freezing until it is dismissed; the pop-up stays crisp on top.
- In the Awesome theme, the spectrum strip lines up with the panes above it - its
  left and right edges now match the browser and playlist borders instead of
  overhanging, and the stray empty bar slot at the right edge is gone (it stays
  gone across terminal resizes).
- The text-entry cursor stays in the input field (save, goto, load, radio search,
  and the rest) instead of blinking in the panes when opened on an idle screen.
- On Linux, the save prompt and the goto bar's tab-completion no longer insert a
  backslash as a path separator (a backslash is a valid filename character there);
  both now use the platform separator, so saving a playlist and drilling into
  directories produce valid paths. Windows is unaffected (still backslashes).
- Closing a pop-up or menu no longer leaves stray cells in the inter-pane gutter;
  the dismissed overlay's footprint is repainted without the full-screen flash the
  Ctrl+L workaround caused.

## [1.3.0] - 2026-07-18

Feature release: Opus and WavPack playback.

### Added (iHeart re-pin control)

- **F6 cycles the iHeart re-pin mode: off, on, smart.** off plays through ads
  continuously like the web player; on re-pins out of every long break (the
  previous always-on behaviour); smart rides out short breaks and re-pins only
  long ad pods. The mode persists across restarts and is independent of the
  Ctrl+K feed toggle (raw vs web-player), so you can pair, e.g., web-player feed
  with smart re-pin.
- **Persistent feed and re-pin indicator.** The lower-left of the now-playing row
  shows the current iHeart modes in yellow, e.g. "digital - smart" or "raw - off",
  replacing the transient Ctrl+K toast. iHeart streams only.
- **Re-pin lands closer to the song start.** On a re-pin out of an ad, if the
  fresh stream shows a clean ad-to-music boundary, playback now primes from the
  song's first segment instead of a couple of segments behind the live edge, so
  less of the incoming song's opening is lost. Falls back to the previous
  live-edge behaviour when no clean boundary is visible.

### Changed (iHeart re-pin default)

- **The default re-pin mode is now smart** (previously the re-pin always fired on
  every long break). On upgrade, iHeart playback rides out short ad breaks and
  re-pins only long pods. Press F6 to choose off or on.

### Documentation

- **Clarified font selection per platform, and made `wingui_font` discoverable.** On
  Windows RE-MOCT sets its own font via the `wingui_font` config key (default a bundled
  JetBrains Mono Nerd Font); on Linux it uses the terminal emulator's font. README and
  BUILD.md now document both mechanisms and the Ctrl+N icon toggle. `wingui_font` is now
  written to the config (empty by default, with the same bundled-default behaviour) so
  it can be found and set. No behaviour change.

### Added (AAC/M4A output)

- **Rip, convert, and record to AAC in an MP4 (.m4a) container.** M4A joins the
  output formats as row 6 (digit key 6) in the rip and convert pickers and as a
  Format choice in the recording panel. AAC-LC only. It uses the FDK-AAC encoder
  already bundled with RE-MOCT, so no new library or DLL ships. Output is a
  standard, seekable, tagged .m4a that plays in RE-MOCT and other players.
- **Per-format AAC quality, VBR or CBR.** The rip and convert format editor and
  the recording panel expose an AAC quality axis: VBR on a 1-5 ladder (5 best,
  default 4), or CBR at 96/128/256/320 kbps via the [M] mode toggle - the same
  editor MP3 and Opus already use. Rip and recording keep independent AAC
  settings, saved to the config.
- **Tags, cover art, and ReplayGain on .m4a.** Title, artist, album, track,
  AccurateRip, and ReplayGain are written as MP4 atoms; cover art is written as a
  covr atom (rip, convert carryover, and recording).

### Changed

- **The recording panel's Copy option moved from key 3 to key 4.** The Format
  rows are now Opus (1), MP3 (2), M4A (3), and Copy (4); Copy stays the last,
  as-broadcast option. For an AAC broadcast, Copy remains the better choice than
  recording to M4A - it captures the original frames with no re-encode.

### Added (rip log)

- **The rip log now states which outputs are the verifiable master.** Each
  selected format is listed as "lossless - verifiable master" or "lossy -
  derived copy", with a Master line in the header and summary. Precisely
  worded: AccurateRip/CTDB verify the disc read; lossless outputs retain
  the verified audio bit-for-bit, lossy outputs are transcoded copies of
  the same verified read. A lossy-only rip says plainly that it has no
  verifiable master.

### Fixed

- **Long station names no longer clip the recording panel.** A station name
  wider than the recording panel used to overrun the modal's right border into
  the pane behind it. The Station and output-directory fields now clamp to the
  panel width, and a long station name scrolls within its field so the whole
  name is readable.
- **Recorded cuts keep their cover art when split-hold is on.** A recorded
  song could ship without its cover even though the art showed correctly in
  the now-playing pane. With the split hold active (the default), a cut stays
  open briefly past the next song's start; the next song's art arriving in
  that window used to overwrite the pending image before the held cut was
  written, so the finished file went out coverless. Each song's art is now
  held separately by title, so a cut always embeds its own cover.
- **Radio cover art now refreshes reliably.** Two staleness sources fixed:
  a track's art could go permanently missing when its lookup finished during
  a metadata dip (an ad break or LIVE stretch) while a recording was active -
  switching stations and back was the only recovery; and a single transient
  network failure on a song's first art lookup used to blank that song for
  the whole session. Art now recovers on the song's return, and failed
  lookups retry after a cooldown instead of never - while genuinely
  art-less songs are still looked up at most once per cooldown window.

- **MP3 ReplayGain and AccurateRip tags are now written in the standard
  form other players read.** Previously the whole "KEY=value" text landed
  in the tag frame's description with an empty value - RE-MOCT could read
  its own files (see the ReplayGain fix below) but foobar2000-class players
  and CUETools saw nothing. New rips and recordings write a proper
  description/value split; the tag names and values are unchanged. Existing
  files are not modified and still read fine in RE-MOCT.
- **MP3 files now honor their ReplayGain tags.** MP3s ripped by RE-MOCT
  carried a correct gain value that the player never applied - MP3 tracks
  played at full level while their FLAC and Opus siblings played
  gain-adjusted. The stored gain is now read and applied on playback.
  Heads up - this is audible: existing ripped MP3s will now play at their
  corrected (usually lower) volume, matching their FLAC siblings.

### Added

- **Convert audio files to another format.** Press x in the file browser to
  convert a single file, every audio file in the current folder (one level, not
  subfolders), or a set you have marked. Press u to mark or unmark the
  highlighted file and U to clear all marks; marked files show a marker and a
  count in the browser, and marks stay put as you move between folders so you can
  gather a set and convert it in one go. The convert picker reuses the same
  format and quality controls as ripping (FLAC, MP3, WAV, Opus, WavPack, with the
  per-format bitrate and CBR/VBR choices). Existing tags and the embedded cover
  art carry over to the new file, so a converted track keeps its artwork even
  when moved away from a folder image. Output is 44.1 kHz; a higher-rate source
  is resampled, and the picker
  warns before converting one. Converting never overwrites the source or an
  existing output (those are skipped), and conversion runs in the background so
  playback continues.
- **Per-format bitrate and a CBR/VBR toggle for MP3 and Opus.** The rip
  confirmation modal and the recording panel now let you tune each lossy
  format's quality on its own row. Move the row cursor with Up/Down, change the
  value with Left/Right, and press M to switch a row between constant and
  variable bitrate. MP3 offers the V0-V9 quality scale in VBR mode and a
  96/128/256/320 kbps choice in CBR mode; Opus offers the same bitrate choice in
  either mode. FLAC, WAV, and WavPack rows have no bitrate and are unaffected.
- **Recording keeps its own quality settings, separate from ripping.** Recorded
  radio can be right-sized (for example Opus 96 kbps) without changing the
  high-quality settings a CD rip uses, and the reverse. The recording panel
  defaults to Opus 96 kbps and MP3 V5.

- **OS media controls.** RE-MOCT now appears in the operating system's own
  media surface and responds to its transport keys. On Windows the now-playing
  title, artist, and cover show in the volume/media overlay and on the lock
  screen, and the keyboard media keys (play/pause, next, previous, stop) and
  the on-screen scrubber drive playback. On Linux it exports the standard MPRIS
  interface, so playerctl and desktop media widgets read the now-playing and
  drive playback the same way. Position and duration are reported so the
  scrubber tracks and can seek. Seeks route through the same smoothing the
  in-app seek keys use. On by default; turn it off with "os_media_control=0" in
  the config. Cover art shows on the card: a local file's embedded picture, or a
  radio track's cover (the station's own art, or the same iTunes/Deezer lookup
  the Info pane uses), falling back to the RE-MOCT logo when a track has none.

- **The help pane scrolls with Home, End, PgUp, and PgDn** (in addition to
  j/k), so the full keybinding list is reachable on a short terminal.
- **Batch ReplayGain scan** (Ctrl+O in the file browser): point it at a
  folder and it computes and writes track gain for every supported audio
  file - FLAC, MP3, Opus, WavPack, and M4A/M4B - using the same loudness
  math as the CD ripper, so batch-tagged and ripped files agree exactly.
  Runs in the background with progress and cancel; already-tagged files
  are skipped (re-runs only touch new files) unless you choose force
  re-tag at the prompt. WAV files are noted and skipped (the format goes
  untagged, matching rip behavior). One-time heads up: MP3s tagged by
  RE-MOCT before the tag-format fix carry their gain in a form other
  players cannot read - the scan detects those as untagged and rewrites
  them in the standard form, so a first run may touch MP3s you thought
  were already tagged. That is the fix reaching your existing library.
  Track gain only; album gain is a possible follow-up. This is a
  decode-bound scan - expect a few seconds per track on a first full run.
- **Recording continues through a playback pause.** Pausing while recording
  now mutes only what you hear - the broadcast keeps being captured, with no
  silence gap and nothing lost; on resume you rejoin the live broadcast.
  (Previously the paused-over airtime was silence in the file and gone for
  good - the recording panel said so.) Requires the updated streaming
  plugin; with an older plugin the old behavior and the old honest note
  remain. The plugin interface grew compatibly - existing plugins keep
  working unchanged.
- **Split hold for recordings** - radio metadata tends to fire a little
  early, guillotining the previous song's outro. The recorder now holds the
  cut boundary by a configurable offset (default 1200 ms, "Split hold" in
  the recording panel, "split_offset_ms" config key) so the closing cut
  keeps its tail. Honest limit: cuts approximate the broadcast - the hold
  trims typical metadata earliness but cannot create clean seams the
  station's own segues and crossfades never had.
- **Ad-aware recording** - the recording panel gains an "Ad segments"
  choice. Save (default) routes segments the station marks or titles as
  non-song (ad breaks, station IDs, live/talk stretches) into an ads/
  subfolder with timestamped names, keeping your song folder clean without
  deleting anything. Discard skips writing them entirely - and shows a
  running "ads skipped" count while recording plus a summary at stop, so
  you can tell it is working (and tell if it is over-firing). Heads up:
  Discard trusts the station's metadata - on a station that mislabels
  songs, a real song can be lost; that is the trade you opt into, and the
  panel says so at the toggle. Titles that do not parse at all are always
  kept in the main folder in both modes.
- **Recorded cuts now embed cover art.** Stream recordings reuse the same
  cover lookup the radio Info pane already does (station art, then the
  iTunes/Deezer song search) and embed the image in each cut's tags - MP3
  and Opus both, and it works whether or not the art pane is open. Strictly
  best-effort: a slow or failed lookup never delays or drops a capture, and
  under the same 1-2 second metadata slop an edge cut may miss its cover -
  it will never carry the wrong one.
- **Stream recording** (Ctrl+E while a radio stream is playing): capture the
  station you are listening to straight to disk. The panel picks the output
  format (Opus at the configured bitrate, default, or MP3 - the broadcast is
  already lossy, so lossless output is deliberately not offered), toggles
  split-on-track-change, and sets the output folder (default:
  Music/re-moct/recordings/, beside your rips - override with the new
  "rec_dir" key; "rec_format" and "rec_split" set the startup defaults).
  Each song is cut and tagged from the station's own now-playing metadata
  (title, artist, per-track ReplayGain, a station credit) and named
  Artist - Title; cuts with missing or unreadable metadata fall back to a
  station-plus-timestamp name. Honest limits, stated up front: song
  boundaries come from the broadcaster's metadata and typically land within
  1-2 seconds of the real change, so edges can carry a moment of the
  neighboring track; the first and last cuts of a session are partial
  (marked in the filename); and pausing playback while recording leaves a
  silence gap - the paused-over airtime is not captured. A [REC] indicator
  shows in the title bar whenever a recording is running, and recording
  stops cleanly (finishing its current file) when you stop the stream,
  switch stations, or quit.
- **Record as broadcast (Copy mode).** A third recording format alongside
  Opus and MP3: capture the station's own encoded audio to disk with no
  re-encoding at all. The cut carries the exact broadcast bytes - MP3
  stations save to `.mp3`, AAC stations to a tagged `.m4a` - so there is no
  second-generation quality loss and the recording plays in RE-MOCT and any
  standard player. Each cut is tagged from the now-playing metadata (title,
  artist, station credit, cover art) just like the re-encode modes, and
  split-on-track-change, the split hold, and ad handling all work the same;
  boundaries snap to whole compressed frames (well under the metadata slop).
  The panel shows the live codec next to the Copy row and greys it out on the
  rare older streaming plugin that cannot supply the encoded feed. Two
  deliberate notes: copy cuts carry no ReplayGain tags (re-encode mode
  remains the one that computes and writes per-track gain), and they keep the
  broadcast's own sample rate rather than resampling - that fidelity is the
  point. Set it as the startup default with `rec_format=copy`.
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
- New library dependencies: libopus, libopusfile, libopusenc, libwavpack,
  libvorbis (all BSD-3-Clause; see THIRD-PARTY-NOTICES.md) - libopusenc backs
  Opus rip and convert output. On Linux, MPRIS media control uses sd-bus from
  libsystemd (libsystemd-dev); Windows media control (SMTC) needs no extra
  package.

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

[1.3.0]: https://github.com/RadMageIRL/re-moct/releases/tag/1.3.0
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
