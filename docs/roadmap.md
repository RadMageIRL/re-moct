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
      (AR/CTDB). See Done section. **`hlsHttpGet` was pulled OUT of (c)** and deferred —
      it was not a clean one-shot.
    - **slice 4 — the audio-thread pair (sites 7 & 8): ✅ DONE** (commit `4c72b09`):
      StreamSource `hlsHttpGet` + IHeart metadata, via the cancel-token +
      persistent-session (`IHttpSession`) extension. **HTTP consolidation is 8/8 —
      fully closed.** The fork is resolved: both sites migrated. See Done section.
      The live StreamSource audio read loop (`InternetReadFile`→ring) stays OUT
      entirely, permanently — proven byte-identical by diff in slice 4.
  - **slice 5 — core/platform directory reorg (HTTP seam): ✅ DONE** (commit
    `1977539`): `include/core/` + `src/platform/win/` established on the finished
    HTTP seam. See Done section. The remaining seams below BUILD INTO this
    structure (interfaces → `include/core/`, Windows impls → `src/platform/win/`);
    they are not relocated later.
  - **slice 6 — IPC: Discord named-pipe → `core::IIpc`: ✅ DONE** (commit `89285d8`):
    the first seam built INTO the slice-5 structure. See Done section. (Linux =
    Unix socket, Phase 3.)
  - **slice 7 — notifications: Toast/PowerShell → `core::INotify`: ✅ DONE** (commit
    `dde7041`): third seam built INTO the slice-5 structure; second ctor-injection
    consumer. See Done section. (Linux = libnotify/notify-send, Phase 3.)
  - **slice 8 — CD access: Windows IOCTL → `core::ICdIo`: ✅ DONE** (commit
    `14aebec`): the LAST platform seam — Phase 1's seam work is complete. See
    Done section. (Linux = SG_IO on /dev/srN, Phase 3.)
  - Clear the parked concurrency debt where cheap.
- **Phase 2 — internal Source interface.** A compile-time C++ abstract base
  (statically linked, NOT a DLL yet). **Design approved (2026-07-02):**
  `core::ISource` at `include/core/ISource.h` — readFrames (the audio-callback
  contract, fixed 44100/stereo/f32) / caps (seekable, finite, live) /
  positionSec / durationSec / seekTo / close. **`open()` and metadata are
  deliberately OUT of the v1 contract** (see Decisions log). Slices:
  - **slice 0 — thin replay net: ✅ DONE** (commit `7adae12`): the regression
    net for the whole phase — see Done section. These tests retarget through
    `ISource*` in slice A and gate every later slice.
  - **slice A — contract + push sources: ✅ DONE** (commit `0b7acf4`): ISource
    landed; StreamSource + CDSource implement it (signature harmonization only —
    concurrency boxes provably unopened: `src/StreamSource.cpp` diff EMPTY,
    sacred-symbol grep over the full diff zero hits); pipeline tests retarget
    through `ISource*`. See Done section.
  - **slice B — LocalFileSource extraction** (the heavy one: decoder_/
    next_decoder_ become source objects; the audio-thread crossfade swap becomes
    a pointer swap under the same release/acquire protocol). Gated by the slice-0
    xfade_handoff_test + the full file-mode listen gate.
  - **slice C — callback dispatch through ISource* (OPTIONAL, own go/no-go):**
    the mode flags stay regardless (they gate per-mode side logic that must not
    unify — file-only ReplayGain, CD-only live-BPM, per-branch buffering/
    track-end). A+B achieve Phase 2's goal; C is declinable.
- **Phase 3 — Linux port.** Forces the boundary clean. WSL2 as the fast inner loop,
  GitHub Actions matrix (Windows + Linux) as the source of truth.
- **Phase 4 — plugin-ize.** Harden the Source interface into a loadable C-ABI
  `.dll`/`.so` boundary, and **extract iHeart as the first real plugin** — the test
  of the whole plugin system. ("Fix iHeart and ship without rebuilding the host.")

## Done (restructure branch)
- **Phase 2 slice A — core::ISource + push sources: DONE** (commit `0b7acf4`).
  `include/core/ISource.h` (new, platform-free, standalone-compile verified):
  readFrames (audio-callback contract, fixed 44100/stereo/f32; ring-backed
  sources always return frame_count and signal end via their own state —
  pull-decoded sources short-read at EOF, slice B) / caps (seekable, finite,
  live — declared, never faked) / positionSec / durationSec / seekTo / close.
  open()/metadata/pause deliberately excluded (Decisions log). StreamSource
  implements it with **zero .cpp hunks** (positionSec widened int→double
  inline; caps=live; durationSec=0; seekTo=false); CDSource with three
  byte-equivalent signature harmonizations (seekTo(int)→bool seekTo(double),
  position/duration int→double — integer math preserved, widened) + caps=
  seekable+finite; CD's extended surface stays concrete. AudioManager: exactly
  4 consumer casts; UIManager + CDRipper compiled unchanged. The slice-0 net
  retargeted through `core::ISource&` — the fidelity/prebuffer assertions now
  also prove interface dispatch — plus new contract blocks per source. Audit:
  sacred-symbol grep over the diff = zero hits; virtual dispatch added only on
  UI-thread query paths (the audio callback still reads through concrete
  members — dispatch there is slice C, deferred). Build clean, ctest 13/13,
  repeated-run stable.
- **Phase 2 slice 0 — thin replay net: DONE** (commit `7adae12`). Three tests
  that lock down the producer/consumer audio-machinery SEMANTICS headlessly,
  through the seams Phase 1 built — the safety net decision #1 required before
  the Source-interface refactor touches the audio path (a diff audit protects
  code that moves unopened; these protect the seams the refactor deliberately
  changes). Zero production-code changes; ctest 13/13, stable across repeated
  runs. **This is Phase 2's regression net: slice A retargets these tests
  through `ISource*` and every subsequent slice gates on them.**
  - `cd_pipeline_test`: FakeCdIo through the REAL CDSource playback pipeline
    (playTrack → readerWorker → SPSC ring → readFrames). Byte-exact PCM fidelity
    across the thread boundary (never-zero pattern keyed on absolute disc
    position), seek flush/restart at target LBA, pause continuity, consumer-stall
    backpressure, transient-error silence-fill (exactly count×1176 zeros
    in-stream, LBA advanced), media-removal hard stop + latch, stopReader-leaves-
    device-open (the CDRipper handoff), playback-never-requests-C2.
  - `hls_pipeline_test`: FakeHttp via `core::setHttp()` through the REAL
    StreamSource HLS/AAC pipeline (open → hlsConnect → producerWorkerAAC →
    segment pump → FDK decode → ring → readFrames) — the Phase-0 Tier-2
    record/replay idea realized cheaply on the slice-4 seam. Synthetic manifests
    + REAL ADTS generated at test start by FDK-encoding a sine (no binary
    fixtures in the repo). Prebuffer, live-window stall → underrun → rebuffer
    self-heal, timed-ID3 (TIT2/TPE1) → nowPlaying, and the prompt-close cancel
    gate: close() during a wedged segment fetch returned in **16 ms** with the
    cancel token observed at the fake transport — the slice-4 "mid-segment stop
    <1 s" property now runs on every ctest, headlessly.
  - `xfade_handoff_test`: REAL AudioManager on generated WAVs through a real
    MUTED device — the swap-adjacent file path slice B restructures: gapless EOF
    short-read → audio-thread swap → track_swap_flag_ → pollEvents install
    ordering (swap lands before the track-end callback fires), crossfade
    trigger/two-decoder mix/isCrossfading visibility/completion (a preload
    event, NOT a track_end), clearNext mid-crossfade (the device-stop quiesce
    branch), end-without-preload. Needs audio hardware: SKIPs (exit 77) when no
    device opens, so the cd/hls tests remain the always-on net.
  - **Honest limits:** the ICY/continuous path (rawRead → InternetReadFile, raw
    WinINet by design) cannot be replayed headlessly — live gates only. Real-time
    callback cadence, device bring-up ordering, and real ad-pod re-pin timing
    also stay with the 7of9 live gates.
- **Phase 1 slice 8 — CD-I/O seam (Windows CD IOCTLs → core::ICdIo): DONE**
  (commit `14aebec`). The LAST platform seam, BUILT INTO the slice-5 boundary:
  `include/core/ICdIo.h` + `src/platform/win/CdIoWin.cpp` (`platform::win::WinCdIo`
  / `WinCdDevice`). Interface = raw optical-drive transport, 8 primitives modeled on
  the two real consumers: factory `open(drive-spec)` → device object (dtor closes;
  concurrent opens of one drive are CONTRACT — CDSource holds its device for playback
  while CDRipper opens a second per rip), `readToc` (raw MSF handed over — MSF→LBA
  math and the "do NOT subtract 150" contract stay consumer-side),
  `lastSessionFirstTrack` (the one value the Enhanced-CD detect consumes), `readRaw`
  (lba/sectors/want_c2/buffer/got), `setSpeed`, `mediaPresent`, `model`. Every impl
  method is ONE baseline DeviceIoControl moved parameter-identical (the lba*2048
  DiskOffset quirk now impl-side). `want_c2` is an EXPLICIT flag — advisory on
  Windows (buffer size IS the request, the baseline shape) but it's what SG_IO's CDB
  error-field bits need at Phase 3; buffer-size-implies-C2 was the one
  Windows-only assumption in the baseline and it's gone. Bytes-returned (`got`) is
  surfaced — the C2 probe (`got == 2646`) and the de-interleave branch depend on it.
  ALL rip/disc logic stayed consumer-side (the IHttp/IIpc/INotify split): AR CRC
  math, 150-sector preamble, normalizeSkip/arPreambleReadable, disc-ID
  normalization, frame450 sweep, Enhanced-CD silence search + +174, retry/
  silence-fill, cache-flush, C2 interpretation, drive-offset table.
  **Ownership: ctor injection per consumer** (the slice-6/7 pattern, now ×2):
  `CDSource(ICdIo* = nullptr)` + `CDRipper(ICdIo* = nullptr)` defaulting to
  `core::cdio()` (link-time bridge only, no setCdio()). Header detox rode along as
  designed: CDSource.h no longer includes windows.h/ntddcdrm.h (AudioManager/
  UIManager stop inheriting Windows from this path); CDTrack + MBLookup TOC params
  DWORD→uint32_t, type-only. CDSource's hand-rolled 6-byte SET_SPEED was DROPPED,
  not migrated — probe-confirmed rejected by cdrom.sys with ERROR_BAD_LENGTH, a
  lifelong silent no-op (see parked item). Unified open flags (ripper's shape;
  CDSource residuals inert and documented in lessons.md). New `cd_toc_test`
  (10th ctest target): FakeCdIo via ctor injection through the REAL CDSource —
  Relish-shaped non-standard pregap (T1 @ LBA 182), Enhanced-CD data track +
  session-2 leadout, malformed-TOC clamps, model→offset lookup, media checks.
  **Two-layer verified: ctest 10/10 + THE hardware gate** — Joan Osborne Relish
  ripped through the seam on the GHD3N: 12/12 AR v2 conf 200, per-track
  crc_v1/crc_v2, all 12 pcm_crc32, frame450 global/local, the T1 first-16-samples
  dump, and "C2 support: no" ALL byte-identical to the pre-migration baseline log
  (diff-verified line-by-line); .bin cache saved; CD playback + seek through the
  seam confirmed, with the two-concurrent-opens contract exercised live.
- **Phase 1 slice 7 — notifications seam (Toast/PowerShell → core::INotify): DONE**
  (commit `dde7041`). Third seam BUILT INTO the slice-5 boundary:
  `include/core/INotify.h` + `src/platform/win/NotifyWinToast.cpp`
  (`platform::win::WinToastNotify`; `git mv` from src/Toast.cpp — rename held at 64%,
  injection-fix history preserved). Interface = "show the user a transient
  notification": ONE primitive, `notify(title, body)` — two strings, no icon/duration
  params (the baseline never set either; ToastImageAndText02's image slot was never
  populated). Contract is the baseline's: fire-and-forget, non-blocking (the win impl
  spawns PowerShell from a detached thread, 5 s bounded reap, CREATE_NO_WINDOW),
  best-effort with NO error reporting (baseline swallowed CreateProcess failure),
  callable from any thread (impl stateless per call). CONTENT stayed consumer-side —
  the IHttp/IIpc protocol/transport split: Toast.h is now a header-only, platform-free
  content adapter, `showTrackToast(title, artist, album, INotify&)`, keeping the
  baseline mapping verbatim (artist PREPENDS the title: "Artist - Title"; empty album
  → "RE-MOCT" body) including its status-shape inversion (("Streaming", station, "")
  → "station - Streaming" — baseline, not fixed). The escape/UTF-16LE/base64
  `-EncodedCommand`/CreateProcess transport block moved byte-identical (one identifier
  rename, `esc(top)`→`esc(title)`; emitted bytes unchanged) — that block fixed a real
  quote-injection bug and is frozen. The `'RE-MOCT'` CreateToastNotifier id stays
  impl-side (platform attribution — the Linux twin's `notify-send -a`).
  **Ownership: constructor injection again** — UIManager is the ONE consumer class
  (33 call sites, all in UIManager.cpp): `UIManager(..., core::INotify* = nullptr)`
  defaulting to `core::notifier()` (link-time bridge only, no `setNotify()`; named
  notifier() to avoid the notify().notify() stutter). A private 3-arg member wrapper
  hides the 4-arg adapter, so all 33 call sites compiled textually unchanged. New
  `notify_toast_test` (9th ctest target, portable — no PowerShell/process spawn):
  FakeNotify through the REAL Toast.h adapter, 8 cases (mapping branches, the
  dominant (msg,"","") status shape, the Streaming inversion, adapter-must-NOT-escape
  — escaping is the transport's job — and call ordering). Two-layer verified:
  ctest 9/9 + live gate on this machine — toasts fire from the real triggers (track
  change, status toggles), quote-injection ('/") and accented-metadata (Björk) probes
  confirmed in Action Center, synchronous script run exit 0 (Show() succeeded).
- **Phase 1 slice 6 — IPC seam (Discord named-pipe → core::IIpc): DONE** (commit
  `89285d8`). First seam BUILT INTO the slice-5 boundary: `include/core/IIpc.h` +
  `src/platform/win/IpcWinPipe.cpp` (`platform::win::WinPipeIpc`). Interface =
  "local bidirectional byte channel", three primitives modeled on the one real
  consumer: `send` (whole buffer, ONE OS write — Discord's framing breaks on split
  writes), `waitReadable(min_bytes, timeout_ms)` (bounded peek-poll, 10 ms baseline
  granularity preserved in the impl), `recvSome` (one blocking OS read; callers
  loop). `IIpc::connect(logical-name)` — the impl owns name→path mapping
  (`\\.\pipe\<name>`; Linux `$XDG_RUNTIME_DIR/<name>` at Phase 3). The asymmetric
  read shape is deliberate parity: bounded header wait, UNbounded body reads —
  a higher-level `recvExact(timeout)` would have invented semantics the baseline
  doesn't have. Protocol stayed consumer-side in DiscordRP (framing, the
  discord-ipc-0..9 probe, handshake, 1 MB squatter cap, CLOSE handling, lazy
  reconnect, payload/nonce/escape) — the IHttp protocol/transport split.
  **Ownership: constructor injection, NOT a second transitional global** —
  `DiscordRP(app_id, core::IIpc* = nullptr)` defaulting to `core::ipc()` (declared
  in core, defined in the impl TU — link-time bridge only, no `setIpc()`); one
  consumer with one obvious injection point didn't justify the http()/setHttp()
  pattern, and the ctor param IS the DI endgame shape. DiscordRP.cpp is
  pipe-API-free (`windows.h` only for `GetCurrentProcessId`, the IHeartRadio
  shape); DiscordRP.h no longer includes `windows.h` at all (UIManager.h stops
  inheriting it from this path). New `discord_ipc_test` (8th ctest target):
  FakeIpc via ctor injection asserts handshake bytes, probe order, SET_ACTIVITY
  framing + jsonEscape, activity-null clear, CLOSE→reject, and
  reconnect-after-death. Two-layer verified: ctest 8/8 + live Discord on this
  machine — RP with title/artist/art, track-change update, Discord-restart
  reconnect, clear-on-toggle (all four gates). `src/platform/linux/` still
  deliberately absent.
- **Phase 1 slice 5 — core/platform directory reorg (HTTP seam only): DONE** (commit
  `1977539`). Pure relocation — `git mv include/IHttp.h include/core/IHttp.h` +
  `git mv src/HttpWinInet.cpp src/platform/win/HttpWinInet.cpp` (both true renames,
  95%/98% similarity — history preserved). Include statements changed to
  `#include "core/IHttp.h"` across all 15 referencing sites (2 headers, 7 src
  consumers, the impl, 5 tests) — chosen over adding `include/core` to the search
  path so the layer boundary is visible at every include site and future core
  headers follow the same pattern with zero extra CMake. CMake: main target + 4
  test targets' `HttpWinInet.cpp` paths updated; no include-dir changes (`include/`
  already on every target's path). Diff surface: 18 files, 29+/29− — provably
  move-plus-paths only. Verified: clean reconfigure+rebuild (rm -rf build) 50/50,
  ctest 7/7 same results, remoct.exe launches and plays a local file.
  **Deliberately scoped to the one finished seam** — reorg-first on known-good code
  establishes the boundary once; IPC / notifications / CD-IOCTL get built into it,
  not relocated later. `src/platform/linux/` NOT created — it's the libcurl (+ Unix
  socket, libnotify, SG_IO) home when Phase 3 arrives.
- **Phase 1 slice 4 — HTTP seam, sites 7 & 8 (the audio-thread pair): DONE** (commit
  `4c72b09`). **HTTP consolidation 8/8 — fully closed.** `core::IHttp` gained the two
  capabilities the audio sites needed, both defaulting inert (six shipped sites
  byte-identical):
  - **Cancellation:** `HttpRequest::cancel` (`const std::atomic<bool>*`, polled before
    open + before each chunk read — baseline `stop_` granularity) + `HttpResponse::
    cancelled` (ok=false, partial body CLEARED via pure `finalizeCancelled()`), so a
    consumer's own stop is never mistaken for network death (reconnect/backoff safe).
  - **Persistent sessions:** `IHttpSession` + `IHttp::openSession(HttpSessionConfig)`
    with a default forwarding impl (fakes/tests unchanged); `WinInetSession` holds one
    `InternetOpen` handle (UA + timeouts at creation) and adds KEEP_CONNECTION per
    fetch. Session lifetime is the caller's: hls dies in `disconnect()` (fresh per
    re-handshake/re-pin), IHeart lives with the object — exact baseline lifetimes.
  - Also: `pragma_no_cache` request field (both audio baselines sent it; default off)
    and `read_error` response flag — an implementation-time survey correction (hls
    FAILS on a mid-body read error, unlike the other six; a partial segment must not
    reach the decoder). Accepted residuals recorded in `lessons.md`.
  - StreamSource: only `disconnect`/`hlsEnsureSession`/`hlsHttpGet` touched (3 diff
    hunks); `InternetReadFile` remains ONLY in `rawRead` (live loop), `InternetOpenUrl`
    only in `connect()`'s ICY branch. IHeartRadio.cpp is now **WinINet-free** (nullptr
    cancel — boundedness stays the deliberate 5 s timeout; wiring `stop_` into its
    polls is a parked, separate decision).
  - Tests 7/7: `http_seam_test` +4 pure cases; new `http_cancel_test` (localhost
    socket fixture — mid-body abort 373 ms, keep-alive reuse = 1 connection observed,
    pre-cancelled = zero network); new `iheart_request_test` (real consumer + injected
    fake, all three endpoints). Two-layer verified: real-world on 7of9 — digital iHeart
    plays clean, **mid-segment stop aborts <1 s** (the regression this design must not
    introduce), stop-during-switch clean, now-playing + Discord art intact, ad-onset
    re-pin a non-event, ICY/SHOUTcast behaviorally unregressed.
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
    `include/core/` and `HttpWinInet.cpp` to `src/platform/win/`. (Done — slice 5.)
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
- **Adopt canonical SET_SPEED in CDSource::open (behavior improvement):** the
  baseline's "reset speed to max after open" never worked — its hand-rolled 6-byte
  struct is rejected by cdrom.sys with ERROR_BAD_LENGTH (probe-confirmed, slice 8),
  so the call was dropped for parity. Actually resetting (`dev_->setSpeed(0xFFFF)`
  at the dropped site) would make playback-mode spin-up real for the first time —
  take it only as its own decided change with a playback listen test.
- **Latent baseline OOB on corrupt multi-session TOCs:** CDSource's session-2 block
  indexes `entries[toc2.last]` unclamped (baseline did the same with TrackData) — a
  corrupt disc reporting last>99 over-reads the array. Preserved as-is in slice 8
  (no ride-along logic); fix alongside the session-2 guards as its own change.
- **Wire `stop_` into IHeart's metadata polls as a cancel token:** would cut worst-case
  `close()` latency by ~5 s. Deliberately NOT done in slice 4 (a behavior improvement,
  not parity) — take it only as its own decided change.
- **Prompt interrupt of a fully stalled connect/read** (cross-thread handle-close):
  baseline gap that slice 4 intentionally preserved (parity); the existing note in
  `StreamSource::close()` still stands. Future hardening item.
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
- **Phase 2 ISource contract (approved 2026-07-02): model the real four, don't
  force uniformity.** `core::ISource` = readFrames/caps/positionSec/durationSec/
  seekTo/close at `include/core/ISource.h` (first core-owned non-platform
  contract; Phase 3 sources and Phase 4 plugins implement the same interface).
  What made a uniform frame contract honest: all paths already converged on
  fixed 44100/stereo/f32, and StreamSource/CDSource::readFrames are deliberately
  identical. Deliberate EXCLUSIONS: **`open()` stays out** (three irreconcilable
  open shapes — file path, stream URL + async-connect orchestration that must
  stay in AudioManager, CD drive+track container; a uniform open(string) would
  push connect orchestration into sources or fake a CD URL scheme — Phase 4's
  problem, the ABI-leak to resist). **Metadata stays out** (least-uniform facet:
  file = full TagLib upfront, CD = TOC + external MB lookup, stream = time-
  varying nowPlaying/art; the unified-metadata trap is where a Phase-4 shape
  sneaks in — grow later per the RedirectPolicy precedent if a real consumer
  needs it). CD's extended surface (TOC/offset/model/stopReader/checkMedia,
  consumed by CDRipper + UIManager) stays concrete beside the interface.
  **Standing rule: if any of the four sources needs pretending to fit, that's
  stop-and-redesign, not push-through.** Slice C (callback dispatch) is optional
  and declinable — the per-mode callback branches gate logic that must not unify.
- **Slice 0 net-first (decision #1, 2026-07-02): thin replay net before the
  refactor, not the full capture harness and not bare diff audits.** Diff audits
  protect what moves unopened (slices 4/8 proved that); the new timing risk is at
  the seams the refactor deliberately changes (callback dispatch, teardown
  ordering, crossfade swap). The Phase-1 seams (setHttp/FakeCdIo) made the net a
  1-session build vs 3-5 for a full instrumented capture harness.
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
