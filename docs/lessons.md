# RE-MOCT — hard-won lessons

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
  '?' — acceptable, no regression.
- Color pairs 1–13 are themed (init loop); `CP_VIZ_TIP` (14) = peak fg on default bg,
  init_pair'd after the loop. Viz pairs are fg==bg solid space-fills, so a partial
  block needs a real bg — that's why the sub-cell tip uses `CP_VIZ_TIP`. Theme reload
  re-runs `initColours()`, so new pairs added there survive `~` live-reload.

## AccurateRip / CD ripping
- **The 150-sector physical preamble is correct BY DESIGN** (a property of how
  AccurateRip works, confirmed across HydrogenAudio threads). Do NOT re-litigate.
- Disc ID normalization is relative to the **T1 LBA**, not a hardcoded 150 — handles
  non-standard pregaps.
- Enhanced CD needs the full-disc-leadout disc ID, binary-search silence detection,
  and a +174-sector correction.
- `mul_by` increments for **every** sample, including skipped boundary samples.
- `AR_SKIP = 2940` (not 2941). CRCv2 = `csum_lo + csum_hi`.
- The negative drive-offset OOB read is real but BLOCKED on testing (Dos's drive is
  +6); needs a synthetic negative-offset vector. Don't patch blind.

## Streaming ring buffer / re-pin
- **An iHeart hole where iHeart's own trackHistory/ctm reports no current song
  is broadcast-side (unfixable); one where OOB shows fresh songs while our
  session loops a stale slate is session-side (the fixable class). The OOB
  liveness signal is what separates them, and it's the correct gate for any
  re-pin.** From the 2026-07-03 desync analysis: the longest, scariest holes
  (8–12 min single-cartcut loops) had NO song playing per iHeart's own
  endpoints — nothing to rejoin, by design; the fixable 131–300 s holes had
  fresh songs provably airing while our edge stayed stale. A re-pin gated on
  OOB liveness cannot fire into a real broadcast break, because the gate was
  false throughout every one observed.
- `ringClear()` is called in BOTH the AAC and MP3 re-pin paths, after
  `prebuffered_.store(false)`, to break a producer/consumer deadlock on self-heal.
  It's load-bearing for ad-skip re-pin crossfade — do not "clean it up."
- `LIVE_STALL_MS = 35000`. `prebuffered_` + `ringClear()` interactions are subtle;
  changes here need explicit justification and confirmation.
- **ICY now-playing only updates when the broadcaster actually sends a StreamTitle in
  the metadata block on a track change.** Plenty of stations skip it on some
  transitions (or send station IDs instead of songs). So a label that fails to update
  across one transition is the STATION's silence, not a client parsing bug — rule out
  the broadcaster before suspecting `readAudio`/`parseIcyMetadata`. Ruled out exactly
  this way during the slice-4 ICY regression gate (a missed update on one transition,
  station-side). Related parked items: ICY metadata improvement is a structural
  protocol limit; ICY ingest sanitization (station-ID stripping) — see `roadmap.md`.

## MP3 seek (bit reservoir)
- MP3 frames borrow main_data from up to ~511 bytes of preceding frames (the bit
  reservoir). A raw seek lands "cold" → the first ~50–150ms decode garbled until it
  warms up. FLAC has no reservoir, so FLAC seeks are clean — that's the whole asymmetry.
- **Prime-after-seek:** in `seekTo`, land ~0.18s before the target and decode-discard
  up to it so the reservoir is warm before audio resumes. Harmless for FLAC. Runs in
  the device-stopped window (no callback race; callback also guards on `seeking_`).
- **Seek coalescing:** holding `[`/`]` fires many key-repeats; each raw seek does a
  device stop/seek/start + warm-up. `requestSeek`/`flushPendingSeek` apply single taps
  instantly and collapse held repeats to ~one seek per 100ms.
- **Track-stamp guard:** a buffered seek is tagged with the current playlist index and
  discarded on flush if the track changed — prevents a seek aimed at track A executing
  on track B. Costless, no deadzone.
- A 300ms post-track-change **lockout was rejected** — silent dropped seeks every
  track change were worse than the rare benign held-key bleed. Lay tradeoffs out.

## Cover art
- iTunes `entity=song` + Deezer `/search/track`, with **dual artist+title
  validation**, prevents wrong-artist matches. CAA is unreliable for Discord (returns
  "?" for missing front covers) — route Discord art through iTunes/Deezer.
- `CoverArt` namespace: `bytesByMbid`/`bytesByText` (tag embed), `urlByText`/`urlBySong`
  (Discord URLs). httpGet caps body at 10MB and rejects truncated-on-cap bodies.

## Process / workflow lessons
- **Additive-only.** Full-file replacements built on a stale baseline silently drop
  prior work. Build on the files Dos uploads in the same turn; prefer tight diffs when
  the base isn't re-uploaded.
- Brace-balance + scoped-diff audit before every handoff. Unit-test pure functions with
  isolated g++ where possible. Probe-first for new parsing/protocol logic.
- Cosmetic iteration is fine ("dessert"), but the Phase 0 harness is the "vegetables"
  that make the next big refactor safe — don't let polish indefinitely defer it.

## Platform gating
- **Gate `#ifdef _WIN32` per-case with the reason on each, never per-region —
  and when keys die on one platform, probe the terminal before blaming it.**
  Slice 1 wrapped a REGION of handleInput's key switch for Ctrl+D (Discord);
  the span silently deleted six OTHER handlers (^B/^F/^G/^U/^Y/^R) from the
  Linux build — the symptom (some control keys dead, others fine) looked
  exactly like tty line-discipline theft, and the first "fix" (clearing
  ISIG/IEXTEN) was about to remove ^C on no evidence. A 30-line minimal-curses
  probe with the app's exact init settled it in one run: every code reached
  getch(), so the bug had to be app-side. Rules: (1) each gated `case` gets
  its own `#ifdef` + a comment naming WHY (which slice un-gates it); (2) a
  dead-key report on Linux = run the key probe FIRST — the terminal is
  innocent until getch() says otherwise; (3) grep `#ifdef _WIN32` spans in
  dispatch switches when porting — a gate around one feature's case swallows
  its neighbors invisibly (the build stays green; the keys just vanish).
- **The same class, one level deeper: whole-FUNCTION-body gates outlive the
  era that justified them — when a slice makes a subsystem portable, sweep for
  the scaffolding gates that assumed it wasn't.** Slice 1 gated the entire
  bodies of updateScrobbler / startLastfmPoll / startListenBrainzValidate
  (correct then: MD5 was a Linux placeholder, scrobbling couldn't work);
  slice 2 made the whole chain portable but the gates stayed — so the login
  prompts worked (key fix) while the ENGINES were empty shells: no scrobbles,
  auth polls never committed, token validation never started. Windows fine,
  probe fine (transport innocent), app silent. Tell-tale: the Linux build's
  "defined but not used" warnings on normTrackId/looksLikeRealTrack/
  deinvertArtist were this bug announcing itself — a static helper warning
  means its consumer is gated out; treat those warnings as porting TODOs,
  not noise. End-of-slice ritual now: `grep -n "#ifdef _WIN32" | audit each
  span` over files the slice made portable.
- **Third instance (slice-5 follow-up, Ctrl+A/Ctrl+K): a per-case gate whose
  comment can't name an unported dependency is a missed un-gate, not a
  deferral — audit EVERY gated case's reason, not just the reported one.** The
  key switch documents why ^D/^F/^Y/^R are gated (Discord readiness / MBSearch
  overlay still Windows-gated / CD slice 6), but ^A (deep iHeart log) and ^K
  (digital-vs-raw stream) sat under bare `#ifdef _WIN32` with NO reason — both
  depend on nothing unported (IHeartDeepLog portable since slice 1; both stream
  paths since slices 2/3), so they were simply forgotten un-gates: dead on Linux
  (no toggle, no toast), reported by Dos closing slice 5. Rule: when un-gating
  per-reason, sweep the neighbours — a gated `case` with no nameable unported
  dep is a bug. (Toast-only variant: ^T/^N run their function but their
  `showTrackToast` is still `#ifdef _WIN32` — the toggle works, the toast is
  silent on Linux; same class, cosmetic.)

## Raw ICY transport (Phase 3 slice 3)
- **Disable ALPN before speaking hand-written HTTP/1.x over a curl
  CONNECT_ONLY connection** (`CURLOPT_SSL_ENABLE_ALPN=0`). With ALPN on,
  curl's TLS handshake offers h2; a CDN edge (cloudflare) accepts it, and the
  hand-written HTTP/1.x request is then protocol garbage — the server just
  closes. Plain Icecast/SHOUTcast hosts don't negotiate h2, so the recipe
  works everywhere EXCEPT behind a CDN — exactly the shape a quick test
  misses. Found by the five-shape station probe before a line of product code
  (probe-first paying again).
- **WinINet never surfaces `icy-metaint` from an "ICY 200 OK" status line** —
  on a true SHOUTcast v1 server the Windows baseline gets `icy_metaint=0`:
  audio plays (the decoder's ADTS/MP3 resync swallows the interleaved
  metadata bleed) but StreamTitle never parses. Found by running the new
  fixture test against the UNTOUCHED baseline (the baseline-first pattern
  doing its job — the fixture spoke honest ICY and Windows failed the title
  asserts). The behavior is pinned per-platform in icy_pipeline_test;
  Windows stays byte-verbatim, the Linux twin parses ICY status itself (a
  named accepted-better delta, same class as its 75 ms vs 8 s stop latency).
- **The offset-0 invariant is the whole correctness story of a hand-rolled
  ICY client:** the de-interleave arithmetic is transport-blind, so alignment
  reduces to "the first byte served is body offset 0" — any bytes read past
  the header terminator MUST be preserved as the stream head (they arrive
  with the headers on most stations: 1078–1931 leftover bytes observed live).
  WinINet hid this by consuming exactly the headers.
- **Script a TUI's FULL prompt flow before reading a gate as failed.** The
  first live gate showed [LIVE] but a silent sink — three diagnostic loops
  (env, pulse, backend theories) before spotting that ^U asks for URL *then a
  station-name prompt*, and the script never sent the second Enter: the
  stream had simply not started during the capture window. A tmux gate is
  driving the app blind — capture the pane after EVERY keystroke batch when
  building the script, not just at the end.

## IPC transport (Phase 3 slice 4)
- **`MSG_NOSIGNAL` on every Unix-socket send is load-bearing parity, not an
  option flag.** WriteFile to a broken pipe returns FALSE; plain send() to a
  dead peer raises SIGPIPE and KILLS the process. The flag turns peer-death
  into EPIPE → `false` — the contract's failure mode. ipc_echo_test S6 pins
  it: a miss is a test-process death, not an assert failure. Same class:
  EINTR must be retried in send/recv (ncurses' SIGWINCH interrupts a blocked
  read on a resize) — signals are a failure mode the Windows twin cannot
  have, so surfacing them as "broken channel" would be a behavior invention.
- **"Byte-verbatim" includes LINE LAYOUT once a preprocessed-TU diff is the
  standard.** Un-gating `&& !config_.discord_presence` and re-flowing the
  3-line if-condition onto 2 lines produced a 1-line TU delta with an
  identical token stream — cmp caught it, the fix was keeping the baseline's
  own line breaks. When removing `#ifdef`s around live code, delete ONLY the
  directive lines; never re-wrap the surviving code.
- **A `nohup`'d background process does NOT survive its `wsl.exe` invocation
  ending; a tmux session does.** The first bridge attempt (nohup socat) was
  dead before the next command ran — silently, with an empty log. Any fixture
  that must outlive one `wsl -e bash` call (socat bridges, servers, the TUI
  under test) goes in `tmux new-session -d`; the tmux server keeps the WSL VM
  and the process alive across invocations.
- **The npiperelay + socat bridge is a REAL live-Discord venue for WSL2** —
  Windows Discord's \\.\pipe\discord-ipc-0 exposed as a genuine Unix socket:
  `tmux new-session -d -s bridge "socat -v 'UNIX-LISTEN:/tmp/discord-ipc-0,
  fork' EXEC:'npiperelay.exe -ep -s //./pipe/discord-ipc-0' 2>log"`. Bonus:
  `socat -v`'s wire capture IS the gate evidence — Discord's READY carries
  the logged-in user, and its SET_ACTIVITY responses echo the ACCEPTED
  activity (asset keys resolved to CDN ids, external art proxied to
  `mp:external/...`), so "RP shows title/artist + art" is provable at the
  byte level without eyes on the Discord window. Honest limit: socat creates
  the socket, so the flatpak/snap discovery candidates stay fixture-proven.

## WSL build/test discipline — one run, one read, the log is the only truth
- Run every build/test FOREGROUND, redirected to a log, with an exit marker:
  `<cmd> > /root/out.log 2>&1; echo EXIT=$?`. Then read the log ONCE:
  tail + `grep -E "error:|Failed|Passed|EXIT="`.
- **Never background a build and poll ps/process state to infer whether it
  passed.** Process-alive tells you nothing about pass/fail — a finished-clean
  build and a still-running build look identical from ps, and you will loop
  forever restarting a build that already succeeded.
- **Never restart a build "to check on it."** If the log has no `EXIT=` yet, it
  is still running — wait, don't relaunch. Relaunching resets the clock and
  hides the result.
- A cold full `cmake -S . -B` configure + build is legitimately slower
  (3–5 min) than an incremental (`ninja: no work to do`, seconds) — slowness is
  not failure. Read the log to distinguish; don't assume a stall and restart.
- The answer to "is it done? did it pass? why did it fail?" is always in the
  log file, never in the process table. If you find yourself checking ps or
  relaunching, stop — go read the log. (Cost three loops in the slice-2
  session before being pinned.)
- **`/tmp` is a per-invocation tmpfs, wiped between separate `wsl.exe` calls;
  `/root` persists.** Each `wsl.exe -d Debian …` launch that lets the distro
  idle out clears `/tmp` — a harness / fake-binary / capture file written there
  in one call is GONE in the next — while `/root` (ext4) survives (that's why
  the synced source tree and build dir persist across calls but a `/tmp`
  scratch file doesn't). Put anything that must outlive a single call under
  `/root`, or do the whole write-run-read in ONE `bash < script.sh` invocation.
  (Cost one loop in slice 5 chasing an "empty" argv capture a prior call had
  actually written to a since-wiped `/tmp`.)

## Readouts / estimators
- **The "live VBR" bitrate estimator was a linear position model — a
  constant-derivative average dressed as a live reading; TagLib's nominal is
  both simpler and more accurate.** `(pos/dur)·file_size` differentiated can
  only ever yield file_size/duration in steady state: no VBR signal, just the
  average plus artifacts (tag-byte inflation, a seek-window spike, timing
  jitter). Before "fixing" or "labeling" a live readout, check whether the
  estimator can measure what it claims at all — this one couldn't, which
  turned a labeling question into a deletion.

## Audio-thread refactoring (Phase 2 slice B)
- **A pointer needs no stronger publication guarantee than the struct it
  replaces — but the FREE side is where a naive pointer swap regresses.** The
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
  pointers are heap-targets, not self-references** — an undocumented
  relocatability property the baseline silently relied on. A fixed heap address
  for the decoder's whole life (the source object) removes that reliance.
- **Slice-B accepted residuals (inert, documented so a future differ isn't
  surprised):** (1) the outgoing decoder's uninit/file-close now happens ~one
  pollEvents tick later on the main thread (was: inside the audio callback);
  (2) on a read that returns a FULL buffer and EOF together, track_done fires
  ≤1 callback (~10 ms) later (the readFrames wrapper signals EOF by short read
  only) — same audio out, flag shifted one tick; same shape in varispeed's
  top-up loop (one extra 0-frame call before eof); (3) the TagLib hint
  pre-reads in play/preloadNext were DROPPED, not moved — open_decoder
  provably discarded them (`(void)hint_channels`) since the forced-44100/2
  config replaced hint sniffing (the slice-8 "don't migrate garbage" rule);
  (4) setDevice's file-restart now primes the decoder and re-reads tags into
  the source's private info_ (current_track_ untouched) — inert warm-up.

## Replay-net / test-fixture lessons (Phase 2 slice 0)
- **"A few quiet reads" is NOT end-of-stream when legitimate in-stream silence
  exists.** With a fake-speed producer, the reader finishes writing a whole track
  (including the transient-error silence-fill gap) into the ring long before a
  paced consumer drains it — a drain loop that exits on "N consecutive all-zero
  reads + reader stopped" terminates mid-gap and reads as data loss. Proof of
  emptiness must be structural: require a consecutive zero-run LONGER THAN THE
  ENTIRE RING — the ring cannot hold that much, so it must be dry. (First
  cd_pipeline_test run failed exactly this way.)
- **Windows `Sleep(1)` is ~15 ms** (default timer granularity). Two consequences
  for producer/consumer tests: pacing margins are far larger than they look on
  paper (good), and pacing where none is needed dominates runtime (bad — pace the
  consumer only while the producer is live; once it has stopped, the ring is a
  static buffer and can be drained flat out. Cut cd_pipeline_test from 25 s to 7 s).

## HTTP / platform-seam migration
- **Parity for an HTTP migration lives at the call sites, not the seam.** The FakeHttp
  unit test feeds canned bodies, so it proves parse logic but CANNOT catch a wrong
  request field — body cap, timeout, user-agent, status-gating, or redirect policy. Those
  must be eyeball-confirmed against the pre-migration baseline, per site, by hand.
- **Verify repo structure with `ls`/`dir` before asserting file placement.** A stated
  layout is a claim, not ground truth. Phase 1 slice 1 nearly committed `IHttp.h` /
  `HttpWinInet.cpp` at the repo root on a false "flat layout" premise; a directory
  listing caught that the tree already had `src/`+`include/`. Check, don't assume.
- **`INTERNET_FLAG_SECURE` derived from the URL scheme is a no-op for all-HTTPS sites
  but a real behavior change for a plain-HTTP one.** Harmless through group (a) (MBLookup
  + RadioBrowser are HTTPS); flag it when CDRipper's AR/CTDB fetch (plain `http://`
  accuraterip.com / cuetools.net) migrates in group (c).
- **HTTP request bodies go over the wire as raw UTF-8 bytes — never widen them.**
  `HttpSendRequest`'s payload is a byte buffer and `nlohmann::dump()` emits UTF-8; widening
  a POST body to `wchar_t` corrupts non-ASCII payloads (e.g. "Björk"). A FakeHttp test proves
  the consumer *builds* the body right, but only a live submission viewed on the service
  proves *wire* fidelity — do both (group (b): Björk/Jóga verified on Last.fm + ListenBrainz,
  accents intact).
- **The two group-(b) GETs inherit the seam's GET-path SEND timeout** their per-call
  baseline never set — inert on a bodyless GET, documented in-code, and accepted rather than
  adding a per-request timeout field (which the interface deliberately doesn't have). Parity
  is matched site-by-site; where a single seam policy can't match every site, pick the one
  that keeps shipped sites byte-identical and document the inert residual.
- **`reject_truncated` must CLEAR the body, not merely fail the response.** A capped read
  can leave a valid-looking leading fragment (image magic bytes, a JSON prefix); returning it
  with only `ok=false` invites a consumer that checks the body to embed a partial cover.
  `finalizeBody()` clears it (CoverArt's 10 MB reject/clear). Groups (a)/(b) pass
  `reject_truncated=false` (cap-and-keep, e.g. MBLookup) and are unaffected.
- **A redirect bool can't express "follow same-scheme only."** CAA `/front-500` *requires*
  following its redirect to the image host, but must not be downgraded http<->https — so
  neither `follow=false` nor default-follow matches baseline. Hence `RedirectPolicy`
  {FollowAll, FollowNone, FollowSameScheme}; FollowAll maps byte-identically to the old bool.
  (c) is where the interface legitimately grew — matching baseline, not gold-plating.
- **Plain-HTTP scheme-derived no-SECURE came due for CDRipper exactly as flagged in (b).**
  `urlIsSecureScheme()` is false for `http://accuraterip.com` / `http://db.cuetools.net`, so
  no `INTERNET_FLAG_SECURE` — correct, and the AR fetch verified byte-identical (Joan Osborne
  12/12 conf 200). The HTTPS (a)/(b) sites were unaffected — a no-op there, real change here.
- **"Keep the partial body on a mid-read error" was NOT a universal baseline.** All six
  one-shot sites used `while (InternetReadFile && got>0)` (error ⇒ keep partial, report ok),
  but `hlsHttpGet`'s baseline FAILED the call on a mid-body read error — a partial segment
  must not reach the decoder path. Found at implementation time, not in the survey; hence
  `HttpResponse::read_error` (slice 4): ok/body semantics unchanged for everyone, hls gates
  on the flag. When surveying a migration, diff the read *loops*, not just the flags.
- **Slice-4 accepted residuals (inert, documented so a future baseline-differ isn't
  surprised):** (1) on an HTTP ≥400 the seam reads the (small) error body before the
  consumer gates, where `hlsHttpGet`'s baseline bailed pre-read — no functional effect;
  (2) the seam reads in 4 KB chunks vs the audio sites' 8 KB — wire-invisible (TCP
  buffering), cancel granularity slightly finer; (3) scheme-derived `INTERNET_FLAG_SECURE`
  replaces hls's flag-less TLS-by-scheme and IHeart's hardcoded SECURE — identical
  outcomes, every URL's scheme accounted for. Cosmetic: the migrated failure logs lose
  their `GetLastError()` codes (the seam doesn't surface OS error codes), same spirit as
  group (c)'s collapsed CTDB log lines.
- **A keep-alive session's pooled connection outlives its fetches — localhost test
  fixtures must account for it.** The `http_cancel_test` server threads blocked in
  send/recv on the client's pooled connection, which only closes when the SESSION object
  is destroyed; joining the server thread before resetting the session deadlocked until
  WinINet's ~60s idle-close (110s test run). Fix: reset the session BEFORE joining fixture
  threads, and cap accepted-socket waits with SO_RCVTIMEO/SO_SNDTIMEO (2.6s run).
- **`close()` on a listening socket does NOT wake a thread blocked in `accept()`
  on Linux — `shutdown()` first.** Windows' closesocket aborts a blocked accept
  (WSAEINTR), and http_cancel_test's fixture relied on exactly that to stop its
  server threads; ported to Linux unchanged, scenarios B/C hung forever in
  server.join() (7/10 tests in, stuck minutes on a 2.5 s test — the log made the
  hang unambiguous). POSIX close() just drops the fd refcount; the sleeping
  accept keeps its reference. `shutdown(fd, SHUT_RDWR)` wakes it (accept returns
  EINVAL) — hence the shim's stopListener(): shutdown-then-close on POSIX,
  plain closesocket on Windows. Belt-and-braces: the test now carries a ctest
  TIMEOUT 60 so this hang class fails the matrix fast instead of eating the
  25-minute default. (Phase 3 slice 2, first Linux run of the portable test.)
- **A fake's observation state must outlive the object the consumer destroys.** The
  consumer discarding its channel/session on failure is often exactly the behavior under
  test (DiscordRP drops its channel on a CLOSE reject) — a fake that stores its captured
  data ON the discarded object hands the test a dangling pointer at the precise moment
  it asserts. `discord_ipc_test` first shipped this bug; fix: channel state lives in
  `shared_ptr`s owned by the Fake FACTORY, channels hold a reference. Twin of the
  http_cancel fixture lesson above — observation belongs to the fixture, not the wire.
- **One consumer with one obvious injection point ⇒ constructor injection, not another
  transitional global.** `core::http()`/`setHttp()` earned their existence: eight
  consumers scattered across seven modules. IIpc has ONE consumer (DiscordRP), so it
  takes `IIpc*` in its ctor (tests inject there; no `setIpc()` exists) and `core::ipc()`
  survives only as the link-time bridge to the platform TU — core code can't #include
  the impl header, so the production default must be reached by name. Pick the pattern
  per seam by consumer count, not by precedent.
- **git rename detection dies if you re-indent a moved body into a class.** Similarity
  is content-based; wrapping an extracted baseline in a class method (+4 columns on
  every line) plus a new header comment dropped Toast.cpp → NotifyWinToast.cpp below
  the 50% threshold — the `git mv` registered as delete+add, severing the
  injection-fix history the move was supposed to preserve. Fix: keep the extracted
  baseline at namespace/file scope at its ORIGINAL indentation (a file-static
  function) and make the interface class a thin adapter that calls it — held the
  slice-7 rename at 64%. Bonus: the frozen block's diff then shows exactly one
  changed identifier, which is the auditable-parity property we want anyway.
- **Probe the baseline call before migrating it — a "working" fire-and-forget ioctl
  may never have worked.** CDSource's "reset speed to max" sent a hand-rolled 6-byte
  struct where cdrom.sys expects the canonical (12-byte here) CDROM_SET_SPEED; a
  standalone probe showed it rejected with ERROR_BAD_LENGTH — a lifelong silent no-op
  (return ignored). A seam can't express "send these exact malformed bytes", and
  silently fixing it would CHANGE drive behavior on the playback path. Rule: probe →
  if it provably fails today, DROP the call (behavior-identical) and park the fix as
  its own decided improvement. Don't migrate garbage faithfully, and don't fix it as
  a ride-along. (slice 8; parked item in roadmap.md)
- **"Buffer size implies capability" is a platform shape — name capabilities as
  explicit flags in a seam.** Windows IOCTL_CDROM_RAW_READ returns C2 error bytes iff
  the output buffer is sized 2646/sector — the request is IMPLICIT in the buffer.
  SG_IO needs the C2 error-field bits set in the CDB; buffer size can't express it.
  Hence `readRaw(..., want_c2, ...)`: advisory on Windows (impl passes the buffer
  through untouched — byte-identical), load-bearing on Linux. Corollary: surface
  bytes-returned — the baseline's C2 probe and de-interleave branch both key on it;
  hiding it would force policy into the seam.
- **One impl can serve two consumers' differing open flags only after arguing each
  delta inert — and the argument goes in the diff.** CDSource opened its device
  SHARE_READ + SEQUENTIAL_SCAN; CDRipper SHARE_READ|WRITE + ATTRIBUTE_NORMAL. The
  seam uses the ripper's shape for both: share-mode widening is strictly more
  permissive (nothing opens a CD device for write), and SEQUENTIAL_SCAN is a
  ReadFile read-ahead hint — inert under DeviceIoControl-only access. Both residuals
  proven by the byte-identical rip gate + live playback. The concurrent-two-opens
  case (CDSource holds while CDRipper rips) is interface CONTRACT, not accident.
- **Preserve asymmetric timeout shapes; don't "clean them up" into a uniform API.**
  DiscordRP's baseline bounds the header wait (peek ≥8 bytes, 1000 ms — a wedged Discord
  must never hang the UI) but deliberately does NOT bound body reads. A tidier
  `recvExact(len, timeout)` seam would have either invented a body timeout (behavior
  change) or hidden the baseline's loop inside the impl (unauditable parity). The
  3-primitive channel (send/waitReadable/recvSome) keeps the consumer's loops verbatim —
  the read-loop twin of "diff the read loops, not just the flags."
