# RE-MOCT roadmap

## Direction (decided)
Linux-first to establish a clean core/platform boundary, **then** plugin-ability,
with **iHeart extracted as the first real loadable plugin** as the proof of the
plugin system. Porting forces the boundary clean; an interface proven across two
platforms is more trustworthy. Get there in two moves: prove an internal
compile-time Source interface first, **then** harden it into a C-ABI DLL/SO. Don't
carve the ABI first.

> **Spotify is dropped for now.** It was the thing that tempted us to pull plugins
> ahead of the Linux port; without it, the original ordering is cleaner. The
> Source-interface design stays transport-agnostic (see `architecture.md`) so a
> future sidecar-style service plugin remains possible, but it is not on the plan.

## Phases (in order)
- **Phase 0 — offline unit-test safety net: ✅ DONE (in the form pursued).** The
  pure-unit half landed and is verified: `IHeartNowPlayingSM` (now-playing decision
  logic — threadless, no network) and `ar_crc` (AccurateRip CRC math) extracted as pure
  units with offline `ctest` coverage (`iheart_sm_test`, `ar_crc_test`). **Honest scope:
  the originally-envisioned full streaming record/replay harness was NOT built** — the
  safety net took the form of pure extractions + offline unit tests, which proved
  sufficient to make the Phase 1 seam work safe. A true capture/replay harness remains
  available to build later if a future refactor needs it.
- **Phase 1 — platform abstraction / cleanup.** Replace platform-specific calls with
  seams (interfaces), not `#ifdef`s:
  - HTTP: consolidate the per-module WinINet GET helpers behind one `core::IHttp`
    seam (Linux = libcurl later; later still, host provides transport to plugins).
    Migrated in groups, split by verification method:
    - **slice 1 / group (a) — GET/JSON: ✅ DONE** (commit `94eb8cb`): MBLookup +
      RadioBrowser. See Done section.
    - **group (b) — POST scrobble: ✅ DONE** (commit `7c13baf`): LastFm + ListenBrainz.
      See Done section.
    - **group (c) — bytes + redirect: ✅ DONE** (commit `dd5f792`): CoverArt + CDRipper
      (AR/CTDB). See Done section. **`hlsHttpGet` was pulled OUT of (c)** and deferred
      (below) — it is not a clean one-shot.
    - **Deferred audio-thread go/no-go items** (the two remaining HTTP sites; each needs a
      cancel token + persistent-session design before it can be touched):
      - StreamSource `hlsHttpGet` — one-shot manifest/segment GETs, but on the shared
        `KEEP_CONNECTION` session and the audio-segment path, with `stop_`-abort mid-read.
      - IHeart metadata — persistent `KEEP_CONNECTION` on the audio-poll thread.
      The live StreamSource audio read loop (`InternetReadFile`→ring) stays OUT entirely.
  - IPC: Discord named-pipe → abstract (Linux = Unix socket).
  - Notifications: Toast/PowerShell → abstract (Linux = libnotify/notify-send).
  - CD access: Windows IOCTL → SG_IO on Linux.
  - Clear the parked concurrency debt where cheap.
- **Phase 2 — internal Source interface.** A compile-time C++ abstract base
  (statically linked, NOT a DLL yet): `open / readFrames / seek? / metadata /
  capabilities / close`. Refactor the EXISTING sources (local file, iHeart, ICY,
  CD) to implement it. Prove it against what already exists, under the Phase 0
  harness, with zero new dependencies.
- **Phase 3 — Linux port.** Forces the boundary clean. WSL2 as the fast inner loop,
  GitHub Actions matrix (Windows + Linux) as the source of truth.
- **Phase 4 — plugin-ize.** Harden the Source interface into a loadable C-ABI
  `.dll`/`.so` boundary, and **extract iHeart as the first real plugin** — the test
  of the whole plugin system. ("Fix iHeart and ship without rebuilding the host.")

## Done (restructure branch)
- **Phase 1 slice 3 — HTTP seam, group (c): DONE** (commit `dd5f792`, pushed). CoverArt
  (`bytesByMbid`, `httpGet`) + CDRipper (AR `.bin`, CTDB) migrated to `core::http().fetch()`;
  **both files WinINet-free** (CoverArt now portable — no `windows.h`). New interface
  capability exercised for real: `RedirectPolicy` enum (FollowAll default = byte-identical to
  the old `follow_redirects=true`; **FollowSameScheme** for CAA cross-scheme-downgrade
  protection); `finalizeBody()` **clears** the body on `reject_truncated`+truncated (CoverArt's
  10 MB reject/clear); plain-HTTP AR/CTDB get **no SECURE flag** (scheme-derived via
  `urlIsSecureScheme`). Per-call UAs preserved (CTDB short, AR/CAA default). Tests: pure
  extensions (byte passthrough, `final_url`, clear-on-reject, `urlIsSecureScheme`) + Windows-only
  `group_c_request_test`. Two-layer verified: `ctest` 5/5 + real-world on 7of9 (Joan Osborne
  12/12 AR v2 conf 200 byte-identical through the seam's plain-HTTP AR fetch, `.bin` still
  saved; cover art resolves in tags + Discord RPC).
  - **HTTP consolidation now 6/8 sites migrated** (a: MBLookup, RadioBrowser; b: LastFm,
    ListenBrainz; c: CoverArt, CDRipper) — this closes the *straightforward* HTTP work. The
    remaining two (`hlsHttpGet`, IHeart metadata) both touch the audio pipeline and are
    deferred go/no-go items needing a cancel token / persistent-session design.
  - Minor: CTDB's two distinct failure log lines collapsed to the generic `HTTP: 0` block
    (result preserved: false/inconclusive) — cosmetic.
- **Phase 1 slice 2 — HTTP seam, group (b): DONE** (commit `7c13baf`, pushed). LastFm +
  ListenBrainz GET/POST helpers migrated to `core::http().fetch()`; **both files now
  WinINet-free** (ListenBrainz has no `windows.h` at all — portable). Added the seam's
  **non-GET path** (`InternetCrackUrlW`→`InternetConnectW`→`HttpOpenRequestW`→
  `HttpSendRequestW`); the request **body is sent as raw UTF-8 bytes, never widened**.
  Signing (md5 `api_sig`), body-building, and status gating (`accepted()` on HTTP 200)
  stay consumer-side. POST path sets CONNECT+RECEIVE only (no SEND — matches the scrobble
  baseline). Interface unchanged — the slice-1 `body`/`content_type`/`headers` fields
  covered it. Added transitional `core::setHttp()` injection hook + Windows-only
  `scrobble_request_test`. Two-layer verified: `ctest` 4/4 + real-world Björk/Jóga scrobble
  round-trip on Last.fm + ListenBrainz (accents intact — raw-UTF-8 wire path confirmed;
  ListenBrainz status gate exercised on a live submit).
  - Remaining HTTP: group (c) — CoverArt, CDRipper (AR/CTDB, plain-HTTP), StreamSource
    `hlsHttpGet` (bytes + `final_url`); IHeart metadata still its own deferred go/no-go.
- **Phase 1 slice 1 — HTTP seam, group (a): FIXED/DONE** (commit `94eb8cb`, pushed).
  `core::IHttp` interface (`include/IHttp.h`, portable — no `windows.h`) +
  `platform::win::WinInetHttp` impl (`src/HttpWinInet.cpp`, `if(WIN32)`). MBLookup +
  RadioBrowser GET helpers now call `core::http().fetch()`. Parity verified vs baseline:
  4 MB cap both, RadioBrowser 6 s timeout, MBLookup no timeout, default UA byte-identical,
  HTTP status ignored, WinINet default redirect-follow. FakeHttp-backed `http_seam_test`
  added (real parse shapes: RadioBrowser `url_resolved`→`url` fallback, MB nested
  `artist-credit`). Two-layer verified: `ctest` 3/3 + real-world on 7of9 (Relish MB lookup
  resolved metadata; RadioBrowser "kwin" search returned KWIN 97.7).
  - `core::http()` is a TRANSITIONAL global (endgame = injection; see `architecture.md`).
  - At the later `src/core` + `src/platform/{win,linux}` reorg, `IHttp.h` moves to
    `include/core/` and `HttpWinInet.cpp` to `src/platform/win/`.
  - Remaining HTTP work (groups b, c, IHeart) tracked under Phase 1 above.
- **CDRipper negative drive-offset preamble OOB read + wrong CRC phase: FIXED**
  (commit `1a2235a`). Floored offset decomposition (`ar::normalizeSkip`) + signed
  preamble guard (`ar::arPreambleReadable`) extracted into `ar_crc.h` and wired into
  `CDRipper::ripTrack`; the 150-sector preamble is untouched. Covered by `ar_crc_test`
  (invariants, OOB-index witness, phase/CRC equivalence on a synthetic disc,
  boundaries); the +6 path is unregressed (Joan Osborne re-rip 12/12 AR v2 conf 200,
  byte-identical CRCs).
  - Open: real negative-offset **hardware** validation remains a HydrogenAudio
    cross-check item — the synthetic suite proves the logic, not on-drive behavior.

## Parked / deferred (not disturbed)
- **Track Info album tag decode:** an album field with accented chars (`é`) renders as
  `??` in the Track Info panel (same neighborhood as the earlier `¿` on the 4 Non Blondes
  filename) — a local tag-read/display decode artifact. **NOT in the scrobble path** (album
  isn't scrobbled to Recent Tracks; artist/title were clean over the wire — confirmed in
  group (b)). A separate local tag-decode thread to look at later.
- iHeart rabbit-hole desync: needs a 15–20 min instrumented ad-block capture (see
  `streaming.md`).
- Scrobble back-to-back duplicate: source commits from debounced `IHNow` + TTL dedup.
- StreamSource `isOpen()` race (consumed in staging lane, defer); `driveOffsetKnown()`
  always-false (skip, no payoff); AAC re-pin doesn't reset FDK (defer, ADTS resyncs).
- ICY/SHOUTcast metadata improvement (structural protocol limit); ICY ingest
  sanitization (station-ID stripping).
- Comb-filter / harmonic-sum tempogram for BPM (shelved).
- Faster manifest polling + staggered-peek machinery (pending rabbit-hole capture).

## Decisions log
- **300ms post-track-change seek lockout: REJECTED.** Silent dropped seeks in every
  track-change window were worse than the rare benign held-key leak they prevented.
  The track-stamp guard (discard a buffered seek if the track changed) was kept — it's
  costless and prevents the genuinely-wrong case with no deadzone.
- **Crossfade head-completion: UI-only.** Read the existing `crossfading_` atomic via
  `isCrossfading()` and let the comet head ride to 100% while crossfading; releases
  cleanly when the swap clears the flag. No audio-thread changes. Awesome-mode only;
  Classic keeps MOC's snap.
- **Don't carve the ABI first.** Internal compile-time interface → prove → then DLL.
- **Probe-first** for any new parsing/protocol work.
