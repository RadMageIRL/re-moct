# RE-MOCT - hard-won lessons

## ncurses / rendering
- **`COLORS=8` everywhere** on this ncursesw build; 256-color is blocked. Don't rely
  on >8 colors.
- **Wide API for every Unicode glyph.** Narrow draw functions (`mvwaddstr`,
  `mvwaddnstr`) do NOT decode UTF-8 here. Use `setcchar` + `mvwadd_wch` /
  `mvwaddnwstr`, with `NCURSES_WIDECHAR` defined before the ncurses include.
- **Measure/cut by display columns, not bytes.** The Tier-6 pipeline replaced
  byte-based slicing with a self-contained column-counter in StringUtils
  (`utf8_next`, `cpWidth`, `dispWidth`, `truncateToWidth`, `truncateToWidthRight`,
  `padToWidth`, `scrollToWidth`). Platform-independent, probe-validated.
- Astral/emoji (surrogate pairs, Windows 16-bit wchar_t) may not render; folded to
  '?' - acceptable, no regression.
- Color pairs 1–13 are themed (init loop); `CP_VIZ_TIP` (14) = peak fg on default bg,
  init_pair'd after the loop. Viz pairs are fg==bg solid space-fills, so a partial
  block needs a real bg - that's why the sub-cell tip uses `CP_VIZ_TIP`. Theme reload
  re-runs `initColours()`, so new pairs added there survive `~` live-reload.
- **A curses pair index is a handle into a global table, not a colour.** `init_pair()`
  mutates process-global state. Caching pair indices per art source (`info_art_pairs_`
  and `radio_art_pairs_`) produced two vectors of identical integers pointing at one
  table; whichever source allocated last owned the colours, and the other silently
  rendered someone else's image (station art -> file art -> back to station showed the
  file's cover). Cache the RGB grid; allocate at the blit; record who owns the table
  (`art_pairs_key_`). Do NOT "optimise" this back into a per-source pair cache - that
  is the bug. (Slice 2.)
- **The art pair base (`kArtPairBase = 20`) sits directly above the theme's CP_* pairs,
  which have grown 13 -> 17** (`CP_VIZ_LOW_B/MID_B/HIGH_B` = 15/16/17). The gap is two.
  The `static_assert(CP_VIZ_HIGH_B < kArtPairBase)` in `UIManager.h` is the only thing
  standing between a new visualizer colour role and art cells being silently repainted
  on every theme apply. If you add an LED role and it fires, raise `kArtPairBase` and
  re-check `p_budget` in `allocArtColorPairs()`.
- **Playlist move rebound `u`/`D` -> `K`/`J`.** `J`/`K` were freed from main-handler
  navigation (they were pure dupes of `j`/`k`) and repurposed to move-down / move-up;
  `j`/`k` + arrows still navigate. Sub-pane `J`/`K` scroll aliases (help, device picker,
  EQ, bookmarks) are left alone - no move command there to collide, stripping them is
  pointless churn (per-pane key meaning is already the design). `u`/`U`/`D` dropped with
  NO alias (no-dupe policy). `d` stays delete (MOC homage). This kills the confusing
  `d`/`D` adjacency (capital-D move-down next to lowercase-d delete). **Slice 13 TODO:**
  the public keybind reference `docs/index.html` still says "u / D" at lines 479 and 713
  - update both to "K / J". The in-app help table (UIManager.cpp) is already updated. (Slice 11.)
- **The Ctrl+Y / Ctrl+F overlays already called `panelFrame` but passed an empty title
  and hand-drew a centered one, so Awesome never themed them.** `panelFrame` draws its
  inset themed title only in the Awesome branch; the Classic path is an early
  `box(w,0,0); return;` with no title. So the fix passes the real title to `panelFrame`
  (Awesome themes it) AND keeps the manual centered draw guarded on `!awesome_mode`
  (Classic still needs it). Chose 2b (pass-title + guarded-manual) over 2a (teach
  `panelFrame` a Classic title) because every pane draws its OWN Classic header (a
  full-width bar, not via `panelFrame`), so a Classic title in `panelFrame` would
  double them. Second bug (what the frame styling actually hinged on): the overlays are
  `newwin` modals with NO `wbkgd`, and on wingui a fresh window needs its background set
  for `COLOR_PAIR()` to render - so the themed frame colours AND the wide-char rounded
  corners silently dropped, leaving a plain square box even after the title was routed
  correctly. The panes never hit this because `createWindows` gives every pane
  `wbkgd(CP_DIM)` ("so colors render correctly"), and in Awesome `CP_DIM` carries the
  themed base fill. Fix: give the overlays the same `wbkgd(CP_DIM)` in Awesome (plain
  default in Classic). Passing the title was necessary but not sufficient. (Slice 9+10.)
- **`allocArtColorPairs`' pair-budget fallback must find the nearest existing pair, not
  return `P0`.** The 22x11 art box (242 cells) exceeds the 236-pair budget, so busy
  covers overflow on real art (Probe A2: 6 of 116 covers). The old fallback returned pair
  20 flat, speckling overflow cells with one wrong hue. It now mirrors `colorFor`'s
  nearest-existing search. The catch: `fg`/`bg` reaching `pairFor` are slot IDs, not RGB -
  map back through `pal` (truecolor, slot = `C0+i`) or `kBasic16` (non-truecolor, slot =
  0..15) to score by colour distance. (Slice 8.)

## AccurateRip / CD ripping
- **The 150-sector physical preamble is correct BY DESIGN** (a property of how
  AccurateRip works, confirmed across HydrogenAudio threads). Do NOT re-litigate.
- Disc ID normalization is relative to the **T1 LBA**, not a hardcoded 150 - handles
  non-standard pregaps.
- Enhanced CD needs the full-disc-leadout disc ID, binary-search silence detection,
  and a +174-sector correction.
- `mul_by` increments for **every** sample, including skipped boundary samples.
- `AR_SKIP = 2940` (not 2941). CRCv2 = `csum_lo + csum_hi`.
- The negative drive-offset OOB read is real but BLOCKED on testing (Dos's drive is
  +6); needs a synthetic negative-offset vector. Don't patch blind.
- **The HL-DT-ST/LG/HLDS drive family holds a soft media-removal lock after raw
  reads that a bare `CloseHandle` does not clear** - the physical eject button
  stays dead for several seconds after stop ("push it repeatedly"); an Asus
  SDRW-08U7M-U releases cleanly from the identical code path, which is what
  localized the difference to drive firmware, not our close logic. Fix: clear the
  lock explicitly before closing - `IOCTL_STORAGE_MEDIA_REMOVAL` with
  `PreventMediaRemoval = FALSE` in `~WinCdDevice()`, return value ignored
  (best-effort; harmless on drives that don't need it). The destructor runs on
  every `dev_.reset()`, so the tray is handed back whenever RE-MOCT lets go of
  the drive. Windows-only semantics; the SG_IO path has no such lock.
  Hardware-confirmed 2026-07-09: the GHD3N releases promptly after stop with the
  unlock (it was error-1-locked to even software eject before). Deliberate
  non-goal: the drives differ on eject DURING playback (the Asus allows it, the
  HL-DT requires stop first) - that is firmware behaviour, not a defect; do not
  try to normalize it.
- **Shift+E eject: E was a pure dupe of e (same case line at all three sites -
  EQ toggle x2, tag-edit entry), so freeing it cost nothing** - e alone still
  does EQ and tag-edit everywhere; E acts only in [Drives], per-pane key meaning
  like d / K / J. Media detection for the eject hint is computed ONCE per drive
  enumeration (entering [Drives] / F12) and cached in cd_drives_with_media_ -
  NEVER polled, or the CHECK_VERIFY-class probe reintroduces the slice-1 idle
  drive spin-up. Consequence, by design: insert a disc while sitting in [Drives]
  and the hint waits for F12. Eject goes through the seam (`ICdDevice::eject()`,
  defaulted virtual so test fakes keep compiling): IOCTL_STORAGE_EJECT_MEDIA on
  Windows (allow-removal first, same as the destructor unlock), START STOP UNIT
  0x1B LoEj on Linux. Ejecting the loaded disc stop-first + full slice-3 unload;
  ejecting mid-rip is REFUSED (the rip owns the platter).
- **On a lock-on-read drive, never "recover" a failed eject with reads.** The
  HL-DT refuses software eject (firmware) AND its failed eject wedges the
  physical button. A wedge-breaker that reproduced the play->stop shape with a
  fresh handle + TOC + raw read made it WORSE: raw reads RE-ASSERT the soft
  media-removal lock on this family, so the "recovery" deepened the wedge and
  poisoned the real play->stop release too (hardware-tested, reverted). What
  play->stop actually provides is settle-time around the reads, not the reads.
  The shipped behaviour: eject() does unlock -> eject -> unlock-on-fail, the
  destructor unlock stays unconditional, the failure path does NOTHING else,
  and the toast tells the truth ("This drive won't eject by software - stop
  playback and use the drive button"). Asus-class drives never hit any of
  this - their eject succeeds.
- **Drive hot-plug is not auto-detected - F12 is the manual refresh.** `[Drives]`
  only rebuilds on entry (`enterDriveList()`), and the periodic dir re-scan
  deliberately skips the drive list, so a USB drive plugged in mid-session never
  appears until re-entry. F12 just re-runs the existing `enterDriveList()`
  rebuild (idempotent - flags + list + cursor reset), restoring the cursor onto
  the previously-selected entry when it survives the refresh. Note the browser
  pane has NO draw-time scroll invariant (unlike the playlist's slice-5 one):
  j/k nudge `dir_scroll_` per-handler, so anything that moves `dir_cursor_`
  behind the draw's back must re-clamp `dir_scroll_` itself.
- **Stopping a transport is not unmounting a device. (Slice 3)** `cd_mode_` means "CD is
  the active audio source." `stop()` correctly closes the drive handle - probe B2: holding
  it open idle spins the drive up audibly, and the syscall returns before the motor is at
  speed so latency can't see it, you have to LISTEN - but a vestigial 6-second poll then
  purged the CD rows + MusicBrainz metadata of the just-stopped disc. That poll never checked
  real media (Slice 1: it only ran `checkMedia()` on the now-null handle, always false) - a
  pure destruction timer. The rows + MB metadata live in **UIManager**, not the handle, and
  `cd_drive_letter_` already survives `stop()` as the faithful "a disc is loaded" signal, so
  NO new flag was needed (a `cd_loaded_` atomic would just be a second source of truth to
  desync). Fix: delete the poll purge; key the `[CD]` tag + album off `cd_drive_letter_` so
  they persist while stopped-but-loaded; reopen the handle lazily on the next Enter / Ctrl+R /
  Ctrl+Y via `reopenCDForAction()` -> `openCD()`, which re-reads the TOC and re-inits the
  device (`initCDDevice()`, extracted so no reopen path skips `ma_device_` setup). `openCD()`
  returns false cleanly on an empty tray, so it doubles as the eject-while-stopped presence
  check (the ONLY detection point once the poll is gone): on failure, purge rows + clear
  `cd_drive_letter_` + toast. Route reopen through `openCD()`, NOT a self-reopening
  `playCDTrack()` - keeps `playCDTrack()` lock-free and unchanged, and every reopen goes
  through the one function that already does open + `initCDDevice` + failure-cleanup.
- **A failed reopen is not proof of an empty tray - discriminate, and when unsure keep the
  disc. (Slice 3 re-gate)** The first hardware gate caught it: a reopen right after file
  playback can hit a TRANSIENT `openCD()` failure (WASAPI uninit->init settling), and a bare
  single attempt escalated that blip into the destructive eject purge - a CD->file->CD
  round-trip unloaded a physically-present disc. `checkMedia()` cannot discriminate
  transient-vs-ejected: after a failed `open()` `dev_` is null, so it short-circuits false
  without ever asking the drive. Two discriminators instead: (1) bounded retry - 3 attempts,
  ~200 ms apart, inside `reopenCDForAction()` only (an explicit user action; never a poll, so
  no spin-up, no seek-storm); (2) the failure POINT - `CDSource::lastOpenFail()` records
  where `open()` died: `DeviceOpen` (OS handle couldn't be acquired - busy/contention, NOT
  proof of an empty tray) vs `TocRead`/`NoAudioTracks` (the drive ANSWERED and there is no
  readable audio disc - confirmed empty). Purge only on confirmed-empty; on busy, keep the
  rows + `cd_drive_letter_` and toast "drive busy" - a stale row self-heals on the next
  action, a nuked loaded disc does not (this is exactly pre-Slice-3 behaviour for a failed
  `openCD`: benign false, nothing destroyed). Every failed attempt is LOGGED (`Log "cd"`:
  drive, failure point, attempt) so a misbehaving drive is diagnosed from the log, never by
  asking someone to catch a flashing toast by eye. Don't put the retry inside `openCD()`
  itself: it also serves genuine first-loads, where a slow real failure shouldn't stall the
  UI.

## Streaming ring buffer / re-pin
- **An iHeart hole where iHeart's own trackHistory/ctm reports no current song
  is broadcast-side (unfixable); one where OOB shows fresh songs while our
  session loops a stale slate is session-side (the fixable class). The OOB
  liveness signal is what separates them, and it's the correct gate for any
  re-pin.** From the 2026-07-03 desync analysis: the longest, scariest holes
  (8–12 min single-cartcut loops) had NO song playing per iHeart's own
  endpoints - nothing to rejoin, by design; the fixable 131–300 s holes had
  fresh songs provably airing while our edge stayed stale. A re-pin gated on
  OOB liveness cannot fire into a real broadcast break, because the gate was
  false throughout every one observed.
- `ringClear()` is called in BOTH the AAC and MP3 re-pin paths, after
  `prebuffered_.store(false)`, to break a producer/consumer deadlock on self-heal.
  It's load-bearing for ad-skip re-pin crossfade - do not "clean it up."
- `LIVE_STALL_MS = 35000`. `prebuffered_` + `ringClear()` interactions are subtle;
  changes here need explicit justification and confirmation.
- **ICY now-playing only updates when the broadcaster actually sends a StreamTitle in
  the metadata block on a track change.** Plenty of stations skip it on some
  transitions (or send station IDs instead of songs). So a label that fails to update
  across one transition is the STATION's silence, not a client parsing bug - rule out
  the broadcaster before suspecting `readAudio`/`parseIcyMetadata`. Ruled out exactly
  this way during the slice-4 ICY regression gate (a missed update on one transition,
  station-side). Related parked items: ICY metadata improvement is a structural
  protocol limit; ICY ingest sanitization (station-ID stripping) - see `roadmap.md`.

## Playlist cursor / follow / tag edits (the fix-plan queue)
- **Follow the now-playing answer, never a proxy for it.** Three separate cursor bugs -
  the lit row and cursor yanking to a stale FILE row while a CD played, file->CD not
  following because `nowPlayingRow()` had no CD branch (7dbb662), and song/CD->radio
  never snapping because the follow-sync watched `playlist_.current()`, a raw index
  that starting a stream never moves (91817b8) - were all the same mistake: inferring
  "which row is playing" from `current()` or a stale `currentTrack().path` instead of
  asking `nowPlayingRow()`, the one function that answers it correctly for every
  source (file by path, stream by URL, CD by track number). The fix in each case was
  to route through `nowPlayingRow()`. Derived state beats event plumbing: the
  follow-sync now does one per-tick comparison of `nowPlayingRow()` against a
  `last_now_playing_row_` marker, so any future source gets follow behaviour for free
  the moment `nowPlayingRow()` understands it - no transition path has to notify the
  cursor. When a follow/display behaviour works for some sources and not others,
  suspect a missing per-source branch or a proxy variable; do NOT rationalise it as a
  "display nuance" - it was mistaken for exactly that twice before the third head
  surfaced the family.
- **`current()` is an index identity; `nowPlayingRow()` is a display target - they
  are deliberately NOT the same function.** `playlist_.current()` goes stale in
  stream mode (`currentTrack()` holds the last file; a queue-launched station has no
  row) but is still the correct anchor for seek-stamp and auto-advance, which reason
  about index identity, not what's on screen. `UIManager::nowPlayingRow()` is the
  canonical "which row is lit / shown" test (source-aware; nullopt when nothing plays
  or the playing source matches no row). Do NOT merge them - a UI fix must not silently
  change auto-advance's notion of the current index. (Slice 4.)
- **`pl_scroll_` has exactly one owner: `ensurePlaylistCursorVisible()`.** The playlist
  scroll offset used to be maintained by a dozen hand-rolled "if cursor < scroll, nudge"
  fragments plus cursor-blind `pl_scroll_ = 0` resets; any path that moved the cursor
  without a matching nudge (radio-add, CD-eject) left the cursor lit but offscreen. The
  invariant clamps cursor-into-range then scrolls the minimum to reveal it, enforced once
  at the top of `drawPlaylist()`. Do not reintroduce per-handler scroll math - move the
  cursor, let the draw reveal it. (There is no PgUp/PgDn/wheel path that moves scroll
  independent of the cursor, so draw-time enforcement is sufficient; if one is ever added,
  it must call the invariant on the cursor mutation.) (Slice 5.) The same one-owner
  discipline now covers the follow snap on `pl_cursor_`: the every-tick follow-sync
  block is its single owner (91817b8).
- **The `d`-delete "was this the playing row" test must use `nowPlayingRow()`, not
  `current()`.** `current()` is the stale file index in stream mode, so the old test could
  `stop()` a stream when deleting an unrelated row (or fail to stop when deleting the
  actually-playing file). (Slice 5.)
- **Follow-the-playing-row (F3, `follow_playing`, default ON) snaps on a CHANGE in
  `nowPlayingRow()`, observed every tick - never on a change in `current()`.** Starting
  a stream doesn't move `current()`, so a `current()`-gated snap is blind to song->radio
  and CD->radio transitions (launch sites setting `pl_cursor_` directly masked this for
  years). Only the cursor move is gated by `follow_playing`; track announcement (toast,
  `recordPlay`, the `last_playlist_current_for_sync_` marker) stays `current()`-keyed and
  unconditional - those announce the track, they don't chase it. A queue-launched station
  returns nullopt, which resets the follow marker and leaves the cursor put. EVERY
  track-change cursor move must respect the toggle, not just auto-advance: the manual n/p
  handlers also gate their cursor set on `follow_playing`, or OFF looks identical to ON
  when you change tracks by hand. (Slice 6; mechanism finalized in 91817b8.)
- **A loop-scope `static` is a global in disguise - the spectrum's "per-band"
  AGC was one shared scalar.** `static float peak_mag` declared INSIDE the
  per-band loop of `computeVizBins()` made every band normalize against the
  loudest (bass) band's rolling peak, permanently pinning the treble low; the
  comment said "auto-scales to the actual signal level" as if per-band, but the
  scope said otherwise. Fix (viz-normalize A+B): per-band rolling peaks as
  MEMBERS (`viz_peak_[]` + `viz_global_peak_`, slow 0.9995 decay) with each
  band's effective peak floored to `VIZ_PEAK_COUPLE` x the global peak - fully
  independent per-band normalization is NOT the goal (it flattens the real
  spectral shape and lets quiet bands pump on noise); couple it. Plus a
  perceptual treble tilt applied BEFORE peak tracking (after = double-count).
  Deliberately MODE-AGNOSTIC: the DSP feeds `viz_smoothed_[]`, consumed by
  Classic AND Awesome and both F2 styles - "Classic looks fine" was the bug
  wearing Classic's minimal vibe. If one house tunable setting can't serve both
  modes, that's a separate decision; do not gate shared-DSP fixes by mode. The
  stride-4 DFT still aliases above ~sr/8 (~5.5 kHz @ 44.1k), so the top ~10
  bars are "alive but low-information" after this fix - that ceiling is Slice
  C's (real FFT) go/no-go trigger, measured not theorized.
- **Bare F-keys are unreliable cross-platform - prefer Shift+letter for new
  bindings.** Terminal emulators claim an unpredictable subset of F-keys at the
  window/terminal layer before the app ever sees them (F1 help, F10 menu, F11
  fullscreen in GNOME Terminal/Konsole/xterm), and you cannot test every
  terminal a user might run; it "works on Windows" only because the wingui GDI
  app IS the window. Shift+letter combos are never intercepted - categorically
  safe. The filetype toggle shipped as F11, fullscreened Linux terminals
  instead, and moved to Shift+F (f=ReplayGain, F=Filetype - the e/E grammar).
  F2/F3/F7/F8/F12 are verified-working keepers, not precedent for new ones.
- **A per-row marquee must evaluate against the SAME width the row renders
  with.** The F11 filetype column narrows the title field; `scrollToWidth`
  decides scroll-vs-pad from the width it is handed, so the row's title width
  (`nw`) is computed ONCE - with the column width already subtracted - and that
  single value feeds both the marquee decision and the render. Compute the
  width in two places and a long title scrolls against the wrong width: under
  the filetype column, or not at all. Also: a blank tag (CD/stream/unknown
  rows) zeroes the column for that row, so the title reclaims the space - the
  column is per-row, not global.
- **Playlist search (\) is pick-to-jump, deliberately NOT a filter.** The results
  overlay holds REAL playlist indices; picking one is a single `pl_cursor_`
  assignment (scroll = the slice-5 invariant, follow = untouched since nothing
  about what's playing changes). Narrowing the list instead would create a
  display-index/real-index duality across every playlist system (move, delete,
  lit-row, follow) - the exact proxy-vs-direct class the three cursor bugs came
  from. Both halves reuse existing machinery: the goto InputMode line (one enum
  value + one prompt + one confirm case) and the Bookmarks-popup results
  pattern. Match against `display_title` - the text the row actually shows -
  never re-derived tags, or search finds things the user can't see.
- **`TagLib::FileRef::save()` returns bool; on Windows a locked-file write returns false
  without throwing.** The old `saveTagEdits()` discarded the return inside a `catch(...)`
  and then updated the playlist title + cleared the info cache unconditionally - so a
  failed save (e.g. the file is the playing track, decoder holds the handle) silently
  showed as success while the disk was unchanged. Always check `save()`, and update UI
  state ONLY on true; on false, touch nothing and warn. Editability (playing-locked / CD
  track / radio stream / empty) is now one predicate, `tagEditability()`, shared by the
  `e` entry guard and the save. Note: on a failed save we exit edit mode rather than
  stay - staying would trap the user, since `s` (stop) is captured as text inside edit
  mode. (Slice 7.)

## MP3 seek (bit reservoir)
- MP3 frames borrow main_data from up to ~511 bytes of preceding frames (the bit
  reservoir). A raw seek lands "cold" → the first ~50–150ms decode garbled until it
  warms up. FLAC has no reservoir, so FLAC seeks are clean - that's the whole asymmetry.
- **Prime-after-seek:** in `seekTo`, land ~0.18s before the target and decode-discard
  up to it so the reservoir is warm before audio resumes. Harmless for FLAC. Runs in
  the device-stopped window (no callback race; callback also guards on `seeking_`).
- **Seek coalescing:** holding `[`/`]` fires many key-repeats; each raw seek does a
  device stop/seek/start + warm-up. `requestSeek`/`flushPendingSeek` apply single taps
  instantly and collapse held repeats to ~one seek per 100ms.
- **Track-stamp guard:** a buffered seek is tagged with the current playlist index and
  discarded on flush if the track changed - prevents a seek aimed at track A executing
  on track B. Costless, no deadzone.
- A 300ms post-track-change **lockout was rejected** - silent dropped seeks every
  track change were worse than the rare benign held-key bleed. Lay tradeoffs out.

## Cover art
- iTunes `entity=song` + Deezer `/search/track`, with **dual artist+title
  validation**, prevents wrong-artist matches. CAA is unreliable for Discord (returns
  "?" for missing front covers) - route Discord art through iTunes/Deezer.
- `CoverArt` namespace: `bytesByMbid`/`bytesByText` (tag embed), `urlByText`/`urlBySong`
  (Discord URLs). httpGet caps body at 10MB and rejects truncated-on-cap bodies.

## Process / workflow lessons
- **The disconfirmation discipline pays for itself.** Briefs assert mechanisms at
  file:line anchors; the implementer reads the tree and confirms each before coding,
  and a mechanism divergence is a full stop, not a footnote. In the fix-plan queue it
  caught a placebo fix before it shipped (a prescribed closeCD->stop swap at the
  file-switch sites traced to functions that were already behaviourally identical -
  the real bug was a transient openCD failure escalating into the eject purge) and
  turned Slice 3's prescribed `cd_loaded_` atomic into the leaner reuse of
  `cd_drive_letter_` (one source of truth instead of two that can desync). "The brief
  is wrong" is a valid, expected outcome; where the tree and the brief disagree, the
  tree wins.
- **Additive-only.** Full-file replacements built on a stale baseline silently drop
  prior work. Build on the files Dos uploads in the same turn; prefer tight diffs when
  the base isn't re-uploaded.
- Brace-balance + scoped-diff audit before every handoff. Unit-test pure functions with
  isolated g++ where possible. Probe-first for new parsing/protocol logic.
- Cosmetic iteration is fine ("dessert"), but the Phase 0 harness is the "vegetables"
  that make the next big refactor safe - don't let polish indefinitely defer it.

## Platform gating
- **Gate `#ifdef _WIN32` per-case with the reason on each, never per-region -
  and when keys die on one platform, probe the terminal before blaming it.**
  Slice 1 wrapped a REGION of handleInput's key switch for Ctrl+D (Discord);
  the span silently deleted six OTHER handlers (^B/^F/^G/^U/^Y/^R) from the
  Linux build - the symptom (some control keys dead, others fine) looked
  exactly like tty line-discipline theft, and the first "fix" (clearing
  ISIG/IEXTEN) was about to remove ^C on no evidence. A 30-line minimal-curses
  probe with the app's exact init settled it in one run: every code reached
  getch(), so the bug had to be app-side. Rules: (1) each gated `case` gets
  its own `#ifdef` + a comment naming WHY (which slice un-gates it); (2) a
  dead-key report on Linux = run the key probe FIRST - the terminal is
  innocent until getch() says otherwise; (3) grep `#ifdef _WIN32` spans in
  dispatch switches when porting - a gate around one feature's case swallows
  its neighbors invisibly (the build stays green; the keys just vanish).
- **The same class, one level deeper: whole-FUNCTION-body gates outlive the
  era that justified them - when a slice makes a subsystem portable, sweep for
  the scaffolding gates that assumed it wasn't.** Slice 1 gated the entire
  bodies of updateScrobbler / startLastfmPoll / startListenBrainzValidate
  (correct then: MD5 was a Linux placeholder, scrobbling couldn't work);
  slice 2 made the whole chain portable but the gates stayed - so the login
  prompts worked (key fix) while the ENGINES were empty shells: no scrobbles,
  auth polls never committed, token validation never started. Windows fine,
  probe fine (transport innocent), app silent. Tell-tale: the Linux build's
  "defined but not used" warnings on normTrackId/looksLikeRealTrack/
  deinvertArtist were this bug announcing itself - a static helper warning
  means its consumer is gated out; treat those warnings as porting TODOs,
  not noise. End-of-slice ritual now: `grep -n "#ifdef _WIN32" | audit each
  span` over files the slice made portable.
- **Third instance (slice-5 follow-up, Ctrl+A/Ctrl+K): a per-case gate whose
  comment can't name an unported dependency is a missed un-gate, not a
  deferral - audit EVERY gated case's reason, not just the reported one.** The
  key switch documents why ^D/^F/^Y/^R are gated (Discord readiness / MBSearch
  overlay still Windows-gated / CD slice 6), but ^A (deep iHeart log) and ^K
  (digital-vs-raw stream) sat under bare `#ifdef _WIN32` with NO reason - both
  depend on nothing unported (IHeartDeepLog portable since slice 1; both stream
  paths since slices 2/3), so they were simply forgotten un-gates: dead on Linux
  (no toggle, no toast), reported by Dos closing slice 5. Rule: when un-gating
  per-reason, sweep the neighbours - a gated `case` with no nameable unported
  dep is a bug. (Toast-only variant: ^T/^N run their function but their
  `showTrackToast` is still `#ifdef _WIN32` - the toggle works, the toast is
  silent on Linux; same class, cosmetic.)

## Raw ICY transport (Phase 3 slice 3)
- **Disable ALPN before speaking hand-written HTTP/1.x over a curl
  CONNECT_ONLY connection** (`CURLOPT_SSL_ENABLE_ALPN=0`). With ALPN on,
  curl's TLS handshake offers h2; a CDN edge (cloudflare) accepts it, and the
  hand-written HTTP/1.x request is then protocol garbage - the server just
  closes. Plain Icecast/SHOUTcast hosts don't negotiate h2, so the recipe
  works everywhere EXCEPT behind a CDN - exactly the shape a quick test
  misses. Found by the five-shape station probe before a line of product code
  (probe-first paying again).
- **WinINet never surfaces `icy-metaint` from an "ICY 200 OK" status line** -
  on a true SHOUTcast v1 server the Windows baseline gets `icy_metaint=0`:
  audio plays (the decoder's ADTS/MP3 resync swallows the interleaved
  metadata bleed) but StreamTitle never parses. Found by running the new
  fixture test against the UNTOUCHED baseline (the baseline-first pattern
  doing its job - the fixture spoke honest ICY and Windows failed the title
  asserts). The behavior is pinned per-platform in icy_pipeline_test;
  Windows stays byte-verbatim, the Linux twin parses ICY status itself (a
  named accepted-better delta, same class as its 75 ms vs 8 s stop latency).
- **The offset-0 invariant is the whole correctness story of a hand-rolled
  ICY client:** the de-interleave arithmetic is transport-blind, so alignment
  reduces to "the first byte served is body offset 0" - any bytes read past
  the header terminator MUST be preserved as the stream head (they arrive
  with the headers on most stations: 1078–1931 leftover bytes observed live).
  WinINet hid this by consuming exactly the headers.
- **Script a TUI's FULL prompt flow before reading a gate as failed.** The
  first live gate showed [LIVE] but a silent sink - three diagnostic loops
  (env, pulse, backend theories) before spotting that ^U asks for URL *then a
  station-name prompt*, and the script never sent the second Enter: the
  stream had simply not started during the capture window. A tmux gate is
  driving the app blind - capture the pane after EVERY keystroke batch when
  building the script, not just at the end.

## IPC transport (Phase 3 slice 4)
- **`MSG_NOSIGNAL` on every Unix-socket send is load-bearing parity, not an
  option flag.** WriteFile to a broken pipe returns FALSE; plain send() to a
  dead peer raises SIGPIPE and KILLS the process. The flag turns peer-death
  into EPIPE → `false` - the contract's failure mode. ipc_echo_test S6 pins
  it: a miss is a test-process death, not an assert failure. Same class:
  EINTR must be retried in send/recv (ncurses' SIGWINCH interrupts a blocked
  read on a resize) - signals are a failure mode the Windows twin cannot
  have, so surfacing them as "broken channel" would be a behavior invention.
- **"Byte-verbatim" includes LINE LAYOUT once a preprocessed-TU diff is the
  standard.** Un-gating `&& !config_.discord_presence` and re-flowing the
  3-line if-condition onto 2 lines produced a 1-line TU delta with an
  identical token stream - cmp caught it, the fix was keeping the baseline's
  own line breaks. When removing `#ifdef`s around live code, delete ONLY the
  directive lines; never re-wrap the surviving code.
- **A `nohup`'d background process does NOT survive its `wsl.exe` invocation
  ending; a tmux session does.** The first bridge attempt (nohup socat) was
  dead before the next command ran - silently, with an empty log. Any fixture
  that must outlive one `wsl -e bash` call (socat bridges, servers, the TUI
  under test) goes in `tmux new-session -d`; the tmux server keeps the WSL VM
  and the process alive across invocations.
- **The npiperelay + socat bridge is a REAL live-Discord venue for WSL2** -
  Windows Discord's \\.\pipe\discord-ipc-0 exposed as a genuine Unix socket:
  `tmux new-session -d -s bridge "socat -v 'UNIX-LISTEN:/tmp/discord-ipc-0,
  fork' EXEC:'npiperelay.exe -ep -s //./pipe/discord-ipc-0' 2>log"`. Bonus:
  `socat -v`'s wire capture IS the gate evidence - Discord's READY carries
  the logged-in user, and its SET_ACTIVITY responses echo the ACCEPTED
  activity (asset keys resolved to CDN ids, external art proxied to
  `mp:external/...`), so "RP shows title/artist + art" is provable at the
  byte level without eyes on the Discord window. Honest limit: socat creates
  the socket, so the flatpak/snap discovery candidates stay fixture-proven.

## CD transport / SG_IO (Phase 3 slice 6)
- **A virtualized CD drive is NOT the physical drive - it has a different model
  string, hence a different (usually default +0) AccurateRip offset, hence
  different corrected bytes.** VMware Workstation Pro presents its own
  `NECVMWar VMware SATA CD00`, not the passed-through `HL-DT-ST DVDRW GHD3N`, so a
  Linux rip there resolved +0 (not the GHD3N's +6), came out 6 samples misaligned,
  matched no AR entry, and ran sub-1x (virtual-drive emulation). This is the
  `model()`→offset load-bearing risk made concrete: the offset lookup is
  consumer-side and keyed on the INQUIRY string, so a different drive silently
  shifts the sample window and reads like a read bug. The byte-identical CD gate
  REQUIRES the exact physical drive whose offset the DB expects - for us that is
  the GHD3N via **usbipd→WSL2**, never a hypervisor's virtual CD. (The tell it was
  NOT a transport bug: the VMware raw reads matched Windows once offset-aligned -
  `raw[+6]` = Windows corrected sample 0.)
- **Prove a byte-identical transport port WITHOUT the full app: compile the
  production impl standalone and `cmp` it against a trusted reference tool.** For
  the SG_IO CD seam, `g++ -I include a_tiny_main.cpp src/platform/linux/CdIoSgIo.cpp`
  links the real `core::cdio()` (it's `#ifdef __linux__`, self-contained), so a
  10-line `main` calls the actual `model()`/`readToc()`/`readRaw()` against
  `/dev/sr0`. Then `cmp` `readRaw`'s bytes against `sg_raw -r 2352 /dev/sr0 be 04
  00 00 00 b6 00 00 01 10 00 00` (the identical READ CD 0xBE CDB): IDENTICAL proves
  the transport returns exactly what the drive's READ CD returns - no full remoct
  build (curl/fdk/taglib/…) needed. Reconcile display formats against the app's own
  dump code before crying "mismatch": remoct packs samples as `(R<<16)|L`
  (CDRipper.cpp), so a naive `L,R` printf looks word-swapped though the bytes are
  identical.
- **usbipd attach needs a RUNNING WSL2 distro** ("There is no WSL 2 distribution
  running") - start a background keep-alive (`wsl -d Debian -e bash -lc "sleep N"`)
  first, then `"C:\Program Files\usbipd-win\usbipd.exe" attach --wsl --busid 4-1`.
  "Device busy (exported)" = something on the Windows side (or another hypervisor)
  holds the drive - free it there first. WSL default user is `dostrom` (uid 1000,
  in the `cdrom` group → `/dev/sr0` reads need no sudo; `/home/dostrom` is writable,
  `/root` is NOT). Detach with `usbipd detach --busid 4-1` when done (the ritual).

## WSL build/test discipline - one run, one read, the log is the only truth
- Run every build/test FOREGROUND, redirected to a log, with an exit marker:
  `<cmd> > /root/out.log 2>&1; echo EXIT=$?`. Then read the log ONCE:
  tail + `grep -E "error:|Failed|Passed|EXIT="`.
- **Never background a build and poll ps/process state to infer whether it
  passed.** Process-alive tells you nothing about pass/fail - a finished-clean
  build and a still-running build look identical from ps, and you will loop
  forever restarting a build that already succeeded.
- **Never restart a build "to check on it."** If the log has no `EXIT=` yet, it
  is still running - wait, don't relaunch. Relaunching resets the clock and
  hides the result.
- A cold full `cmake -S . -B` configure + build is legitimately slower
  (3–5 min) than an incremental (`ninja: no work to do`, seconds) - slowness is
  not failure. Read the log to distinguish; don't assume a stall and restart.
- The answer to "is it done? did it pass? why did it fail?" is always in the
  log file, never in the process table. If you find yourself checking ps or
  relaunching, stop - go read the log. (Cost three loops in the slice-2
  session before being pinned.)
- **`/tmp` is a per-invocation tmpfs, wiped between separate `wsl.exe` calls;
  `/root` persists.** Each `wsl.exe -d Debian …` launch that lets the distro
  idle out clears `/tmp` - a harness / fake-binary / capture file written there
  in one call is GONE in the next - while `/root` (ext4) survives (that's why
  the synced source tree and build dir persist across calls but a `/tmp`
  scratch file doesn't). Put anything that must outlive a single call under
  `/root`, or do the whole write-run-read in ONE `bash < script.sh` invocation.
  (Cost one loop in slice 5 chasing an "empty" argv capture a prior call had
  actually written to a since-wiped `/tmp`.)

## Readouts / estimators
- **The "live VBR" bitrate estimator was a linear position model - a
  constant-derivative average dressed as a live reading; TagLib's nominal is
  both simpler and more accurate.** `(pos/dur)·file_size` differentiated can
  only ever yield file_size/duration in steady state: no VBR signal, just the
  average plus artifacts (tag-byte inflation, a seek-window spike, timing
  jitter). Before "fixing" or "labeling" a live readout, check whether the
  estimator can measure what it claims at all - this one couldn't, which
  turned a labeling question into a deletion.

## Audio-thread refactoring (Phase 2 slice B)
- **A pointer needs no stronger publication guarantee than the struct it
  replaces - but the FREE side is where a naive pointer swap regresses.** The
  release/acquire flag that published a multi-hundred-byte struct publishes a
  pointer + pointee identically (happens-before is transitive over the deref).
  The genuinely new hazard was deallocation: baseline's UI-thread positionSec()
  racing the audio-thread swap read a torn MEMBER struct (never freed memory);
  a naive `unique_ptr` move-assign would have FREED the object under that read.
  Fix: the audio thread RETIRES the outgoing source (retired_src_), the main
  thread reaps it in pollEvents under the existing track_swap_flag_
  synchronizes-with edge. Raced deref now always hits a live object, and the
  audio callback stops calling free entirely. Rule: when changing a payload's
  ownership mechanism, audit the READERS of the old payload for lifetime
  assumptions, not just the writers for ordering.
- **The by-value ma_decoder copy worked only because miniaudio's internal
  pointers are heap-targets, not self-references** - an undocumented
  relocatability property the baseline silently relied on. A fixed heap address
  for the decoder's whole life (the source object) removes that reliance.
- **Slice-B accepted residuals (inert, documented so a future differ isn't
  surprised):** (1) the outgoing decoder's uninit/file-close now happens ~one
  pollEvents tick later on the main thread (was: inside the audio callback);
  (2) on a read that returns a FULL buffer and EOF together, track_done fires
  ≤1 callback (~10 ms) later (the readFrames wrapper signals EOF by short read
  only) - same audio out, flag shifted one tick; same shape in varispeed's
  top-up loop (one extra 0-frame call before eof); (3) the TagLib hint
  pre-reads in play/preloadNext were DROPPED, not moved - open_decoder
  provably discarded them (`(void)hint_channels`) since the forced-44100/2
  config replaced hint sniffing (the slice-8 "don't migrate garbage" rule);
  (4) setDevice's file-restart now primes the decoder and re-reads tags into
  the source's private info_ (current_track_ untouched) - inert warm-up.
- **Varispeed is linear interpolation BY CHOICE, not by laziness.** The playback-
  speed resampler uses linear interp deliberately - it's cheap, zero-latency, and
  the slight top-end softening is the right feel for a scrub/speed control (you're
  changing pitch anyway). What keeps it CLICK-FREE is discipline, not filter order:
  RESET the interpolator residual (the fractional read position / last-sample carry)
  on every seek, track-swap, and stop, so a fresh stream never interpolates against
  a stale tail. Don't "upgrade" to sinc/polyphase without a concrete reason - it buys
  inaudible quality here at real latency + complexity cost, and a sinc kernel that
  forgets to flush its history on the same three events reintroduces exactly the
  clicks the reset already prevents. If varispeed ever clicks, look at residual reset
  on those boundaries FIRST, not at the interpolation math.

## Replay-net / test-fixture lessons (Phase 2 slice 0)
- **"A few quiet reads" is NOT end-of-stream when legitimate in-stream silence
  exists.** With a fake-speed producer, the reader finishes writing a whole track
  (including the transient-error silence-fill gap) into the ring long before a
  paced consumer drains it - a drain loop that exits on "N consecutive all-zero
  reads + reader stopped" terminates mid-gap and reads as data loss. Proof of
  emptiness must be structural: require a consecutive zero-run LONGER THAN THE
  ENTIRE RING - the ring cannot hold that much, so it must be dry. (First
  cd_pipeline_test run failed exactly this way.)
- **Windows `Sleep(1)` is ~15 ms** (default timer granularity). Two consequences
  for producer/consumer tests: pacing margins are far larger than they look on
  paper (good), and pacing where none is needed dominates runtime (bad - pace the
  consumer only while the producer is live; once it has stopped, the ring is a
  static buffer and can be drained flat out. Cut cd_pipeline_test from 25 s to 7 s).

## HTTP / platform-seam migration
- **Parity for an HTTP migration lives at the call sites, not the seam.** The FakeHttp
  unit test feeds canned bodies, so it proves parse logic but CANNOT catch a wrong
  request field - body cap, timeout, user-agent, status-gating, or redirect policy. Those
  must be eyeball-confirmed against the pre-migration baseline, per site, by hand.
- **Verify repo structure with `ls`/`dir` before asserting file placement.** A stated
  layout is a claim, not ground truth. Phase 1 slice 1 nearly committed `IHttp.h` /
  `HttpWinInet.cpp` at the repo root on a false "flat layout" premise; a directory
  listing caught that the tree already had `src/`+`include/`. Check, don't assume.
- **`INTERNET_FLAG_SECURE` derived from the URL scheme is a no-op for all-HTTPS sites
  but a real behavior change for a plain-HTTP one.** Harmless through group (a) (MBLookup
  + RadioBrowser are HTTPS); flag it when CDRipper's AR/CTDB fetch (plain `http://`
  accuraterip.com / cuetools.net) migrates in group (c).
- **HTTP request bodies go over the wire as raw UTF-8 bytes - never widen them.**
  `HttpSendRequest`'s payload is a byte buffer and `nlohmann::dump()` emits UTF-8; widening
  a POST body to `wchar_t` corrupts non-ASCII payloads (e.g. "Björk"). A FakeHttp test proves
  the consumer *builds* the body right, but only a live submission viewed on the service
  proves *wire* fidelity - do both (group (b): Björk/Jóga verified on Last.fm + ListenBrainz,
  accents intact).
- **The two group-(b) GETs inherit the seam's GET-path SEND timeout** their per-call
  baseline never set - inert on a bodyless GET, documented in-code, and accepted rather than
  adding a per-request timeout field (which the interface deliberately doesn't have). Parity
  is matched site-by-site; where a single seam policy can't match every site, pick the one
  that keeps shipped sites byte-identical and document the inert residual.
- **`reject_truncated` must CLEAR the body, not merely fail the response.** A capped read
  can leave a valid-looking leading fragment (image magic bytes, a JSON prefix); returning it
  with only `ok=false` invites a consumer that checks the body to embed a partial cover.
  `finalizeBody()` clears it (CoverArt's 10 MB reject/clear). Groups (a)/(b) pass
  `reject_truncated=false` (cap-and-keep, e.g. MBLookup) and are unaffected.
- **A redirect bool can't express "follow same-scheme only."** CAA `/front-500` *requires*
  following its redirect to the image host, but must not be downgraded http<->https - so
  neither `follow=false` nor default-follow matches baseline. Hence `RedirectPolicy`
  {FollowAll, FollowNone, FollowSameScheme}; FollowAll maps byte-identically to the old bool.
  (c) is where the interface legitimately grew - matching baseline, not gold-plating.
- **Plain-HTTP scheme-derived no-SECURE came due for CDRipper exactly as flagged in (b).**
  `urlIsSecureScheme()` is false for `http://accuraterip.com` / `http://db.cuetools.net`, so
  no `INTERNET_FLAG_SECURE` - correct, and the AR fetch verified byte-identical (Joan Osborne
  12/12 conf 200). The HTTPS (a)/(b) sites were unaffected - a no-op there, real change here.
- **"Keep the partial body on a mid-read error" was NOT a universal baseline.** All six
  one-shot sites used `while (InternetReadFile && got>0)` (error ⇒ keep partial, report ok),
  but `hlsHttpGet`'s baseline FAILED the call on a mid-body read error - a partial segment
  must not reach the decoder path. Found at implementation time, not in the survey; hence
  `HttpResponse::read_error` (slice 4): ok/body semantics unchanged for everyone, hls gates
  on the flag. When surveying a migration, diff the read *loops*, not just the flags.
- **Slice-4 accepted residuals (inert, documented so a future baseline-differ isn't
  surprised):** (1) on an HTTP ≥400 the seam reads the (small) error body before the
  consumer gates, where `hlsHttpGet`'s baseline bailed pre-read - no functional effect;
  (2) the seam reads in 4 KB chunks vs the audio sites' 8 KB - wire-invisible (TCP
  buffering), cancel granularity slightly finer; (3) scheme-derived `INTERNET_FLAG_SECURE`
  replaces hls's flag-less TLS-by-scheme and IHeart's hardcoded SECURE - identical
  outcomes, every URL's scheme accounted for. Cosmetic: the migrated failure logs lose
  their `GetLastError()` codes (the seam doesn't surface OS error codes), same spirit as
  group (c)'s collapsed CTDB log lines.
- **A keep-alive session's pooled connection outlives its fetches - localhost test
  fixtures must account for it.** The `http_cancel_test` server threads blocked in
  send/recv on the client's pooled connection, which only closes when the SESSION object
  is destroyed; joining the server thread before resetting the session deadlocked until
  WinINet's ~60s idle-close (110s test run). Fix: reset the session BEFORE joining fixture
  threads, and cap accepted-socket waits with SO_RCVTIMEO/SO_SNDTIMEO (2.6s run).
- **`close()` on a listening socket does NOT wake a thread blocked in `accept()`
  on Linux - `shutdown()` first.** Windows' closesocket aborts a blocked accept
  (WSAEINTR), and http_cancel_test's fixture relied on exactly that to stop its
  server threads; ported to Linux unchanged, scenarios B/C hung forever in
  server.join() (7/10 tests in, stuck minutes on a 2.5 s test - the log made the
  hang unambiguous). POSIX close() just drops the fd refcount; the sleeping
  accept keeps its reference. `shutdown(fd, SHUT_RDWR)` wakes it (accept returns
  EINVAL) - hence the shim's stopListener(): shutdown-then-close on POSIX,
  plain closesocket on Windows. Belt-and-braces: the test now carries a ctest
  TIMEOUT 60 so this hang class fails the matrix fast instead of eating the
  25-minute default. (Phase 3 slice 2, first Linux run of the portable test.)
- **A fake's observation state must outlive the object the consumer destroys.** The
  consumer discarding its channel/session on failure is often exactly the behavior under
  test (DiscordRP drops its channel on a CLOSE reject) - a fake that stores its captured
  data ON the discarded object hands the test a dangling pointer at the precise moment
  it asserts. `discord_ipc_test` first shipped this bug; fix: channel state lives in
  `shared_ptr`s owned by the Fake FACTORY, channels hold a reference. Twin of the
  http_cancel fixture lesson above - observation belongs to the fixture, not the wire.
- **One consumer with one obvious injection point ⇒ constructor injection, not another
  transitional global.** `core::http()`/`setHttp()` earned their existence: eight
  consumers scattered across seven modules. IIpc has ONE consumer (DiscordRP), so it
  takes `IIpc*` in its ctor (tests inject there; no `setIpc()` exists) and `core::ipc()`
  survives only as the link-time bridge to the platform TU - core code can't #include
  the impl header, so the production default must be reached by name. Pick the pattern
  per seam by consumer count, not by precedent.
- **git rename detection dies if you re-indent a moved body into a class.** Similarity
  is content-based; wrapping an extracted baseline in a class method (+4 columns on
  every line) plus a new header comment dropped Toast.cpp → NotifyWinToast.cpp below
  the 50% threshold - the `git mv` registered as delete+add, severing the
  injection-fix history the move was supposed to preserve. Fix: keep the extracted
  baseline at namespace/file scope at its ORIGINAL indentation (a file-static
  function) and make the interface class a thin adapter that calls it - held the
  slice-7 rename at 64%. Bonus: the frozen block's diff then shows exactly one
  changed identifier, which is the auditable-parity property we want anyway.
- **Probe the baseline call before migrating it - a "working" fire-and-forget ioctl
  may never have worked.** CDSource's "reset speed to max" sent a hand-rolled 6-byte
  struct where cdrom.sys expects the canonical (12-byte here) CDROM_SET_SPEED; a
  standalone probe showed it rejected with ERROR_BAD_LENGTH - a lifelong silent no-op
  (return ignored). A seam can't express "send these exact malformed bytes", and
  silently fixing it would CHANGE drive behavior on the playback path. Rule: probe →
  if it provably fails today, DROP the call (behavior-identical) and park the fix as
  its own decided improvement. Don't migrate garbage faithfully, and don't fix it as
  a ride-along. (slice 8; parked item in roadmap.md)
- **"Buffer size implies capability" is a platform shape - name capabilities as
  explicit flags in a seam.** Windows IOCTL_CDROM_RAW_READ returns C2 error bytes iff
  the output buffer is sized 2646/sector - the request is IMPLICIT in the buffer.
  SG_IO needs the C2 error-field bits set in the CDB; buffer size can't express it.
  Hence `readRaw(..., want_c2, ...)`: advisory on Windows (impl passes the buffer
  through untouched - byte-identical), load-bearing on Linux. Corollary: surface
  bytes-returned - the baseline's C2 probe and de-interleave branch both key on it;
  hiding it would force policy into the seam.
- **One impl can serve two consumers' differing open flags only after arguing each
  delta inert - and the argument goes in the diff.** CDSource opened its device
  SHARE_READ + SEQUENTIAL_SCAN; CDRipper SHARE_READ|WRITE + ATTRIBUTE_NORMAL. The
  seam uses the ripper's shape for both: share-mode widening is strictly more
  permissive (nothing opens a CD device for write), and SEQUENTIAL_SCAN is a
  ReadFile read-ahead hint - inert under DeviceIoControl-only access. Both residuals
  proven by the byte-identical rip gate + live playback. The concurrent-two-opens
  case (CDSource holds while CDRipper rips) is interface CONTRACT, not accident.
- **Preserve asymmetric timeout shapes; don't "clean them up" into a uniform API.**
  DiscordRP's baseline bounds the header wait (peek ≥8 bytes, 1000 ms - a wedged Discord
  must never hang the UI) but deliberately does NOT bound body reads. A tidier
  `recvExact(len, timeout)` seam would have either invented a body timeout (behavior
  change) or hidden the baseline's loop inside the impl (unauditable parity). The
  3-primitive channel (send/waitReadable/recvSome) keeps the consumer's loops verbatim -
  the read-loop twin of "diff the read loops, not just the flags."

## Plugin C-ABI (Phase 4)
- **Loading a shared library (`LoadLibrary`/`dlopen`) is a platform call - it's a
  SEAM, not an `#ifdef`.** The plugin loader became the 5th platform seam
  (`core::IPluginLoader` + Win/Posix impls), exactly like http/ipc/notify/cd_io:
  the raw OS load lives in `src/platform/*`, and ALL policy (resolve the entry, the
  ABI version gate, descriptor validation) stays consumer-side in `PluginHost` - the
  same transport/protocol split every seam holds. If `core/` compiles on Linux,
  nothing leaked.
- **`void*` ↔ function pointer: convert with `memcpy`, never a cast.** `dlsym`/
  `GetProcAddress` hand back an object pointer; casting it to a function-pointer type
  is conditionally-supported and trips `-Wpedantic` (which the project builds with).
  `std::memcpy(&fn, &sym, sizeof fn)` is the warning-free, standard-blessed bridge -
  the same discipline as refusing UB-that-happens-to-work elsewhere.
- **Prove the ABI against a DISPOSABLE consumer before moving real code.** Slice (a)
  froze the whole v1 C header + loader against a throwaway pure-C sine plugin; slice
  (c) then moves StreamSource against a FROZEN contract instead of debugging a
  code-move and an evolving ABI at once. The pure-C plugin also PROVES the header is
  genuinely C-consumable (no C++ leaked across the line). The reject-path tests
  (null / wrong-ABI / too-small / missing-fn) are the important half - proving the
  version gate REFUSES bad plugins is what makes the versioning contract real.
- **`std::atomic_ref<T>` and `std::atomic<T>` must not both touch the same object.**
  Directing "make `stop_` a `std::atomic<int32_t>` and have the transport read it via
  `std::atomic_ref<int32_t>`" is UB: [atomics.ref.generic] requires an object be
  accessed EXCLUSIVELY through `atomic_ref` while any `atomic_ref` references it, so
  mixing `std::atomic` member ops (the writer) with `atomic_ref` (the reader) on the
  same bytes is non-conforming (it happens to work on GCC - which is exactly the trap).
  The conforming realization of "unify the HTTP cancel token on the ABI's `int32_t`":
  leave `stop_` a `std::atomic<bool>` (zero churn on its 22 sites), add a PLAIN
  `int32_t http_cancel_` accessed via `atomic_ref` on BOTH sides, written only through
  a `setStop()` helper at `stop_`'s two write sites so it can never drift. Same
  int32-end-to-end result, standard-correct, minimal. "UB-that-happens-to-work isn't
  what we build on" - the twin of the `memcpy` fn-ptr rule above.
- **Assigning a C++ static to a C ABI function pointer is fine - the codebase already
  does it.** The RemoctPlugin table's members are C-linkage function pointers; the
  StreamSource adapter's callbacks are internal-linkage C++ statics. Assigning them is
  the SAME pattern as `ma_device_config.dataCallback = &AudioManager::maDataCallback`
  (miniaudio's C callback). No `extern "C"` wrapper needed (and `extern "C"` inside an
  anonymous namespace is a linkage contradiction to avoid).
- **An in-process plugin makes the (b)→(c) boundary a single line.** Slice (b) drives
  the streaming source through the C ABI while it is still compiled IN, reached by a
  direct `remoct_stream_plugin_query()` call. Slice (c) flips ONLY that acquisition to
  the `.so`'s exported `remoct_plugin_query` via `loadPlugin()` - the host-driving code
  (`PluginSource`) is byte-identical across the flip. Host-plumbing (b) and the binary
  extraction (c) never debug at the same time.
- **What's host state doesn't belong in the ABI.** The plugin table has no `url()` or
  `get_paused()` getter: the host opened the URL and set the pause flag, so
  `PluginSource` tracks both itself. Surveying the real consumer surface (10 call
  sites) is what revealed these two were host bookkeeping, keeping the contract minimal.
- **Extracting a subsystem to a .so, survey the host's INBOUND calls too, not just the
  subsystem's outbound refs (Phase 4 slice c).** The `core::http()` survey (the moving
  files' OUTBOUND host refs) was exhaustive and correct - but it can't see a HOST module
  that reaches INTO the moving code: `UIManager`'s Ctrl+A called `IHeartDeepLog::toggle()`/
  `path()` directly, a host->plugin call invisible in the plugin's own diff. Caught by
  grepping the host for `#include` of the moving headers (`grep -rn '#include
  "IHeartDeepLog.h"' src/`), not by the outbound survey. Rule: when a file moves behind a
  binary boundary, grep BOTH directions - what it calls out (rewire to injected services)
  AND who calls in (must cross the ABI). The fix reused the ABI's `set_config` (host tracks
  the toggle state, pushes `("deeplog","1")`), exactly the sibling Ctrl+K handler's shape -
  no new mechanism. (The two-layer discipline earned its keep: the static survey missed it,
  the include-grep/build caught it.)
- **A moved plugin links the raw transport it PRIVATELY owns - that's not "HTTP in the
  plugin" (slice c, the single most load-bearing call).** "The plugin must not carry its own
  HTTP" is true only for the `core::IHttp` SEAM (`HttpWinInet`/`HttpCurl` stay host-side,
  reached via the injected service table). StreamSource's sacred ICY read loop is PRIVATE
  raw WinINet / raw `curl_easy_recv` - not `core::http()` - so it moves with the plugin
  byte-verbatim and the `.so` links `-lwininet` / `CURL::libcurl` for it. Two HTTP consumers
  in one plugin; only the seam one crosses the table. Reading "no HTTP in the plugin"
  literally would have meant rewriting the sacred loop or a link failure.
- **Plugin-side HTTP is the host shim inverted.** `PluginHostServices` fills a
  `RemoctHostServices` table FROM `core::IHttp` (host side); the plugin needs the MIRROR -
  `plugin::HostServiceHttp : core::IHttp` OVER the injected C table - so the moved code's
  `->openSession(cfg)`/`fetch(req)` is unchanged and the cancel `const int32_t*` crosses
  verbatim (unified in slice b). The rewire is then exactly the two `core::http()` call
  sites; audit `grep core::http() plugins/stream/ = 0`, no default arg `= core::http()` (it
  would leave the symbol in a plugin header and fail link + audit).
- **Two miniaudio/Log implementations in one process are safe under `RTLD_LOCAL`.** The host
  compiles `MINIAUDIO_IMPLEMENTATION` (device + decode); the plugin needs its own decode-only
  copy (`MA_NO_DEVICE_IO` - it never opens a device, the host owns it). Separate binaries +
  `dlopen(RTLD_LOCAL)` (Posix) / per-module symbol space (Windows) keep the plugin's symbols
  private - no collision. Same reasoning as linking `Log.cpp` into both: `fopen(...,"a")`
  per-write is atomic-append so the host and plugin instances interleave safely. **Accepted
  residual:** the plugin's `Log` has its own `g_enabled`, so a host log-disable doesn't gate
  the plugin's `[stream]` line (documented, not worth an ABI knob). Also: the Ctrl+A "Deep
  log: ON" toast dropped the capture path (`IHeartDeepLog::path()` is now plugin-side, and it
  was lazily-created/racy - empty right after enable - so nothing reliable was lost).
- **Prove "loaded, not compiled-in" with a negative control.** The loaded-`.so`/`.dll` ABI
  round-trip (`core::loadPlugin` → create → caps → set_config → destroy) shows it loads; but
  the airtight proof that PLAYBACK runs through the module is to remove the `.so` and confirm
  the sink goes silent (RMS 0.0) while the same drive sequence runs. The host binary carries
  no StreamSource symbol (removed from its sources), so streaming is impossible without the
  module - the negative control makes that visible.
- **Byte-identity-testing a live source across a binary boundary (Phase 4 slice d).** To prove
  a loaded plugin is *identical* to compiled-in - not just "sounds right" - assert byte-exact
  PCM between two runs of the SAME deterministic input, one via the compiled-in descriptor
  (keep an in-process `*_query()` accessor for exactly this) and one via `loadPlugin()`. Four
  disciplines make it trustworthy instead of flaky:
  1. **Feed the input through the REAL crossing, both runs.** Inject the fake transport AS THE
     HOST SERVICE (`core::HostServices(fake).table()`), not into the plugin - the plugin reaches
     HTTP only through the injected table (audit: `grep core::http() plugins/stream/ = 0`), so
     there's no bypass to accidentally take, and byte-identical PCM then proves the ABI
     marshalling (request/response translate + host-malloc/free) is transparent. Both runs cross
     the same table → the only variable is compiled-in symbol vs `dlopen` (a controlled
     experiment). A `segment_gets>0` counter makes the crossing observable.
  2. **Make determinism a TESTED assertion, not a hope.** Run the compiled-in descriptor TWICE
     and assert `refA==refB` *before* comparing to the loaded run. If the drain protocol ever
     flakes, that assert fails FIRST - a flake can never masquerade as a `.so` fidelity bug.
  3. **Keep the capture path flag-independent.** Two separately-compiled targets (test vs `.so`)
     can differ in float optimization. Pick a fixture that stays in fixed-point/integer math:
     a 44100 source bypasses the resampler (the one float-sensitive stage), the decoder is
     fixed-point (FDK-AAC), the ring is `int16`, and `int16→float` is `x/32768` (exact). Then
     `-O`/`-ffast-math` can't move the bytes. **Name what you thereby DON'T test** - here the
     resample path's byte-identity (a 48k fixture would be flag-sensitive) - and cover it
     behaviorally instead; a scoped honest proof beats a broad flaky one.
  4. **Remove thread-timing from the capture.** A live source silence-pads on underrun, so a
     naive drain captures timing-dependent silence. Capture a FIXED head window after prebuffer
     with `DRAIN < PREBUFFER`: the ring can't empty during the drain → no silence-fill → the
     head is fixed decoded audio whatever the producer thread does (the slice-0 "static buffer,
     drain flat out" lesson, applied to byte-identity).

## Windows static single-exe (Option C - Phase 0 gate)
Probe on `experimental/win-pdcurses`, MSYS2 UCRT64, against the CURRENT ncursesw (PDCurses
not yet swapped - this deliberately isolates the codec question from the curses question).

- **Static archives exist for everything except libebur128.** UCRT64 ships `libFLAC.a`,
  `libFLAC++.a`, `libmp3lame.a`, `libfdk-aac.a`, `libtag.a` (+`libtag_c.a`), `libogg.a`,
  `libncursesw.a`, `libpanelw.a`, plus the GCC runtime trio (`libstdc++.a`, `libwinpthread.a`,
  `libgcc.a`). **libebur128 is DLL-only** - only a `libebur128.dll.a` import stub + the DLL, no
  `.a` anywhere in ucrt64. fdk-aac and TagLib (the plan's predicted problem children)
  static-link CLEAN; the lone holdout is ebur128 (ReplayGain / EBU R128).
- **`find_library` resolves to the `.dll.a` import stub by default**, so the stock build is
  fully dynamic even though the `.a` files sit right beside them. `-DCMAKE_FIND_LIBRARY_SUFFIXES=".a;.dll.a"`
  passed at configure time does NOT stick - MSYS2's Windows-GNU platform module resets that
  variable during `project()`. It must be set inside CMakeLists (after `project()`, before the
  `find_library` calls) to take effect.
- **Every dllimport-decorated lib needs its "I am static" define at COMPILE time**, else the
  objects reference `__imp_<sym>` that the static `.a` lacks (link fails: undefined reference to
  `` `__imp_...' ``). Compile-side fix, not link-side:
  - ncurses -> `NCURSES_STATIC` (already in the tree)
  - TagLib  -> `TAGLIB_STATIC`
  - FLAC    -> `FLAC__NO_DLL`
  The C codecs with no dllimport decoration (mp3lame, fdk-aac) needed nothing.
- **Static libFLAC needs libogg linked explicitly.** The dynamic build pulled ogg in
  transitively via the FLAC DLL; the static `libFLAC.a` exposes the dependency, so
  `${MSYS2_PREFIX}/lib/libogg.a` must be added to the link line.
- **Result:** `-static -static-libgcc -static-libstdc++` + the three defines + libogg ->
  `re-moct.exe` (10.9 MB) links, and `ldd` shows ONLY Windows system DLLs plus the one
  documented exception `libebur128.dll`. No `libgcc_s` / `libstdc++` / `libwinpthread` / codec /
  curses DLLs. **Phase 0 gate = PASS**, single-DLL exception. Gated behind a
  `-DREMOCT_STATIC_PROBE=ON` CMake option so the default dynamic build is untouched.
### Decisions locked post-probe (the shipping package is a COHERENT set, not one file)
- **fdk-aac stays DYNAMIC on purpose - it static-links clean, but we choose not to.** The exe
  AND the `remoct_stream` plugin both use fdk-aac. Static-linking it into the exe while the
  plugin links its own copy would put TWO independent fdk-aac states in one process (aliasing /
  dual-decoder-state -> the classic "works 99%, glitches inexplicably 1%" trap). Keeping one
  shared `libfdk-aac.dll` = one codec, one state, one copy in the address space. This DISSOLVES
  the plugin-codec double-linkage question rather than probing it. **Do NOT convert fdk-aac to
  static later - that reintroduces the exact dual-state bug this avoids.** Contrast: FLAC / LAME
  / TagLib are exe-only, so they static-link with no such risk.
- **ebur128 stays DYNAMIC by necessity** - no `.a` published in UCRT64 (ReplayGain / loudness).
- **Shipping shape:** `re-moct.exe` (static: PDCurses, GCC runtime, FLAC+LAME+TagLib+ogg) +
  `libfdk-aac.dll` (shared exe/plugin, by design) + `libebur128.dll` (holdout) + `plugins/`
  (dynamic, unchanged). Every DLL has a stated reason.
- **Graduation gate (corrected from "single DLL exception"):** the Phase-5 `ldd` / Dependencies
  check passes IFF the ONLY non-system DLLs are EXACTLY `libfdk-aac-2.dll` and `libebur128.dll`
  (note the fdk-aac soname carries a `-2` version suffix - the exe's `ldd` shows
  `libfdk-aac-2.dll`, not `libfdk-aac.dll`). Any third non-system DLL fails the gate. The CI
  dependents-check must assert that explicit two-name allowlist so nothing new sneaks in later
  and gets waved through as "expected."
- **Plugin drags the GCC-runtime DLLs (OPEN packaging question).** `remoct_stream.dll` builds
  dynamic (correct - it must stay a loadable module) and links the SHARED `libfdk-aac-2.dll`
  (good - one codec state), but it also pulls `libgcc_s_seh-1.dll` / `libstdc++-6.dll` /
  `libwinpthread-1.dll` that the STATIC exe does not carry. So the shipping package would need
  those 3 GCC-runtime DLLs too, unless the plugin target gets `-static-libgcc -static-libstdc++`
  (folds the C++ runtime into the plugin DLL; safe because the plugin boundary is a C ABI - no
  C++ objects cross it, proven byte-identical in Phase 4 slice d). That is NOT the same as
  `-static` on the plugin (which would break the loadable module). DECISION for Dos before Phase
  5: static-libgcc/stdc++ the plugin (keeps the 2-DLL package) vs ship the 3 runtime DLLs.
- **Static libwinpthread needs `--whole-archive`, not just `-static-libstdc++`.** libwinpthread
  is pulled by `libstdc++.a`'s pthread refs at the driver's IMPLICIT tail, and `-static-libstdc++`
  emits a `-Bdynamic` right after libstdc++, so a plain `-lwinpthread` (before the objects) is
  discarded and a trailing `-Bstatic` is undone -> the DLL stays dynamic. `-Wl,-Bstatic,--whole-archive
  -lwinpthread -Wl,--no-whole-archive,-Bdynamic` forces the whole archive in regardless of ref
  position. Verified by `ldd` showing no libwinpthread-1.dll.

## PDCursesMod wingui (Option C - the Windows GUI port)
The wingui port draws its OWN GDI window - it is NOT a console. Almost every failure here is
runtime-discoverable, not compile-discoverable; a green build proves nothing about the window.

- **The seam header must NOT be named `Curses.h`.** On Windows's case-insensitive filesystem,
  with `include/` ahead of the vendored dir on the search path, `#include <curses.h>` (from the
  seam's own PDCurses branch) matches the seam itself, and `#pragma once` then expands it to
  nothing -> every curses symbol undeclared. Named it `CursesSeam.h`. (Symptom: `WINDOW` undefined,
  "did you mean WINDOWPOS" - windows.h is all that's visible.)
- **wingui owns the font; the registry overrides a pre-initscr set.** The face is the extern
  global `PDC_font_name` (`wchar_t[128]` under the forced-UNICODE build; no public setter), set
  BEFORE initscr (wingui measures the font during screen open). BUT wingui restores the last
  font+size from `HKCU\SOFTWARE\PDCurses\<exeBaseName>` INSIDE initscr, which clobbers your set -
  a stale "Courier New" saved by a pre-fix run stays sticky forever. Clear that value first
  (`RegDeleteKeyValueW`) so your choice wins. Bundle a glyph-complete Nerd Font via
  `AddFontResourceExW(FR_PRIVATE)` from `<exeDir>/fonts/` so it need not be installed. Default
  "Courier New" lacks rounded box corners / sub-blocks / Nerd icons -> tofu.
- **Resize: use `resize_term(0,0)`, never the console APIs.** `GetConsoleWindow`/`CONOUT$` report
  nothing meaningful on a console-less GDI app and drive `resize_term` to a mismatched size (no
  reflow + crash from drawing into a stale cell buffer). wingui already stored the new window
  size and queued KEY_RESIZE; `resize_term(0,0)` adopts it and refreshes LINES/COLS/stdscr - the
  canonical PDCurses idiom (its own getch.c does the same).
- **Live repaint during a drag needs `PDC_set_window_resized_callback`.** Windows runs a MODAL
  message loop while the user drags the frame, so the app's getch()/draw loop is blocked and the
  window blanks until release. wingui invokes the registered callback from its WM_SIZE handler
  INSIDE that modal loop - repaint from there. Skip the intermediate blank-frame `doupdate()` in
  resizeWindows for wingui or every WM_SIZE flashes (wingui double-buffers WM_PAINT, so one clean
  composed frame per tick is flicker-free).
- **THE ordering trap: register that callback LAST.** Any window op that fires WM_SIZE
  SYNCHRONOUSLY (SetWindowPos with SWP_FRAMECHANGED, SetMenu, ...) re-enters the resize callback
  the instant it is registered - and in the CONSTRUCTOR that runs `resizeWindows()` (drawAll,
  createWindows) on a HALF-BUILT UIManager -> crash / garbage launch. Do ALL ctor-time window
  manipulation FIRST, then register `PDC_set_window_resized_callback` as the last wingui step.
  (Cost us a black launch that looked like a font/resize bug but was pure reentrancy.)
- **Hide the menu with wingui's OWN toggle, not raw SetMenu.** A raw `SetMenu(hwnd, NULL)` grows
  the client area and desyncs the curses grid; trying to fix that with a hand-computed
  `resize_term` mis-sized/relocated the window (title bar shoved off-screen, unresizable). wingui's
  `WM_TOGGLE_MENU` (`WM_USER+4`, sent via `WM_COMMAND`) is built for this: it removes the menu,
  KEEPS the curses grid unchanged, and shrinks the window to match - no desync, and because the
  grid doesn't change it can't re-enter the resize callback. `menu_shown` starts at 1 (we clear
  the registry each launch), so one toggle hides it.
- **DWM dark mode does not cover the menu bar.** `DwmSetWindowAttribute(hwnd,
  DWMWA_USE_IMMERSIVE_DARK_MODE=20, ...)` (19 on older Win10) darkens the title bar/border to
  match the OS (read `HKCU ...\Personalize\AppsUseLightTheme`, 0=dark) - but a standard Win32 menu
  bar stays light regardless. Dark-theming the menu bar itself needs owner-draw / undocumented
  uxtheme; hiding it (above) was the clean answer once the font moved to config.
- **Spectrum in a short strip: gamma-lift + a floor, and DFT bins are ~free.** A 5-row strip
  crushes dynamics that fill a 40-row pane, so in Awesome only lift values (`pow(val,0.65)`) and
  raise the floor so bars fill and do not collapse to a nub. Spread bars by interpolating each
  bar's column span (`x0..x1 = b*cols/n_bars`) so they fill the FULL width and integer rounding
  never drops one. More `VIZ_BINS` is nearly free: the DFT cost is dominated by the per-k
  frequency sweep (bins just partition the same k's). A low band whose log-spaced range collapses
  onto a single FFT bin (`k_lo==k_hi`) must use that k, not hit the above-Nyquist "empty" skip, or
  low bands drop out.

## Decode backends / Opus + WavPack (decode-opus-wv slice)
- **`.ogg` Vorbis is advertised-but-broken today.** `.ogg` sits in `AUDIO_EXTS`
  and `fileTypeTag`, but this build compiles NO Vorbis decoder: miniaudio gates
  its whole stb_vorbis backend on `STB_VORBIS_INCLUDE_STB_VORBIS_H`, which nothing
  in the tree defines (proven by compile probe, plan §1.2). Browsable, labelled,
  un-playable - exactly the trap `.opus` was in before this slice. Becomes the
  next slice; until then a red `.ogg` result is NOT a decode-slice regression.
- **Custom decoder backends register through `remoct_custom_backends()`
  (CustomBackends.{h,cpp}) - add codecs THERE, not at the call sites.** Playback
  (`open_decoder`) and BPM analysis (`detectBpm`) share the array; before the
  helper they were two hand-maintained literals that could drift.
- **TagLib ships its own `opusfile.h` and its include dir is on the target's
  path.** `#include <opusfile.h>` can resolve to TagLib's C++ tag reader instead
  of libopusfile's C codec API. Our code always writes `#include <opus/opusfile.h>`.
  The `-I.../include/opus` dir itself IS still required (opusfile.h includes its
  libopus siblings like `<opus_multistream.h>` unprefixed - upstream mandates
  that dir via pkg-config Cflags). **The enforced guard is the CI grep gate**
  (ci.yml "Include-guard lint": bare `<opusfile.h>`/`<vorbisfile.h>` fails the
  build; vorbisfile is pre-covered for the Vorbis slice). The -I ordering is
  defense-in-depth only. Recorded fact - the REAL compile line for
  LocalFileSource.cpp (ninja -v, Option C config, 2026-07-16):
  `-IE:/code/remoct/include -IE:/code/remoct/lib -IC:/msys64/ucrt64/include/opus
  -IC:/msys64/ucrt64/include/taglib -IE:/code/remoct/lib/pdcursesmod` - the opus
  dir does precede taglib's. Note `${MSYS2_PREFIX}/include` is ABSENT: CMake
  filters it as a compiler-implicit dir - which is also why the empty
  `${FDKAAC_INCLUDE}` interpolation never broke anything (the compiler's own
  default path covers those headers).
- **Opus ReplayGain is R128_TRACK_GAIN: an INTEGER in Q7.8 (dB×256), referenced
  to -23 LUFS.** Reading it as plain dB (the old third fallback in
  `populate_track_info`) turned an ordinary tag into hundreds of negative dB ->
  gain 0.0 -> a muted track. Correct read: `value/256 + 5` (+5 rebases -23 ->
  -18 LUFS, the ReplayGain convention). No header-gain term: libopusfile applies
  `OpusHead.output_gain` itself, and RFC 7845 defines the tag relative to that.
- **Pre-existing, KNOWN, deliberately not fixed here** (tombstones so they don't
  get rediscovered as new bugs):
  - `${FDKAAC_INCLUDE}` is interpolated in the include block ~50 lines before
    `find_path` sets it, so it expands EMPTY; builds work only because the whole
    MSYS2/system include dir is added separately. The Opus/WavPack include call
    was placed after the finds to avoid inheriting this.
  - The WASAPI warm-up in `initDevice` claims "file_src_ is null" in its comment,
    but on the first `play()` `file_src_` is assigned BEFORE `initDevice()`, so
    the warm-up callback can consume a few frames from the just-primed decoder;
    the outer "the callback never fires" comment is stale (the code does
    start/stop). Benign today, worth knowing before touching device bring-up.
  - `REPLAYGAIN_TRACK_PEAK` is written by the ripper but never read back: the RG
    apply path has NO clip protection beyond the clamp at 4.0.
  - The unconditional +6 dB RG preamp boosts UNTAGGED tracks by +6 dB when RG is
    on (db==0 -> gain 1.0, then ×1.995) - the opposite of normalization for
    untagged files, and a 0.0 dB tag is indistinguishable from "no tag".
- **vorbisfile.h does NOT need its own include dir (unlike opusfile.h).**
  Probe-verified on the live toolchain: `#include <vorbis/vorbisfile.h>`
  compiles with only the include ROOT on the path, because vorbisfile.h pulls
  its siblings as QUOTED includes (`#include "codec.h"` - resolved relative to
  its own dir) and codec.h uses the prefixed `<ogg/ogg.h>`. Contrast opusfile.h,
  whose unprefixed `<opus_multistream.h>` sibling includes force
  `-I<prefix>/include/opus` onto the path. So the vorbis CMake block adds NO
  extra -I entry and there is no ordering question vs TagLib's dir; a bare
  `<vorbisfile.h>` (which would resolve to TagLib's C++ header) is caught by
  the CI grep gate before anything compiles.
- **ov_read_float is PLANAR (float** per channel, decoder-internal buffers);
  op_read_float is INTERLEAVED (one flat float*).** They are not symmetric -
  the interleave loop lives only in VorbisDecoder, and the pointers ov_read_float
  hands back are invalidated by the next call, so interleave immediately.
  Chained Ogg streams can also switch channel count per link: clamp to the
  CURRENT link's channels (ov_info(vf, bitstream)) before indexing pcm[], or a
  channel-count change mid-file reads a garbage plane pointer.

## Rip encoder seam (rip-encoder-seam slice)
- **The rip write tail was ALREADY a streaming fan-out** - one 27-sector block
  fed FLAC -> LAME -> ebur128 -> AR/CTDB CRCs inside one loop iteration of
  ripTrack, same thread as the read. The IEncoder seam (IEncoder.h +
  FlacEncoder/Mp3Encoder) NAMES that structure; it did not create it. Tags,
  art, and ReplayGain are NOT part of IEncoder and never can be: album gain
  only exists after the last track (ebur128_loudness_global_multiple over
  handed-back per-track states), so tagging is a post-loop worker pass
  (tagFile) that the seam leaves at a zero-line diff.
- **Byte-identity of the transliteration is machine-enforced** by
  rip_encoder_seam_test: the pre-seam inline FLAC/LAME sequence is FROZEN
  verbatim in the test as a reference arm and diffed against the seam classes
  on a fixed PCM fixture. If you touch FlacEncoder/Mp3Encoder settings or call
  order, that test failing is the point - do not update the reference arm to
  match; the reference IS the contract until a slice deliberately changes the
  output format (then move the gate).
- **KNOWN, DELIBERATELY PRESERVED quirk: an MP3-side open failure leaves a
  finished, valid-but-EMPTY .flac on disk** (the inline code FLAC-finished on
  the LAME unwind path; the seam reproduces it via finalize(false)). The
  worker's zero-CRC heuristic catches it downstream. Do NOT "helpfully" clean
  it up in a refactor - changing error-path behavior couples a bug-fix to the
  byte-identity gate. If it deserves fixing, it is its own slice.
- One-bit simplification, analyzed as invisible: the inline code set
  ARStatus::NotFound when FLAC__stream_encoder_new returned null (alloc
  failure only) and NotQueried on every other open failure; the seam's bool
  open() collapses both to NotQueried. The distinction was dead: the worker's
  abort check keys on crc_v1==0 && crc_v2==0 (status not consulted), and the
  Pass-2 NotFound gate is unreachable for a track that failed before reading.
- **Session rip-format selection vs config default are two different states
  on purpose** (rip-format-select): `DigiConfig::rip_formats` is ONLY the
  startup seed; `UIManager::rip_sel_` is the live session state, toggled by
  the modal digits, normalized to kRipFormats TABLE order when building
  RipOptions (fan-out order must not depend on toggle history), and never
  written back to conf - config_.save() persists the unchanged default.
  Not reset on eject: preference, not disc data (contrast mb_release_).
  The modal's input disambiguation is structural: N/Esc handled first,
  digit cases return inside themselves, commit letters guarded on a
  non-empty selection - the wrong transition is unreachable, not guarded.
  CUE/M3U8 entries follow the MASTER format (first selected lossless, else
  first selected) so subset rips never emit playlists over absent files.
- **WavEncoder's strict writeFrames is load-bearing, not defensive**
  (rip-wav-encoder): a short fwrite returns false so the track aborts and
  the caller removes the partial. A lenient write would combine with the
  finalize size back-patch into "disk-full produces a valid-looking,
  silently TRUNCATED .wav marked success" - the header would honestly
  describe the truncated data and even pass a bit-exact check of what
  remains. The back-patch exists only for non-exact callers (the CD path is
  TOC-exact); with writeFrames strict it can never launder a write failure.
  Guarded by the /dev/full short-write check in rip_encoder_seam_test
  (Linux job). Do not "simplify" the strictness away.
- **The R128<->RG dialect lives in include/R128Gain.h, both directions, and
  nowhere else** (rip-opus-encoder): decode (LocalFileSource RG read) and
  encode (CDRipper tagFile Opus branch) call the same two inline functions,
  so the ~5 dB reference bug (decode slice, found in the wild) structurally
  cannot reappear as a drift between the two sides. Round trip is EXACT for
  every Q7.8 int (/256 and +/-5 are exact in binary FP over the range) -
  asserted over the FULL range in rip_encoder_seam_test. Do not inline
  either direction "for simplicity"; one home is the guard.
- **Opus rip notes**: libopusenc accepts 44100 input and resamples to 48k
  internally, sample-exact (probe-proven: 88200 in -> exactly 96000 @ 48k).
  Wide paths go through ope_encoder_create_callbacks over port::fopenUtf8 -
  ope_encoder_create_file's char* path is an ANSI trap on Windows. The class
  is OpusRipEncoder because opus.h typedefs a C OpusEncoder and the TU sees
  both. Header output gain is pinned 0 so the R128 tags are the only gain.
  Opus tags carry NO REPLAYGAIN_* and NO peak keys - R128 defines none.
- **The block-callback laundering trap (rip-wavpack-encoder), one level
  deeper than WAV's**: libwavpack writes through a BLOCK callback, and its
  finalize-time insurance (sample-index-vs-declared) counts samples
  SUBMITTED, not bytes written - so a lenient callback turns disk-full into
  a complete-looking truncated .wv marked success, and no downstream check
  can see it. The safety chain is: strict blockOut (short fwrite -> return
  0 + sticky write_failed_) -> propagation (probe-proven: PackSamples AND
  FlushSamples both return 0 after a failed block; note WavpackGetNumErrors
  stays 0 - it counts decode CRC errors, NEVER write failures, do not rely
  on it) -> IEncoder::finalize now returns bool so the final block's flush
  failure can fail the track (ripTrack folds finalize returns into ok
  before the cleanup). Same treatment applied to Opus's drain (the same
  buffering-encoder class). FLAC/MP3/WAV finalize return true
  unconditionally - their best-effort semantics are the pre-seam baseline,
  unchanged. Guarded end-to-end by the /dev/full chain test (Linux job).
- **Third layer of the same trap, found BY the ENOSPC chain test on its
  first run**: highly-compressible audio makes WavPack blocks smaller than
  the stdio buffer - fwrite "succeeds" into the buffer, the library flush
  "succeeds" (its callback never reached the disk), and the real ENOSPC
  would only surface at an unchecked fclose. Every FILE*-owning buffering
  encoder's finalize now does fflush-and-check before declaring completion
  (WavPack, Opus, and WAV's tail). This is why the chain test exists: the
  strict callback alone was NOT enough, and no amount of reasoning about
  libwavpack's propagation would have caught stdio's layer.
