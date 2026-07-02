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
- **Preserve asymmetric timeout shapes; don't "clean them up" into a uniform API.**
  DiscordRP's baseline bounds the header wait (peek ≥8 bytes, 1000 ms — a wedged Discord
  must never hang the UI) but deliberately does NOT bound body reads. A tidier
  `recvExact(len, timeout)` seam would have either invented a body timeout (behavior
  change) or hidden the baseline's loop inside the impl (unauditable parity). The
  3-primitive channel (send/waitReadable/recvSome) keeps the consumer's loops verbatim —
  the read-loop twin of "diff the read loops, not just the flags."
