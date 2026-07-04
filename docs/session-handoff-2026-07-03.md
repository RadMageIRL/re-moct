# Session handoff - 2026-07-03 (rev 8: slice 4 (Unix-socket IIpc) LANDED & 100% CLOSED - Dos verified on native Debian 13 + real Discord; next = slice 5 (libnotify))

## Rev-8 delta: slice 4 - the Unix-domain-socket IIpc twin (Discord RP transport)
- **Landed:** code `671d2b3` + this docs commit. Design
  `docs/phase3-slice4-design.md` (`581c2ce`) ratified by Dos BEFORE code,
  with both open calls decided: (a) flatpak/snap subdir probing lives in the
  IMPL (named knowledge leak, buys DiscordRP zero-diff); (b) npiperelay
  bridge attempted as the live gate - it WORKED.
- **Shape:** `IpcUnixSocket.cpp` (`platform::lnx::UnixSocketIpc`) - send =
  one `::send()` + `MSG_NOSIGNAL` (dead peer → false, not SIGPIPE - the
  load-bearing flag); waitReadable = the Windows 10 ms peek-poll loop
  verbatim with poll()+FIONREAD (queued bytes count after peer close, pinned
  Windows-baseline-first; the wait slice wakes early on data - named
  accepted-better); recvSome = one blocking recv, EOF→false; EINTR retried
  everywhere (SIGWINCH ≠ broken channel). Discovery: XDG_RUNTIME_DIR →
  TMPDIR/TMP/TEMP → /tmp, plain → flatpak → snap. DiscordRP/IpcWinPipe zero
  diff; UIManager Discord gates swept (^D live on Linux); **UIManager
  Windows preprocessed TU bit-identical** (cmp caught a 1-line if-condition
  re-flow - keep the baseline's line breaks when un-gating; lessons.md).
- **Tests:** discord_ipc_test now portable (IPC_IMPL pattern); NEW
  both-platform ipc_echo_test (real local server through the real impl -
  transport semantics incl. the MSG_NOSIGNAL dead-peer case + Linux
  discovery order), green on the untouched Windows impl FIRST. Windows
  ctest 15/15 baseline → **16/16**; Linux **13/13** first run.
- **Live gates, all passed in WSL2:** (1) socat echo probe - handshake +
  SET_ACTIVITY observed on the wire through a real external Unix socket,
  ONE write per frame. (2) **REAL Discord RP from the Linux TUI** via the
  npiperelay+socat bridge (`\\.\pipe\discord-ipc-0` as a genuine Unix
  socket, bridge in tmux - nohup does NOT survive a wsl.exe invocation;
  lessons.md): production READY for Dos's account, ^D → KKJO stream
  (`stream.abacast.net/.../eagleradio-kkjofmaac-ibc4`, real artist/song
  titles - Dos's pick after Dance Wave sat titleless) → "Doja Cat - Say So"
  ACCEPTED, async iTunes art refresh ACCEPTED (mp:external proxy), natural
  on-air track change (Ariana Grande) pushed through, bridge kill+restart →
  next track (JoJo) drove the lazy reconnect (failed send → disconnect →
  fresh handshake + RP restored with art). socat -v wire captures = the
  evidence (Discord's SET_ACTIVITY responses echo the accepted activity).
- **Honest limits, documented:** the bridge socket is socat's, so the
  flatpak/snap discovery candidates are fixture-proven only (ipc_echo_test
  S7); Windows live ^D spot-check treated as a formality (TU bit-identical,
  and the same Discord client displayed the Linux-driven RP all gate long).
- **100% CLOSED: Dos verified slice 4 on the native Debian 13 box with a
  real Discord install - native socket discovery + RP end-to-end, no
  bridge, all good.** NEXT = **slice 5 (notify: libnotify/notify-send** -
  gate: real notification via dunst; headless = documented best-effort
  no-op; REVISIT the slice-2 cmdline toast fallback: keep or drop). Then
  slice 6 (SG_IO CD - LAST).

(Rev-7 and earlier below - still current history.)

## Rev-7 delta: slice 3 - the ICY continuous twin; raw-transport work COMPLETE
- **Landed & PUSHED:** code `5c823f8` + this docs commit. Design doc
  `docs/phase3-slice3-design.md` ratified by Dos BEFORE code (survey with
  file:line, transport argument, THE correctness argument, audit + gate plan).
- **Shape:** curl `CONNECT_ONLY=1` (TCP+TLS) + hand-spoken HTTP/1.0 ICY
  request + hand-parsed headers + `curl_easy_recv` in InternetReadFile's
  refill slot - the pull-read shape verbatim; de-interleave/ring/producers
  untouched. `hConn_` holds the CURL* (shared guards unmodified); redirects
  via the portable hlsResolveUrl twin (cap 5); recv-first-then-poll (TLS
  buffering), 100 ms stop_ slices, 8 s dry bound. **Windows byte-verbatim at
  the strongest standard: preprocessed `_WIN32` TU bit-identical (g++ -E -P
  diff EMPTY).**
- **Probe findings (five wild shapes validated pre-code):** ALPN must be OFF
  (`CURLOPT_SSL_ENABLE_ALPN=0` - cloudflare negotiates h2 and kills the
  hand-written request); leftover header-overrun bytes are common (1078–1931
  observed) - the offset-0 invariant is exercised by real stations.
- **Baseline finding (baseline-first fixture run):** WinINet yields
  `icy_metaint=0` on "ICY 200 OK" status lines - Windows plays SHOUTcast v1
  but never parses titles; pinned per-platform in the test; the twin parses
  ICY status itself (named accepted-better, like its 75 ms vs 7703 ms
  measured blocked-read close).
- **New `icy_pipeline_test` (15th Windows / 11th Linux target):** real
  localhost ICY server through the real Continuous pipeline; run green on the
  untouched Windows baseline FIRST. Closes the slice-0 "ICY is live-gate-only"
  honest limit.
- **Gates ALL PASSED:** Windows ctest 15/15, Linux 11/11. Live Linux TUI:
  Dance Wave (https + cloudflare redirect followed live, MP3 continuous path)
  audible off the Pulse sink monitor (RMS 6393, 99.97% non-zero), StreamTitle
  across 2+ transitions in the [LIVE] display, station switch → new connect
  ~1.4 s after keypress; SomaFM real song transition caught. Windows
  non-event proven live (temp harness, scratchpad-only): same stations,
  titles + RMS + close 15 ms/0 ms.
- **Gotcha recorded:** ^U prompts for URL THEN station name (Enter skips) -
  a tmux gate must script the full prompt flow (cost three diagnostic loops).
- **Also this session:** Last.fm ^G usability pair landed & pushed
  (`02d1aa7`, Dos-ordered): session-check-first (mirrors ^B) + dead-pending
  recovery (getSession error codes: 14 keeps token + re-arms poll; other API
  errors clear pending and restart auth in the same keypress; transport
  failures keep it). Live-proven on Linux both ways (real session toast;
  sandboxed fake-HOME garbage token → fresh auth).
- **NEXT: slice 4 - IPC: Unix-socket IIpc twin** (`$XDG_RUNTIME_DIR/<name>`
  per the IIpc.h contract). Gate: discord_ipc_test on Linux + live socat echo
  probe; full Discord RP stays Windows-verified, documented. Then slice 5
  (libnotify), slice 6 (SG_IO CD - LAST).

(Rev-6 and earlier below - still current history.)

## Rev-6 delta: slice 2 - libcurl IHttp + vendored MD5, both platforms green
- **Verified:** Windows ctest 14/14; Linux ctest 10/10 (was 4). Shape + full
  mapping table: `docs/phase3-slice2-design.md` (approved pre-code) + the
  roadmap Done entry. Highlights: CURLSH share-handle sessions (scenario B of
  http_cancel_test = live proof the pool reuses ONE TCP connection);
  XFERINFOFUNCTION cancel; CONNECTTIMEOUT + LOW_SPEED stall-guard (NOT a
  whole-transfer deadline); MD5 = byte-verbatim Openwall `lib/md5.{h,c}`
  (approved deviation from header-only - verbatim upstream is more auditable),
  one LastFm signing path both platforms, vectors re-proven vs Python hashlib.
- **Live gates (Linux, real libcurl):** MB standalone probe - REAL Relish TOC
  from the 2026-06-21 rip log resolved to Joan Osborne - Relish (1995), 12/12
  titles; RadioBrowser TUI search returned KWIN 97.7; digital iHeart HLS (Z100)
  PLAYED - pump at edgeLag 0, live nowPlaying via CurlSession, audible output
  proven by sink-monitor capture (RMS 5791, 100% non-zero). NOTE: the progress
  bar in radio mode only displays metadata - it does not move (Dos-confirmed
  expected; do not chase a static readout as a bug). **Scrobble round-trips
  PASSED both platforms** (standalone probe + Dos's real keys): Last.fm
  updateNowPlaying + scrobble ACCEPTED and ListenBrainz validate (RadMageIRL)
  + single ACCEPTED - on Windows/WinINet (the MD5 wire regression) AND
  Linux/libcurl (Björk/Jóga accents). Linux keys live in `~/.config/RE-MOCT/`
  (UPPERCASE - lowercase is silently ignored).
- **Key-fix follow-up (`b2abb12`):** Linux ^U/^G/^B dead - an `#ifdef _WIN32`
  span in handleInput's key switch deleted seven cases from the Linux build;
  the terminal was proven innocent by a key probe (termios stays IXON-only).
  Per-case gates now; ^B/^G/^U live on Linux (verified: all three prompts
  open in the TUI); ^F/^Y/^R/^D stay gated until their slices (4/6).
- **TUI-parity follow-up (`4f0b240`, Dos-found):** (1) stream status readouts
  (Negotiating / nowPlaying + [LIVE]) were `#ifdef _WIN32` in drawProgress -
  Linux drew the blank file-mode bar over a live stream; un-gated (portable
  curses). (2) **Toast fallback, interim until slice 5:** showTrackToast also
  writes the message to the cmdline bar for ~5 s on Linux (`#ifndef _WIN32`,
  rip_status_ pattern) - ^B-with-token now visibly says "logged in as …",
  ^G-with-keys visibly says "approve in browser". REVISIT AT SLICE 5 (keep or
  drop once libnotify lands). (3) The "^G divergence" was CONFIG STATE, not
  platform: key+secret present → browser-auth branch on BOTH platforms; none →
  in-app key→secret prompts on BOTH (fresh-user bars verified live on Linux).
  Gate outputs: "Negotiating Radio Stream…" → "Lady Gaga - Alejandro  [LIVE]";
  ^B → "ListenBrainz: logged in as RadMageIRL"; fresh-user ^G/^B bars open.
  Windows 14/14 (zero behavior change), Linux 10/10.
- **Scrobbler-engine follow-up (`91caf7a`, Dos-found: prompts opened but no
  scrobbles landed on Linux):** the ENTIRE bodies of updateScrobbler +
  startLastfmPoll + startListenBrainzValidate were whole-body `#ifdef _WIN32`
  (slice-1 scaffolding from the MD5-placeholder era) - empty shells on Linux.
  Gates removed (Discord blocks keep inner gates, slice 4). LIVE app-driven
  proof on Linux: Z100 in the TUI → junk filter skipped the station slate →
  "Zara Larsson - Lush Life" now-playing (Last.fm resp OK, LB playing_now
  200) → scrobble at elapsed=90: **Last.fm accepted:1 ignored:0, LB single
  200.** Windows 14/14, Linux 10/10. Lesson recorded: sweep whole-body gates
  when a slice makes a subsystem portable; "defined but not used" warnings =
  gated-out consumers, treat as porting TODOs.
- **Hard-won:** POSIX `close()` does NOT wake a blocked `accept()` - first
  Linux http_cancel run hung forever; fix = shutdown-then-close
  (`stopListener()`) + ctest TIMEOUT 60. Also pinned in lessons.md: WSL
  build/test discipline (ONE foreground run to a log with an EXIT= marker,
  ONE read; never poll ps, never restart-to-check).
- **Parked (recorded, untouched):** `port::logDir()` misses the grandparent
  mkdir (fresh account → silent no-log); radio-browser nl1/at1 mirrors
  DNS-dead (de1 fine).
- **NEXT: slice 3 - ICY raw-loop Linux twin.** Design-first, sacred territory
  (the Windows loop stays raw WinINet byte-verbatim; zero-diff invariant).
  Lean shape per the slice-0 decision: curl CONNECT_ONLY + curl_easy_recv
  keeps the pull-read shape. Gate: ICY station plays on Linux; Windows loop
  byte-diff EMPTY.

(Rev-5 and earlier below - still current history.)

## Rev-5 delta: slice 1 - portable core compiles, links, and PLAYS on Linux
- **Gate PASSED both sides:** remoct's TUI ran in a WSL2 tmux pty, browsed
  /root/Music, and played a generated 20 s WAV to completion (progress bar/
  bitrate/BPM live; `pactl` showed sink-input "remoct", Corked: no). Windows
  ctest 13/13 BEFORE the edits (baseline) and AFTER (regression); Linux ctest
  4/4; the CI Linux job now builds the FULL remoct binary.
- **Shape of the change** (full detail in roadmap Done): whole-file `#ifdef
  _WIN32` gates off 21 files; `include/PortUtil.h` (Windows expansion of every
  helper = the baseline call VERBATIM - sleepMs/tickMs/fopenUtf8/tempDir/
  logDir); Log + IHeartDeepLog rewritten portable (std::filesystem; XDG state
  dir on Linux); CDRipper mechanical (kSep keeps Windows rip-log paths
  byte-identical for slice 6); LastFm MD5 = Linux placeholder (slice 2 vendors
  real MD5 both platforms, per decision); StreamSource: ICY transport gated
  Windows-only byte-verbatim (connect() refuses Continuous on Linux with a
  clear error until slice 3), HLS fully portable incl. a hlsResolveUrl twin;
  utf8_to_wide got a UTF-32 Linux twin reusing utf8_next; ShellExecuteA →
  xdg-open; version tag -win/-linux per platform.
- **Audits held:** zero sacred symbols in the StreamSource diff; all its
  change lines in 5 enumerated classes; no unguarded platform includes outside
  src/platform/win/; PortUtil.h consumed from .cpp only.
- **Review notes:** Ctrl+Q didn't reach the app through the tmux pty (suspect
  IXON - recheck on a real terminal); config dir is ~/.config/RE-MOCT vs logs
  in ~/.local/state/re-moct - naming unification someday; a live Windows
  listen spot-check is recommended (headless 13/13 done here).
- **NEXT: slice 2 - HTTP: libcurl IHttp impl** (+ IHttpSession keep-alive,
  cancel token via progress callback, RedirectPolicy incl. SameScheme,
  read_error; vendored single-file MD5 on BOTH platforms with api_sig parity;
  POSIX twin of http_cancel_test; port the if(WIN32) request tests). Gate on
  Linux: MB lookup resolves, RadioBrowser search, live scrobble round-trip,
  digital iHeart HLS PLAYS; hls_pipeline_test in the matrix. STOPPED for Dos
  review before starting.

(Rev-4 delta below: slice 0 + readiness survey, still current.)

## Rev-4 delta: Phase 3 kickoff session (2026-07-03, morning)
- **Readiness survey DONE and approved by Dos** - full assessment in
  `docs/phase3-readiness.md` (committed `ebf5382`). Headlines: 12/23 TUs
  Linux-clean once the pervasive whole-file `#ifdef _WIN32` gates are stripped
  (the gates make a naive Linux build an empty shell - the survey's key
  method finding); both known-unknowns GREEN (ncursesw = 256 colors + wide-API
  3/3 on Trixie, the COLORS=8 ceiling was MSYS2-terminfo; miniaudio =
  PulseAudio live in WSL2, 27k frames through the real callback); CMake
  configures unchanged; 4 pure suites pass on Linux as-is; fdk-aac needs
  Debian non-free (enabled).
- **Slice 0 LANDED** (code `27735f5`): CI matrix (debian:trixie container +
  windows msys2/UCRT64, both on every push - the trixie container matches the
  WSL2 inner loop, drift-immune), `src/platform/linux/SeamStubs.cpp` (inert
  contract-failure stubs + the four core:: bridges, namespace `platform::lnx`),
  CMake WIN32-guarded MSYS2 block + `remoct_linux_seams` OBJECT target.
  Verified: Windows 13/13 before AND after; WSL2 rehearsal 4/4; live matrix
  green on push.
- **CD gate venue RESOLVED with evidence - usbipd→WSL2, no VM:** the GHD3N is
  USB (JMicron 152d:0578, busid 4-1); usbipd-win 5.3.0 installed, bound (stays
  bound), attached → real scsi3-mmc `/dev/sr0`+`/dev/sg4`; sg_inq, cdparanoia
  full TOC (Relish - T1 LBA 32 = MSF 182 − 150, conventions reconcile as
  pinned), raw READ CD 0xBE SCSI Good + byte-identical re-read. Gate ritual:
  `usbipd attach --wsl --busid 4-1` (drive leaves Windows) / `detach` returns it.
- **Decisions taken with Dos:** vendored single-file MD5 both platforms (gate:
  api_sig parity via live scrobble on both); ICY twin transport deferred to
  slice 3 design-first (lean: curl CONNECT_ONLY + curl_easy_recv - keeps the
  pull-read shape); directory policy = XDG conventions, exact mappings proposed
  in the slice that touches them; WSL2 Debian Trixie provisioned (deps in
  readiness doc §4).
- **NEXT: slice 1** - portable core compiles + links on Linux (platform-util
  layer; mechanical de-Win32 per the §2 classification; de-gate whole-file
  wraps; StreamSource header detox with ICY staying Windows-only until slice
  3). Gate: remoct plays a local file on Linux + Windows ctest 13/13 zero
  behavior change. **STOPPED for Dos review before starting slice 1.**

(Rev-3 content below - the Phase 2 close + cleanup pass - still valid history.)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(Prior history: `session-handoff-2026-07-02.md` - Phase 1 completion + the
Phase 2 kickoff/slice 0/slice A, all still valid; this file adds slice B and
the current fork.)

## Pinned constraints (do NOT re-litigate)
- **The 150-sector offset is a physical property of the disc** - AccurateRip's
  design, per the HydrogenAudio forum posts at
  https://hydrogenaudio.org/index.php/topic,97603.0.html. Never "fix" the 150.
  AR math / disc-ID normalization (T1-LBA-relative) untouched.
- **The live StreamSource audio read loop stays OUT of any seam, permanently**
  (`InternetReadFile` only in `rawRead`; `InternetOpenUrl` only in connect()'s
  ICY branch). Producer/ring machinery (`ringClear`, `prebuffered_`, re-pin) is
  sacred - zero-diff invariant in every Phase 2 slice, held through A and B.
- **`next_decoder_initialised_` is the load-bearing swap protocol** - slice B
  changed its PAYLOAD (struct → pointer) with the atomic itself appearing in
  the diff only as unchanged context. Its name/orderings/positions stay.
- **The retirement scheme is load-bearing** (slice B): the audio thread parks
  the outgoing LocalFileSource in `retired_src_` and NEVER frees; pollEvents
  reaps under the `track_swap_flag_` synchronizes-with edge. Do not "simplify"
  the reap away - it closes latent race F2 (UI-thread positionSec vs the swap).
- Ctor DI per single-consumer seam; `core::http()/setHttp()` the one
  transitional global; the frozen NotifyWinToast `-EncodedCommand` block; CD
  seam audit invariants - all as per the 07-02 handoff.

## What this session (07-02 → 07-03) accomplished
- **Slice A** (`0b7acf4` + docs `6d06a0e`, live gate `3d4e061`): `core::ISource`
  at `include/core/ISource.h` (readFrames/caps/positionSec/durationSec/seekTo/
  close; open()/metadata/pause deliberately excluded - roadmap Decisions log);
  StreamSource (zero .cpp hunks) + CDSource implement it; pipeline tests
  retargeted through `ISource&`; live gate: file + live iHeart digital + CD
  seek all green.
- **Slice B** (`845a155` + this docs commit): LocalFileSource extraction - see
  roadmap Done entry. The crux: the audio-thread struct-copy swap became a
  pointer move + retirement under the identical release/acquire protocol;
  correctness argument ratified BEFORE code; audio-thread diff = exactly three
  enumerated change classes. Verified: ctest 13/13 (xfade +S5, test-first),
  headless mechanics gate ALL PASS (incl. 88.9 M positionSec reads hammered
  through a live crossfade swap, varispeed 1.50× measured, device-switch
  continuity, 11 h .m4b), live listen gate passed ("works as previous").
- **VBR live-bitrate readout investigated and parked** (Dos-spotted): forward
  seek on a VBR MP3 → huge one-window kbps spike, settles slightly above
  nominal. A/B probe on baseline `3d4e061` reproduced it identically -
  pre-existing estimator behavior (linear file-position model), slice B
  exonerated. Parked in roadmap as a product decision (what should the readout
  mean); do NOT fix as a ride-along.
- `docs/AccurateRipPipe.md` (Dos's Mermaid AR-flow diagram) tracked with this
  docs commit - verified consistent with the pinned 150-as-physical-preamble
  framing ("150 sectors before track start", disc-absolute mul_by anchoring).

## Current state
- **Branch:** `restructure`. Slice B = code `845a155` + this docs commit -
  LOCAL until Dos says push. Origin in sync through slice A's `3d4e061`.
- **Tests: 13/13** (xfade_handoff_test now has S1–S5 incl. replace-preload).
- **Layout:** `include/core/{IHttp,IIpc,INotify,ICdIo,ISource}.h`;
  `include/{LocalFileSource,TrackInfo}.h` + `src/LocalFileSource.cpp` beside
  StreamSource/CDSource; platform impls in `src/platform/win/`.
- **Phase 2 status: A+B achieve the phase's goal** - all three real source
  families implement `core::ISource`, proven against what existed.

## PHASE 2 CLOSED (rev-2 delta)
**Slice C assessed via written go/no-go and DECLINED (Dos confirmed).** The
decisive reason (§1c of the assessment): a single active-`ISource*` is
factually WRONG for the file branch - it runs TWO live sources during a
crossfade (varispeed reads the file source; the mix and gapless fill read
`next_src_`), so only 3 of the 6 callback readFrames sites could use it - the
hlsHttpGet lesson at callback scale. Supporting reasons: a second source of
truth for the playing mode (can disagree with the mode flags mid-transition -
the teardown-ordering class slice 0 guards); a fourth audio-thread change
class (pointer staleness at every swap - the F2 lesson replayed); the vtable
cost itself is unmeasurable and was NOT the argument; and nothing downstream
needs it (Phase 3 recompiles the portable core; Phase 4's plugin dispatch is
its own boundary - revisit there if its registry generates a concrete need).
Full entry in roadmap.md's Decisions log. **Phase 2's goal is achieved: all
four real sources (file, iHeart, ICY, CD) behind `core::ISource`, proven under
the slice-0 net + live gates, zero new dependencies.** Docs-only close-out;
nothing in the tree changed, which is the point.

## Cleanup pass (rev-3 delta) - three briefs, three closures, all pushed
1. **VBR bitrate readout: FIXED** (`adacbb1`/`4cdfa88`) - the "live" estimator
   was a linear position model (constant-derivative average, never
   instantaneous); now `bitrateKbps()` = TagLib nominal for everything, gate
   probe pinned VBR at 264 across a seek (was peak 90k).
2. **Canonical SET_SPEED: ADOPTED** (`d2ad038`/`0b0ddb2`) - CDSource::open
   really resets to max (0xFFFF) via the seam for the first time; contract in
   cd_toc_test; live gate on drive G clean; 1764 quiet-mid is a NAMED FALLBACK
   only if max ever proves audibly objectionable.
3. **iHeart rabbit-hole desync: CLOSED WITH EVIDENCE** (`2913a1b`, analysis
   only) - Stage-B promotion REJECTED (0/8 lane-music-first; SSAI stitches new
   joiners into ad pods too), faster-polling/staggered-peek rejected same
   premise, edgeLag flat, 35 s stall validated 68/68. Hole classes: A =
   broadcast-side (OOB shows no song, unfixable), B = session-side stale slate
   with OOB-proven live music. **Class-B OOB-gated re-pin is PARKED, NOT
   greenlit** (own design-first pass + live gate when chosen); Stage-A scaffold
   retirement is its own small future cleanup, never folded in.

## NEXT SESSION: Phase 3 kickoff (Linux port) - planned for the morning
Read CLAUDE.md + roadmap + lessons + architecture + THIS handoff first, then
design-first as always. The shape from roadmap/architecture: WSL2 on 7of9 as
the fast inner loop; GitHub Actions matrix (windows-latest via msys2 +
ubuntu-latest) as the source of truth - the CI matrix is the highest-leverage
first move (it catches Windows-isms leaking into core the moment they appear);
`src/platform/linux/` impls for the four seams (libcurl HTTP, Unix-socket IPC,
libnotify notify, SG_IO CD - the ICdIo header already documents the SCSI
one-to-one mapping); the pure suites already build Linux-clean by design.
Expect a survey first: what actually stands between today's tree and a Linux
configure (ncursesw UI, miniaudio backend, windows.h stragglers outside
platform/ - e.g. CDSource.cpp's Sleep, Mp4Chapters, IHeartDeepLog, Log).
Remaining parked pool: Class-B OOB-gated re-pin, Stage-A retirement, `stop_`
into IHeart polls, stalled-connect interrupt, Track Info album-tag decode,
scrobble dedup, ICY items.

## Operating discipline (unchanged)
- Read CLAUDE.md + roadmap + lessons + architecture + THIS handoff first.
- Design-first; correctness argument before audio-thread changes; probe-first.
- Verify → build → ctest → real-world gate → commit (code and docs separately,
  no Co-Authored-By). Update roadmap/lessons/handoff as the session's last step.
