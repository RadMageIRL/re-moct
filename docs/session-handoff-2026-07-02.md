# Session handoff — 2026-07-02 (Phase 1 HTTP seam: groups a/b/c done, 6/8)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(The prior handoff `session-handoff-2026-07-01.md` covers Phase 0 + the
negative-offset bug and is still valid history.)

## Pinned constraints (do NOT re-litigate)
- **The 150-sector offset is a physical property of the disc** — a design aspect of
  AccurateRip, worked out across the HydrogenAudio forum posts at
  https://hydrogenaudio.org/index.php/topic,97603.0.html. Correct by design. Never
  "fix" the 150. (Also in `lessons.md`, the `ar_crc.h` comment, the negative-offset commit.)
- **HTTP request bodies go over the wire as raw UTF-8 bytes — never widen them.**
  `HttpSendRequest`'s payload is a byte buffer; `nlohmann::dump()` emits UTF-8. Widening
  corrupts non-ASCII (proven live: Björk/Jóga round-tripped clean on both scrobble services).
- **Parity for an HTTP migration lives at the call sites, not the seam.** A FakeHttp test
  proves request-building; only a live submission viewed on the service proves wire fidelity.
- **`core::http()` / `core::setHttp()` are TRANSITIONAL globals** — the endgame is
  dependency injection (host hands plugins an `IHttp`, per `architecture.md`). Don't enshrine.

## What this session accomplished — Phase 1 HTTP seam, groups a/b/c
The `core::IHttp` seam (`include/IHttp.h`) + `platform::win::WinInetHttp`
(`src/HttpWinInet.cpp`, `if(WIN32)`) now backs **6 of the 8** WinINet sites. All
migrations are behavior-preserving (parity matched site-by-site) and two-layer verified.

- **Group (a) — GET/JSON** (commit `94eb8cb`): MBLookup, RadioBrowser. Interface born
  (HttpRequest/HttpResponse, transitional `http()`). Verified: ctest 3/3 + live MB lookup /
  RadioBrowser search on 7of9.
- **Group (b) — POST scrobble** (code `7c13baf`, docs `b9d5987`): LastFm, ListenBrainz.
  Added the seam's non-GET path (`InternetCrackUrlW`→`InternetConnectW`→`HttpOpenRequestW`→
  `HttpSendRequestW`); **body sent as raw UTF-8 bytes**. POST path sets CONNECT+RECEIVE only
  (no SEND — matches baseline). Signing (md5 `api_sig`), body-building, and status gating stay
  consumer-side. Added transitional `core::setHttp()` + Windows-only `scrobble_request_test`.
  Both consumers WinINet-free (ListenBrainz fully portable). Verified: ctest 4/4 + live
  Björk/Jóga scrobble round-trip on Last.fm + ListenBrainz (accents intact).
- **Group (c) — bytes + redirect** (code `dd5f792`, docs `4fb213b`): CoverArt (`bytesByMbid`,
  `httpGet`) + CDRipper (AR `.bin`, CTDB). Both WinINet-free (CoverArt now portable). New
  interface capability, exercised for real:
  - `RedirectPolicy` enum {FollowAll, FollowNone, FollowSameScheme}. **FollowAll (default)
    maps byte-identically to the old `follow_redirects=true`** (no flag). FollowSameScheme =
    CAA cross-scheme downgrade protection (`IGNORE_REDIRECT_TO_HTTP|HTTPS`).
  - `finalizeBody()` **clears** the body on `reject_truncated`+truncated (CoverArt's 10 MB
    reject/clear — a valid-looking partial image must never be embedded). Pure, unit-tested.
  - `urlIsSecureScheme()` — plain-HTTP AR/CTDB get **no SECURE flag** (scheme-derived). Pure.
  - Per-call UAs preserved (CTDB short `RE-MOCT/1.0.0-rc1`; AR/CAA the full default UA).
  - Tests: pure `http_seam_test` extensions (byte passthrough, `final_url`, clear-on-reject,
    `urlIsSecureScheme`) + Windows-only `group_c_request_test`.
  - Verified: ctest 5/5 + real-world on 7of9 — Joan Osborne **12/12 AR v2 conf 200,
    byte-identical** through the seam's plain-HTTP AR fetch (`.bin` still saved to
    `.ar-db-info`); cover art resolves in tags + Discord RPC.
  - Cosmetic: CTDB's two distinct failure-log lines collapsed to the generic `HTTP: 0`
    block (result preserved: false/inconclusive).

## The fork (this is the decision for next session)
**6/8 migrated; the two remaining HTTP sites both touch the audio pipeline:**
- **StreamSource `hlsHttpGet`** (`src/StreamSource.cpp:159`) — one-shot manifest/segment GETs,
  BUT on the shared `KEEP_CONNECTION` session, on the audio-segment path, with a `stop_`
  check mid-read for prompt abort. Migrating to the current per-call seam would drop
  connection reuse and lose mid-segment abort (up to an 8 s hang on stop).
- **IHeart metadata** (`src/IHeartRadio.cpp:73`) — persistent `KEEP_CONNECTION` on the
  audio-poll thread.
Both need a **cancel token in the `IHttp` interface + persistent-session support** before
they can migrate safely — that is their own design, not a fold-in. The live StreamSource
audio read loop (`producerWorker` → `InternetReadFile(hConn_)` → SPSC ring, `src/StreamSource.cpp:992/1130`)
stays OUT of the seam entirely, always.

**Choose one:** (1) design the cancel-token + persistent-session extension next and finish
HTTP 8/8, or (2) declare HTTP "done enough" at 6/8 and move to another Phase 1 seam. The
straightforward HTTP work is closed either way.

## Rest of Phase 1 — still untouched
- **IPC:** Discord named-pipe → abstract interface (Linux = Unix socket).
- **Notifications:** Toast/PowerShell → abstract (Linux = libnotify/notify-send).
- **CD access:** Windows IOCTL → SG_IO on Linux.
- **The `src/core` + `src/platform/{win,linux}` directory reorg** — where `IHttp.h` moves to
  `include/core/` and `HttpWinInet.cpp` to `src/platform/win/` (use `git mv`; keep buildable).
- Parked concurrency debt (see `roadmap.md` Parked/deferred).

## Current state
- **Branch:** `restructure`, pushed to `origin/restructure` (tip `4fb213b`).
- **Tests:** `iheart_sm_test`, `ar_crc_test`, `http_seam_test`, `scrobble_request_test`,
  `group_c_request_test` — **5/5 green** via `ctest` (Windows; Linux CI would run the 3 pure).
- **Build:** clean, `remoct.exe` at `build\bin\remoct.exe`.
- **This `E:\code\remoct` clone has the full MSYS2 UCRT toolchain + deps** — full build +
  ctest run locally; only interactive/real-world checks (rip / scrobble / stream) need 7of9.

## Build / test commands (MSYS2 UCRT64)
```
cmake -S . -B build -G Ninja && cmake --build build          # build (also builds tests)
ctest --test-dir build --output-on-failure                    # run tests (build FIRST)
```
- Trust `ctest` output, not the main build's link-line count, as the test signal.
- Prefer the `cmake -S . -B build` form (pins source dir, forces reconfigure) for a fresh
  test target rather than an in-`build` invocation.

## Operating discipline for next session
- **Read `CLAUDE.md` + `roadmap.md` + `lessons.md` + `architecture.md` + THIS handoff FIRST**,
  before proposing anything. Verify against the actual code — a stated layout is a claim, not
  ground truth (`ls`/grep before asserting).
- **Design-first, no code until the boundary is confirmed.** Survey (verified, file:line),
  propose the seam/parity, flag the risks, then stop for sign-off.
- **Verify → build → ctest → real-world → commit.** Two-layer: green tests prove the logic,
  a real rip/scrobble/stream proves reality. No commit until the real-world gate passes.
- **Commit code and docs separately** (small, focused commits).
- **No `Co-Authored-By` trailer** on commits.

## Standing rule (going forward)
At the END of every working session, **update `roadmap.md` + `lessons.md` and refresh the
handoff doc as the last step before stopping** — so the durable docs are always the source of
truth and the next session never depends on session memory.
