# Session handoff — 2026-07-03 (rev 3: Phase 2 closed + cleanup pass done; NEXT SESSION = PHASE 3 KICKOFF, Linux port)

Read this at the start of the next session to pick up cleanly. Pairs with
`CLAUDE.md`, `roadmap.md`, `lessons.md`, `architecture.md`, `streaming.md`.
(Prior history: `session-handoff-2026-07-02.md` — Phase 1 completion + the
Phase 2 kickoff/slice 0/slice A, all still valid; this file adds slice B and
the current fork.)

## Pinned constraints (do NOT re-litigate)
- **The 150-sector offset is a physical property of the disc** — AccurateRip's
  design, per the HydrogenAudio forum posts at
  https://hydrogenaudio.org/index.php/topic,97603.0.html. Never "fix" the 150.
  AR math / disc-ID normalization (T1-LBA-relative) untouched.
- **The live StreamSource audio read loop stays OUT of any seam, permanently**
  (`InternetReadFile` only in `rawRead`; `InternetOpenUrl` only in connect()'s
  ICY branch). Producer/ring machinery (`ringClear`, `prebuffered_`, re-pin) is
  sacred — zero-diff invariant in every Phase 2 slice, held through A and B.
- **`next_decoder_initialised_` is the load-bearing swap protocol** — slice B
  changed its PAYLOAD (struct → pointer) with the atomic itself appearing in
  the diff only as unchanged context. Its name/orderings/positions stay.
- **The retirement scheme is load-bearing** (slice B): the audio thread parks
  the outgoing LocalFileSource in `retired_src_` and NEVER frees; pollEvents
  reaps under the `track_swap_flag_` synchronizes-with edge. Do not "simplify"
  the reap away — it closes latent race F2 (UI-thread positionSec vs the swap).
- Ctor DI per single-consumer seam; `core::http()/setHttp()` the one
  transitional global; the frozen NotifyWinToast `-EncodedCommand` block; CD
  seam audit invariants — all as per the 07-02 handoff.

## What this session (07-02 → 07-03) accomplished
- **Slice A** (`0b7acf4` + docs `6d06a0e`, live gate `3d4e061`): `core::ISource`
  at `include/core/ISource.h` (readFrames/caps/positionSec/durationSec/seekTo/
  close; open()/metadata/pause deliberately excluded — roadmap Decisions log);
  StreamSource (zero .cpp hunks) + CDSource implement it; pipeline tests
  retargeted through `ISource&`; live gate: file + live iHeart digital + CD
  seek all green.
- **Slice B** (`845a155` + this docs commit): LocalFileSource extraction — see
  roadmap Done entry. The crux: the audio-thread struct-copy swap became a
  pointer move + retirement under the identical release/acquire protocol;
  correctness argument ratified BEFORE code; audio-thread diff = exactly three
  enumerated change classes. Verified: ctest 13/13 (xfade +S5, test-first),
  headless mechanics gate ALL PASS (incl. 88.9 M positionSec reads hammered
  through a live crossfade swap, varispeed 1.50× measured, device-switch
  continuity, 11 h .m4b), live listen gate passed ("works as previous").
- **VBR live-bitrate readout investigated and parked** (Dos-spotted): forward
  seek on a VBR MP3 → huge one-window kbps spike, settles slightly above
  nominal. A/B probe on baseline `3d4e061` reproduced it identically —
  pre-existing estimator behavior (linear file-position model), slice B
  exonerated. Parked in roadmap as a product decision (what should the readout
  mean); do NOT fix as a ride-along.
- `docs/AccurateRipPipe.md` (Dos's Mermaid AR-flow diagram) tracked with this
  docs commit — verified consistent with the pinned 150-as-physical-preamble
  framing ("150 sectors before track start", disc-absolute mul_by anchoring).

## Current state
- **Branch:** `restructure`. Slice B = code `845a155` + this docs commit —
  LOCAL until Dos says push. Origin in sync through slice A's `3d4e061`.
- **Tests: 13/13** (xfade_handoff_test now has S1–S5 incl. replace-preload).
- **Layout:** `include/core/{IHttp,IIpc,INotify,ICdIo,ISource}.h`;
  `include/{LocalFileSource,TrackInfo}.h` + `src/LocalFileSource.cpp` beside
  StreamSource/CDSource; platform impls in `src/platform/win/`.
- **Phase 2 status: A+B achieve the phase's goal** — all three real source
  families implement `core::ISource`, proven against what existed.

## PHASE 2 CLOSED (rev-2 delta)
**Slice C assessed via written go/no-go and DECLINED (Dos confirmed).** The
decisive reason (§1c of the assessment): a single active-`ISource*` is
factually WRONG for the file branch — it runs TWO live sources during a
crossfade (varispeed reads the file source; the mix and gapless fill read
`next_src_`), so only 3 of the 6 callback readFrames sites could use it — the
hlsHttpGet lesson at callback scale. Supporting reasons: a second source of
truth for the playing mode (can disagree with the mode flags mid-transition —
the teardown-ordering class slice 0 guards); a fourth audio-thread change
class (pointer staleness at every swap — the F2 lesson replayed); the vtable
cost itself is unmeasurable and was NOT the argument; and nothing downstream
needs it (Phase 3 recompiles the portable core; Phase 4's plugin dispatch is
its own boundary — revisit there if its registry generates a concrete need).
Full entry in roadmap.md's Decisions log. **Phase 2's goal is achieved: all
four real sources (file, iHeart, ICY, CD) behind `core::ISource`, proven under
the slice-0 net + live gates, zero new dependencies.** Docs-only close-out;
nothing in the tree changed, which is the point.

## Cleanup pass (rev-3 delta) — three briefs, three closures, all pushed
1. **VBR bitrate readout: FIXED** (`adacbb1`/`4cdfa88`) — the "live" estimator
   was a linear position model (constant-derivative average, never
   instantaneous); now `bitrateKbps()` = TagLib nominal for everything, gate
   probe pinned VBR at 264 across a seek (was peak 90k).
2. **Canonical SET_SPEED: ADOPTED** (`d2ad038`/`0b0ddb2`) — CDSource::open
   really resets to max (0xFFFF) via the seam for the first time; contract in
   cd_toc_test; live gate on drive G clean; 1764 quiet-mid is a NAMED FALLBACK
   only if max ever proves audibly objectionable.
3. **iHeart rabbit-hole desync: CLOSED WITH EVIDENCE** (`2913a1b`, analysis
   only) — Stage-B promotion REJECTED (0/8 lane-music-first; SSAI stitches new
   joiners into ad pods too), faster-polling/staggered-peek rejected same
   premise, edgeLag flat, 35 s stall validated 68/68. Hole classes: A =
   broadcast-side (OOB shows no song, unfixable), B = session-side stale slate
   with OOB-proven live music. **Class-B OOB-gated re-pin is PARKED, NOT
   greenlit** (own design-first pass + live gate when chosen); Stage-A scaffold
   retirement is its own small future cleanup, never folded in.

## NEXT SESSION: Phase 3 kickoff (Linux port) — planned for the morning
Read CLAUDE.md + roadmap + lessons + architecture + THIS handoff first, then
design-first as always. The shape from roadmap/architecture: WSL2 on 7of9 as
the fast inner loop; GitHub Actions matrix (windows-latest via msys2 +
ubuntu-latest) as the source of truth — the CI matrix is the highest-leverage
first move (it catches Windows-isms leaking into core the moment they appear);
`src/platform/linux/` impls for the four seams (libcurl HTTP, Unix-socket IPC,
libnotify notify, SG_IO CD — the ICdIo header already documents the SCSI
one-to-one mapping); the pure suites already build Linux-clean by design.
Expect a survey first: what actually stands between today's tree and a Linux
configure (ncursesw UI, miniaudio backend, windows.h stragglers outside
platform/ — e.g. CDSource.cpp's Sleep, Mp4Chapters, IHeartDeepLog, Log).
Remaining parked pool: Class-B OOB-gated re-pin, Stage-A retirement, `stop_`
into IHeart polls, stalled-connect interrupt, Track Info album-tag decode,
scrobble dedup, ICY items.

## Operating discipline (unchanged)
- Read CLAUDE.md + roadmap + lessons + architecture + THIS handoff first.
- Design-first; correctness argument before audio-thread changes; probe-first.
- Verify → build → ctest → real-world gate → commit (code and docs separately,
  no Co-Authored-By). Update roadmap/lessons/handoff as the session's last step.
