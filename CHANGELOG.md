# Changelog

All notable changes to RE-MOCT are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.5.0] - Unreleased

### Added

- A `[Library]` section in the browser, listing every artist in your music
  folder regardless of how the folders are arranged. RE-MOCT stays a folder
  player - the directory browser is unchanged and still where everything
  starts - and this sits beside it for the times you know the artist and not
  the folder. Opening it the first time reads the tags of everything under your
  music folder, which takes a little while and shows its progress as it goes;
  after that it opens instantly, and it only re-reads files that have actually
  changed. Choosing an artist opens their albums, and choosing an album opens
  its tracks, numbered and with their lengths. The left arrow and the `[Back]`
  row both step back up one level, and the header always says which artist and
  album you are inside. Tracks are ordered by disc and track number, falling
  back to the filename for rips that were never tagged, and albums that share a
  name between two artists stay separate. Coming back up a level puts the cursor
  back on the row you came through rather than at the top of the list.
  A library track behaves exactly like the same file in the folder browser, so
  everything you already know works on it: Enter adds it to the playlist and plays
  it, `a` adds it without playing so you can walk an album picking tracks, `q`
  queues it, `*` favourites it, `u` marks it and `x` converts it, and `;` opens its
  chapters. `a` on an album row takes the whole album in one press, in the order
  the album is listed - disc then track number, falling back to the filename for
  discs that were never tagged - and says how many tracks it added. Tracks you
  already have in the playlist are not added twice, so the count is what actually
  went in rather than the length of the album, and any track that has been deleted
  since the last scan is reported as missing rather than passed over in silence. The `\` search works at all three levels, artists, albums and tracks.
  If a track has been deleted or moved since the last scan, RE-MOCT now says which
  file is missing instead of doing nothing, because the answer is to rescan and
  there was no way to tell that from silence.
- **Search your whole collection with `|`.** Type, and the list narrows as you type -
  across every track you have, not just the folder you happen to be looking at. It
  matches title, artist, album, album artist, genre and the filename, so a rip that
  was never tagged is still findable by its name, and a query with several words
  matches when every word is found somewhere. Press Enter on a result and it plays,
  exactly as it would from anywhere else in the library; `a`, `q`, `*`, `u`, `x` and
  `;` all work on a result too. Accents are handled the way you would want: typing
  `bjork` finds `BJÖRK`, `motley crue` finds `Mötley Crüe`, and `dvorak` finds
  `Dvořák`. `|` works from inside `[Library]` and from the ordinary folder browser,
  because finding a track you have lost should not require going somewhere first.
  `\` is unchanged and still searches the list in front of you - the two sit on the
  same key, shifted: `\` for what you are looking at, `|` for everything you have.
- `[Library]` can be turned off, and told where to look. Two new settings in
  `remoct.conf`: `library=0` removes the section entirely - not a row that refuses
  to open, actually absent, from the browser and from `[Drives]` alike - and
  `library_root=` points it at a folder other than your system music folder. It
  defaults to on, because until you open it the section costs nothing: no scan, no
  index file, no background work, just one row in the browser. Pointed at a folder
  that does not exist, or one it cannot read, it now says which folder and why
  instead of claiming the scan was cancelled. Change the folder between runs and it
  rescans once, says that is what it is doing, and then settles.
- **F12** rescans the library, the same way F12 already refreshes `[Drives]`. The
  library never rescans behind your back - that is deliberate, so opening the
  section is always instant - so this is how you tell it to look again after adding
  or retagging music. It works from any level, and the list you were looking at is
  still the list you are looking at afterwards.
- **Esc** cancels a library scan while it is running, and the section says how to do
  it while it scans. A cancelled scan changes nothing: whatever index you had before
  is left exactly as it was, down to the byte, because a half-finished walk cannot
  tell the difference between a file you deleted and a file it simply has not reached
  yet. The next time you open `[Library]` it says the scan was cancelled, and the
  time after that it tries again.
- Adding tracks from `[Library]` is now instant. It was re-reading the tags of every
  file as it added it, which it already had in the index from the last scan: adding a
  seventeen-track album took about a tenth of a second of pure disk reading, and now
  takes no measurable time at all. The rows that end up in your playlist are
  identical either way - the index and the playlist read tags exactly the same way -
  and if you have retagged a file since the last scan, the playlist shows what
  `[Library]` is showing you, which F12 is there to bring up to date.

- **Edit tags on any file you can see, not just ones in the playlist.** Put the cursor
  on a track anywhere in the browser - a library album, a collection search result, a
  folder, `[FAVs]`, `[Recent]` or `[Books]` - press `i` for track info and `e` to
  edit. Before, that only worked on playlist rows and everything else told you to add
  the track first. Editing a track in the library updates what the library shows
  straight away, including regrouping an album if you change who it is by, so you do
  not have to rescan to see your own change. Playing files still refuse until you
  stop, read-only files say so rather than failing quietly, and podcast episodes are
  left alone because a re-download would throw the tags away.
- **The library can now watch more than one folder.** Music kept on a second drive,
  or in a folder outside your music folder, was invisible to the library and to the
  `|` collection search no matter how much of it there was. Put the cursor on a
  folder in the browser and press `@` to add it; press `@` on one you have already
  added to remove it. A box asks first, because adding a folder starts a scan.
  Removing one takes its tracks out of the library straight away and never touches
  the files. Folders you add are remembered between runs. Adding a folder that is
  already covered, or one that contains a folder you already added, is refused with a
  message rather than quietly doing the wrong thing.
- **A folder that is not there is skipped, not emptied.** If one of your library
  folders lives on a drive that is unplugged, or you rename it, a rescan leaves its
  tracks in the library exactly as they were and tells you it could not read that
  folder. Before, anything the scan could not reach was treated as deleted.
- **Browse by genre with `g`.** Inside `[Library]`, `g` lists every genre in your
  collection and picking one narrows the artist list to it, then albums and tracks
  as usual. Tracks that carry several genres in one tag count under each of them, so
  a track tagged `Pop / Rock` appears under both. Genres separated by `/` or `;` are
  split apart; commas and hyphens are left alone, which keeps
  `Folk, World, & Country` and `Hip-Hop` intact as the single genres they are. A
  track with no genre tag is not filed under an invented heading - it stays
  reachable by artist, album and search. On this collection 35 different genre
  strings become 27 real genres.
- **Two new views on what you have played, with `%`.** Press it inside `[Library]`
  to see your most played tracks with their counts, press it again for everything
  you have never played, and again to go back. Never-played is most of a large
  collection, so it shows the first 500 with the true total beside it. These are not
  `[Recent]`, which is the short list of what you played last; these are the whole
  collection sorted two ways.

### Fixed

- **The track info pane (`i`) now shows the row you have highlighted.** Highlighting a
  file in the browser and pressing `i` used to show a track from the playlist instead
  of the one under the cursor, so the pane looked stuck on an unrelated song. It now
  follows the cursor through every listing - library artists, albums and tracks,
  collection search results, `[FAVs]`, `[Recent]`, `[Books]` and ordinary folders -
  and where a row is not a file at all, such as an artist or a folder, it says so
  plainly rather than showing something unrelated. Editing tags with `e` still works
  on playlist rows exactly as before; on a browser row it now declines and tells you
  to add the track first, rather than editing a different file than the one on screen.
- **Split play counts are now repaired, once, on the next start.** The reading fix
  below made the right number appear; this fixes the stored data, so a track that had
  two separate tallies now has one and every future play adds to it. On this
  collection 17 tracks were affected and not a single play is lost in the merge - the
  totals before and after are identical. **A backup of your settings file is written
  first**, as `remoct.conf.statbak` beside it, and RE-MOCT says so on the status line
  the one time it happens. If anything looks wrong afterwards, quit RE-MOCT, rename
  `remoct.conf.statbak` over `remoct.conf`, and start it again - you are back exactly
  where you were. The backup is written only once and is never overwritten by a later
  run. Nothing else in the file is touched, and no play history is discarded: stats
  for tracks you have deleted, or that live outside your library folders, are kept.
- **Play counts were being split in two, and the count shown was the wrong one.**
  RE-MOCT recorded plays under the exact path a track was opened by, and the same
  file opened from a folder, a playlist and a favourite could produce differently
  capitalised paths - so one file ended up with two separate tallies. On this
  collection 17 tracks were affected, and a track played 95 times showed as 73 or as
  22 depending on which tally was found first. Worse, a track whose only tally was
  stored under different capitalisation than the way you opened it read as never
  played at all. Counts are now added together, so the number shown is the number of
  times you actually played the file. This is a reading fix and nothing stored has
  been altered.
- The browser pane now always shows the row the cursor is on. Scroll down a long
  listing, go somewhere else and come back, and the pane used to return to the top
  of the list while the cursor stayed where you left it - so the highlighted row was
  off the screen and the arrow keys appeared to do nothing until you had scrolled
  back down to find it. The view now follows the cursor wherever it moves and
  whatever moved it: returning to a section, stepping between artist, album and
  track levels, rescanning with `F12`, searching, deleting a station, feed,
  favourite or bookmark, and resizing the terminal smaller. This was never specific
  to `[Library]` - it applied to any long listing, including a large folder,
  `[FAVs]`, `[Recent]`, `[Books]` and a podcast's episodes - but the library made it
  easy to hit, being the first section that routinely runs to hundreds of rows. The
  view moves as little as it has to, so a row that is already visible never shifts
  the pane. The playlist pane, which has always behaved this way, is unchanged.
- Compilations no longer shatter. A various-artists album used to appear in
  `[Library]` as one album per track, filed under a different artist each time, so a
  40-track eighties compilation became forty one-track albums under forty artists.
  They now group together under `Various Artists`, one row, with each track showing
  who performed it. On this collection that removed about a hundred artist rows that
  should never have been there. An album is treated as a compilation when it has no
  album-artist tag, or one saying something like `Various` or `Soundtrack`, and at
  least three different track artists - so an album with guest features on it, like
  a Gorillaz record with six credited artists, stays where it belongs rather than
  being scattered. Nothing about your files changes; this is how they are read.
- Opening `[Library]` is faster, and noticeably so on a large collection. Building
  the artist list was sorting one entry per track rather than one per artist: with a
  hundred thousand tracks that was about sixty milliseconds every time the section
  was opened or rescanned, and it is now about three.
- The left arrow now does the same thing as the `[Back]` row in every section,
  and leaving a section no longer moves you to a different folder. Previously the
  left arrow only really worked in the folder browser: in `[FAVs]`, `[Radio]`,
  `[Books]` and `[Recently Played]` it dropped you out of the section and, on the
  way out, moved you up to the parent of the folder you had been browsing - so
  you came back somewhere you had not been. In `[Podcasts]` it skipped past the
  show you were inside and left the section entirely, with no way to step back
  from a show's episodes to your list of shows. Now the left arrow steps back one
  level where a section has levels, leaves the section where it does not, and
  always returns you to the folder you were actually browsing. Every section's
  header also says how to leave it; four of them did not say before.
- In `[Radio]`, a saved station could stop playing when selected. After using the
  station search and then leaving `[Radio]` and coming back, the section drew your
  saved stations while still treating them as search results, so pressing Enter
  quietly did nothing at all. Two of these leftover search states were not being
  cleared; both are now.
- The `[FAVs]` header said `f:fav/unfav`. The favourite key is `*`; `f` toggles
  ReplayGain. The one section whose whole purpose is favourites was naming the
  wrong key for managing them.
- In `[Recently Played]`, the row at the top of the list is now labelled `[Back]`
  however you got there. Opening it from `[Drives]` labelled that row `[Drives]`,
  but it has always returned to the folder browser rather than to the drive list,
  so the label named somewhere it did not go.
- `[Drives]` now has a `[Back]` row, and the left arrow leaves it, the way every
  other section already worked. It was the one section with no way out: there is
  no parent directory to rise to from a list of drives, so the left arrow did
  nothing there, and the only exits were opening a drive or jumping sideways
  into another section. Whichever folder you were browsing when you opened it
  was simply unreachable again. Both now return you to it.
- Opening an older playlist no longer closes RE-MOCT. A .m3u or .pls written
  years ago, or by another program, stores its text in the Windows ANSI encoding
  rather than UTF-8, so an accented or punctuated filename inside it is not
  valid UTF-8. Handing one of those lines to the system's path handling raised
  an error that nothing caught, and the player exited on the spot - not a failed
  load, an exit. Any playlist naming a file with an accent in it could do it,
  which is to say a great many playlists that predate this one.
  Those lines are now converted to UTF-8 as they are read, so the track loads
  and plays instead of merely failing quietly, and a line that still cannot be
  made sense of is skipped and counted with the other missing files rather than
  taking the rest of the playlist down with it. Playlists already written in
  UTF-8, including everything RE-MOCT saves itself, are read exactly as before,
  byte for byte.
- A track added from the library no longer turns up twice in the playlist when it
  is already there. RE-MOCT has always refused to add a file the playlist already
  holds, but on Windows it decided that by comparing the two paths letter for
  letter, so the same file reached as `C:\Users\...` and as `c:\users\...` looked
  like two different files and got two rows. It is one file, Windows treats it as
  one file, and RE-MOCT now does too, quietly, exactly as it always did for a file
  you added twice from the same place. This was never only a library problem - the
  folder browser could do it to itself, and a saved playlist could come back with
  the same track in it twice - so the fix is in one place and every route into the
  playlist gets it. Playlists that already carry a track under both spellings lose
  the second copy the next time they load, which is a row that was never a
  different track.
  Two things this deliberately does not do. It does not touch different formats of
  the same song: `song.flac`, `song.opus` and `song.mp3` are three separate files
  and all three still go in, which is the whole point of keeping them. And on Linux,
  where two filenames differing only in capitals really are two different files,
  nothing changes at all - both still add.
- The play queue is unaffected, and that is on purpose: the queue is an order to
  play things in rather than a list of what you own, so asking for the same song
  twice in a row is a reasonable thing to want and still works.
- **`g` now closes the genre list as well as opening it.** It used to be a one-way
  door: once you were looking at genres, nothing inside `[Library]` took you back to
  the plain artist list, and the only way out was to leave the section entirely and
  come back. Pressing `g` a second time now returns you exactly where you pressed it,
  and the left arrow and `[Back]` agree with it. `%` already worked this way, and
  this is `g` catching up. The one cost, said plainly: leaving `[Library]` from the
  genre list now takes two presses instead of one, because each press of the left
  arrow undoes exactly one thing, which is how it behaves everywhere else.
- **The left arrow could stop working entirely.** Pressing `%` for the stat views and
  then `|` to search, then coming back, left the left arrow doing nothing at all -
  permanently, until you went somewhere else. `[Back]` was dead alongside it. It only
  needed two keypresses from opening the section, and it is fixed.
- **Genres written two ways are one genre.** `Post Punk` and `Post-Punk` were two
  rows; they are now one, listed under whichever spelling more of your files use, and
  opening it shows everything from both. Nothing is split to achieve this, so
  `Hip-Hop` is still one genre and not a "Hip" and a "Hop", and
  `Folk, World, & Country` is still itself and not filed under `Folk`. Your files are
  not touched, the tags are not rewritten, and a rescan produces exactly the same
  index it did before - this only changes which row a track is listed under.
- **Sections no longer throw you out when a folder changes on disk.** RE-MOCT
  watches the folder you are browsing and relists it when something appears there,
  which is right in the file browser and wrong everywhere else - it was still
  running while you were inside `[Library]`, `[Radio]`, `[Podcasts]`, `[FAVs]`,
  `[Recent]` or `[Books]`, and the relist tore the section down and dumped you back
  in the folder browser. On Linux, with the music on a shared folder from the host,
  it happened by itself about a second after opening the section, which is where it
  was found. It was never only a Linux problem: on Windows the same thing happened
  as soon as anything actually wrote into the folder you had been browsing - a
  podcast finishing its download into it, or a rip writing a track. Only `[Drives]`
  had been protected. The file browser still notices changes and still refreshes,
  which is the part that was always meant to happen.
- `?` now covers `[Library]`. Sixteen slices of it had gone in without the help pane
  ever mentioning the section existed, so there was no way to find out from inside
  the program that you could browse by artist, search everything you own with `|`,
  add a folder with `@`, or that `e` in the info pane edits tags. It says what exists
  and which key reaches it; the per-list detail stays in the header of the list it
  applies to, where it was already right.

### Internal

- The browser's section rows are now built from one list everywhere. `[Drives]`
  had its own hand-written copy of them, so the `[Library]` switch would have had
  to be applied in two places that must agree - which is the same way `[Library]`
  once ended up rendering at the bottom of the pane. Same rows, same order, one
  list.
- Groundwork for the library view: the metadata index, its on-disk cache format,
  and the artist/album/track queries the browser will read from. Nothing is
  wired to it yet, so there is no user-visible change in this entry - the index
  has no scanner to fill it and no screen to draw it. It is recorded here
  because the version now reads 1.5.0, and a build that says so should say why.
- More library groundwork: the scanner that fills that index. It walks the music
  folder, reads tags, and refreshes what it already knows by re-reading only the
  files that actually changed. Still nothing wired to a screen. The list of
  audio file types the player will open now lives in one place instead of two,
  which is a change with no visible effect today and one fewer way for the
  library and the playlist to disagree about what is playable later.

## [1.4.1] - 2026-07-25

### Changed

- Installing from source no longer builds the test tools, so a first install
  finishes considerably sooner. install.sh now builds the player and its
  streaming plugin and stops there; the forty-odd test programs that ship with
  the source are of no use in running RE-MOCT, and waiting for them to compile
  was pure delay for anyone who just wanted the player. Pass --with-tests to
  build them alongside it, or --tests-only to build only the test tools without
  installing anything. Building by hand with cmake is unchanged and still
  includes the tests, so existing build directories and scripts carry on
  exactly as before.
- CUETools rip mode now records its verdict in the files it produces, and says
  plainly that the verdict covers the whole disc. Ripping with [C] computes a
  CRC32 across the disc and checks it against the CUETools database; that result
  used to appear on screen and then be gone. Every track from the rip now
  carries the verdict and the disc ID it was checked under, so a rip can be
  re-checked later without reading the disc again. The verdict is one result for
  the whole disc, unlike AccurateRip which reports each track separately with a
  confidence count, and the wording on screen and in the rip log now says so.
  The [C] menu entry no longer claims no network is required, because the
  database check is an online lookup; only [Y] and [B] work entirely offline.

### Fixed

- The Discord activity timer no longer jumps back to zero partway through a
  radio song. Stations relabel the song they are already playing, moving a
  featured artist between fields or changing the spacing, and that was read as a
  different song starting: the timer restarted mid-song, sometimes more than
  once. The same song is now recognised through a relabel, the way scrobbling
  already recognised it, so the timer keeps running. A genuine change to a
  different song still updates everything as before, and artwork arriving a
  moment after a song starts now refreshes the cover without disturbing the
  timer.
- The Discord activity timer now restarts when a track does. Playing a track
  again left Discord counting up from the first play, so a repeated three minute
  song would show six, then nine, then longer, with no relation to where the
  track actually was. It resets on every replay now, whether the track looped
  under repeat-one, was started again from its row, or was a CD.
- Replaying a track now scrobbles again, instead of being silently dropped.
  Scrobbling recognised a track by its artist and title, so playing the same one
  a second time looked like the play it had already counted and was skipped.
  Repeat-one never scrobbled past the first pass, and neither did restarting a
  track after stopping it, or pressing enter on the row already playing. All of
  them now count, for files and for CD alike. Each pass still has to earn it by
  playing past the usual threshold, so a looping track scrobbles once per pass
  rather than once per restart, and each scrobble carries its own start time,
  which is what makes repeats legitimate rather than duplicates. Radio is
  unchanged: a station that relabels the same song mid-play is still counted
  once.
- The documentation describing how your credentials are stored was wrong, and is
  corrected. The README, the website, and the privacy policy all still said
  scrobbling credentials sat in the configuration file as plaintext. That stopped
  being true when credential protection landed, and the privacy policy put it
  most strongly, saying anyone with read access to the file could read those
  secrets. All three now describe what actually happens: which fields are
  protected, that Windows uses DPAPI bound to your user account, that the Linux
  side is obfuscation rather than encryption, and which fields genuinely do stay
  plaintext. Nothing about how credentials are stored changed here - only the
  description of it, which had been understating the protection and overstating
  the risk at the same time.
- The example configuration in the README and on the website was invented rather
  than copied from a real file, and several of the keys in it do not exist:
  nothing reads or writes `repeat`, `crossfade_ms`, `theme`, `vol`, or
  `eq_gains`. Anyone who copied those examples was editing keys that could never
  take effect. Both are now a trimmed extract of a real generated configuration,
  with the values that need explaining described underneath rather than invented
  inside. The website also pointed at the wrong configuration directory and did
  not mention the Linux location at all.
- The website still described CUETools mode as requiring no network, and still
  listed the rip output as FLAC and MP3 only. The database check is an online
  lookup, and the tagged formats have included Opus, WavPack, and M4A for some
  time. Both corrected, along with the CUETools verdict and disc ID now being
  listed among the tags a rip writes.

## [1.4.0] - 2026-07-24

### Added

- Podcast episodes now show chapters published by the feed, not only chapters
  embedded in the audio file. Many shows carry a chapter list alongside each
  episode; pressing `;` on such an episode shows it - even before the episode is
  downloaded, fetched on demand over the network with a brief "Loading
  chapters..." while it lands. A downloaded episode keeps its chapters offline:
  they are saved beside the audio when it downloads, so an episode pulled at home
  still has its chapters on a train with no signal. Selecting a chapter needs the
  episode on disk to play from that point ("Download the episode to play from a
  chapter"), but viewing the list never does. Chapters that come with the audio
  file itself still take precedence, and an episode that publishes none says so
  honestly. A cached chapter list is refreshed if the show moves it or after a
  week, so a corrected list is picked up without a manual refresh.
- Queued tracks now crossfade. Jumping to a queued track - and from one queued
  track to the next - used to hard-cut even with crossfade on; only the return
  to the playlist after the queue drained ever faded. Now every queue transition
  fades like a normal track change, using the same crossfade length. The queue
  still behaves as before otherwise: queued tracks play first, then playback
  returns to the playlist. Queueing a track during an already-running fade still
  cuts (that fade is already sounding), and removing a queued track before it
  starts means the fade goes to whatever is actually next.
- `remoct --version` and `remoct --help` (also `-V` and `-h`). Until now every
  argument was treated as a file or folder to play, so asking the program what
  version it was launched it instead. Both flags now print and exit without
  opening an audio device or taking over the terminal, which is what a packaged
  install expects when it checks itself after installing. The help lists the
  usage and points at `?` for the key bindings.
- An `install.sh` for building and installing from source on Linux. It builds a
  release, installs the player and its streaming plugin to a prefix (default
  /usr/local, `--prefix` for anything else), and asks for elevation only when
  the destination actually needs it. `--no-build` installs an existing build,
  `--link` points the installed command at your build tree for development, and
  `--uninstall` removes exactly what was installed.

### Changed

- CUETools rip mode now records its verdict in the files it produces, and says
  plainly that the verdict covers the whole disc. Ripping with [C] computes a
  CRC32 across the disc and checks it against the CUETools database; that result
  used to appear on screen and then be gone. Every track from the rip now
  carries the verdict and the disc ID it was checked under, so a rip can be
  re-checked later without reading the disc again. The verdict is one result for
  the whole disc, unlike AccurateRip which reports each track separately with a
  confidence count, and the wording on screen and in the rip log now says so.
  The [C] menu entry no longer claims no network is required, because the
  database check is an online lookup; only [Y] and [B] work entirely offline.
- Crossfade is now configurable - and off by default. A new `crossfade` key in
  remoct.conf sets the fade length in seconds; 0 (or absent) means no fade, the
  MPD convention. Until now every transition faded over a fixed two seconds with
  no way to change or disable it, which damaged gapless album transitions. Album
  listeners get clean gapless boundaries out of the box; set `crossfade=2` to
  keep the old behaviour. Values are clamped to 0-30 and a malformed value falls
  back to off.
- Next and previous now navigate under repeat-one, and behave the same as each
  other. Pressing n used to replay the repeating track while p skipped backward -
  an inconsistency, not a design. Both now move through the playlist normally
  with repeat-one staying on, and whatever ends up playing becomes the track
  that repeats.

### Fixed

- The chapter list (`;`) now works from the file browser - `[Books]`, plain
  directories, and downloaded podcast episodes - not only from the playlist. It
  used to answer "no chapters" for a book browsed in `[Books]` without ever
  looking at the file; the same book showed its chapters fine from the playlist.
  Selecting a chapter starts playback at that chapter, wherever the list was
  opened from, and an explicit chapter choice wins over a stored resume
  position - picking chapter 1 of a half-finished book starts at chapter 1, not
  at the old bookmark. (Podcast episodes now also show chapters published by the
  feed, described under Added - so an episode you have not downloaded can show
  its chapters too.)
  and starting the app again restored the mode on screen but not in the audio
  engine, so the engine-side repeat safeguards sat inert until the repeat key
  was pressed once. The restored mode now reaches everything it should.
- A queued track no longer stutters its first moment when it starts at a
  tape-speed track end. With playback speed set off normal, the end-of-track
  handoff started the queued track and then immediately restarted it from zero,
  doubling the first fraction of a second - audible on a quiet intro.
- The OS media controls now show the real duration while a CD plays, instead of
  duration 0. The in-app progress bar was always right; only the OS card read
  from the wrong clock.
- The OS media controls now show real track information for a CD that has no
  MusicBrainz match, instead of keeping the previous track's details on screen.
  The card shows the same track title the playlist shows for the disc and the
  CD's own clock. Unmatched CD tracks still never scrobble - a track number is
  not a song identity.
- Reaching the end of the playlist now stops playback cleanly. With repeat off,
  the last track used to finish and then hang - the player stayed stuck showing
  the finished track at full duration with the play indicator still up, and the
  only way out was stopping it by hand. Playback now goes to stopped at the end,
  the same as pressing next past the last track always did.
- Repeat-one no longer bleeds the next track. With crossfade on, a track set to
  repeat could be heard mixing with the following track for the whole crossfade
  before looping back to itself, and it recurred on every loop. Repeat-one now
  loops cleanly and silently, whatever else is queued up behind it.
- Queued tracks now play while repeat-one is on. Queueing a track with repeat-one
  active could leave it stuck in the queue indefinitely, because the repeating
  track never signalled that it had ended. The queue drains normally again.
- Windows: the OS media controls (lock screen and the volume/media flyout) now show
  "RE-MOCT" as the app instead of "Unknown app". On launch RE-MOCT ensures a
  Start-menu shortcut to itself exists carrying its application id (the silent,
  no-installer way Windows resolves an app name); it heals the shortcut if the exe
  moved. Cosmetic only and best-effort - it never blocks or affects startup, and
  Linux is unaffected.
- Podcast episode rows: the new/downloaded/in-progress/played state marker and the
  resume time now stay pinned while a long title scrolls, instead of marqueeing off
  with the text. The state is readable at every scroll position.
- A playing podcast episode now shows the show's cover art on the OS media controls
  (Windows and Linux) and in Discord Rich Presence, instead of the audio file's
  embedded picture or a mistaken album lookup. An `.m4b` episode no longer resumes
  twice (the audiobook and podcast resume no longer both fire on it).

### Changed

- Podcasts: add a feed with `a` (was `/`).
- Durations display as `H:MM:SS` (or `M:SS` under an hour) instead of raw seconds -
  the Track Info pane now shows `3:12`, not `192s`, and reads sensibly for long
  podcast episodes and multi-hour audiobooks. Display only; saved positions and
  playlist/cue files are unchanged.
- Searching a list with `\` and finding nothing now reports it in the status line
  (naming the query and which list was searched, e.g. `No match for "…" in browser:
  Music`) instead of a pop-up toast - so a focus-aware miss reads clearly and a
  wrong-pane search is obvious.
- Entering a podcast feed shows a "Loading …" working state in the status line, in
  the same voice as download progress, instead of a toast; it stays up for the whole
  fetch of a large feed.
- Finished-operation status messages - a completed rip, ReplayGain scan, or convert -
  now linger about 5 seconds, matching a finished podcast download.

### Added

- Mark a podcast episode played or unplayed by hand: `y` on an episode toggles its
  state, and the row marker updates. It is a status change only - your resume
  position and the downloaded file are left exactly as they were.
- Find and subscribe to new podcasts by search, not only by pasting a feed URL.
  Press `/` in the `[Podcasts]` section to search the Podcast Index directory;
  matches appear in the same list as your feeds and Enter subscribes, returning to
  your feed list with the show added. It uses your own free Podcast Index API key:
  the first time you press `/`, RE-MOCT offers to open the signup page or let you
  enter a key and secret (shown on screen so a launcher that cannot open a browser
  is not a dead end), and the secret is protected at rest like the Last.fm one.
  Every failure degrades to a status-line message: no key, a wrong key or a drifted
  system clock, a network drop, or zero results never blocks pasting a URL, and a
  search never freezes the interface. Already-subscribed shows are named, not
  duplicated.
- The `\` search-results list now takes PgUp/PgDn/Home/End for fast movement
  through large result sets.
- `\` now searches the list in the focused pane, not always the playlist. In the
  file browser it searches the current view - and because every browser section
  (`[Podcasts]` feeds and episodes, `[Radio]`, `[Books]`, `[FAVs]`, `[Recent]`,
  `[Drives]`, and plain directories) shares one list, they all gain search at
  once; the file browser had none before. Matching is case-insensitive over the
  text shown on the row; a single hit jumps straight to it, several open the same
  pick-list as playlist search, and Enter lands on the row with the browser still
  focused. Playlist search is unchanged. If the list is rebuilt while a result
  overlay is open, the overlay closes rather than jump to a since-moved row.

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
  a real podcast reports the problem and changes nothing.
- Play an episode by selecting it: RE-MOCT downloads it (with an on-screen
  percentage, like a CD rip) and then plays it, so you get full seeking, the real
  duration, and embedded chapters. The transport controls (pause, seek, volume)
  work as they do for any track.
- Episodes remember where you left off. Come back to a half-finished episode and
  it resumes at your position; two episodes in progress keep their own positions.
  The list marks each episode new, in-progress (with the resume time), or played,
  and an episode that reaches the end is marked played and stops (it does not
  auto-play the next one). Positions and states are saved across restarts.
- Queue episodes for offline listening: Shift+D on an episode downloads it for
  later (without playing). The queue holds up to 5 and downloads them one at a
  time, in order, each row showing its percentage while it works and "queued"
  while it waits; a download that fails is retried up to three times, then skipped
  so one bad episode never stalls the rest. Downloaded episodes are marked so, and
  play instantly and fully offline.
- Delete a downloaded episode with d or Del to free space - your resume position
  and played state are kept, so re-downloading picks up where you left off.
- If you press play on an episode while a different one is still downloading,
  RE-MOCT asks whether to wait (queue it to play next) or play now (interrupt the
  download, which restarts later).
- A playing episode shows its cover in the art pane - the episode's own image if
  it has one, otherwise the show's poster - as truecolor half-blocks, just like
  album art for music. The show art is cached to disk, so a downloaded episode
  still shows its poster offline. A show with no art simply leaves the pane empty.
- In `[Podcasts]`, the info pane follows the highlighted row - playing or not. It
  shows the highlighted show's poster (feed list) or the highlighted episode's
  cover and title (episode list), never whatever music track the playlist last
  pointed at. A playing episode's own cover shows when you browse away from the
  podcast pane, so the pane still follows now-playing elsewhere.

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
