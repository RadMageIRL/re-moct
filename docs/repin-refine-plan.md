# repin-refine - design of record

Scope: refine RE-MOCT's iHeart live-edge re-pin. Three separable sections:
A - reduce the clip at the start of the incoming song after a re-pin;
B - a user-facing F6 toggle for re-pin behaviour (off / on / smart);
C - a persistent lower-left indicator for feed + re-pin mode.

Context (settled, do not reopen): the iHeart investigation closed with identity
inert, no keepalive (HAR-proven on Z100), and no clean-tier exemption (the free
web player takes the same ads). RE-MOCT's advantage over a naive poller is the
ACTIVE RE-PIN. This slice refines that re-pin. It does NOT touch identity or add
keepalive/session machinery, and does NOT build a dual-stream handoff.

Branch: experimental/win-pdcurses. Version stays 1.3.0 (one unreleased cycle; no 1.3.1).

---

## Part A diagnosis (what the deep log + tool ground truth showed)

- The re-pin fires on a FIXED ~35s LIVE-floor stall timer (`LIVE_STALL_MS`), not on
  ad-end detection. Every re-pin in remoct-deep-analysis-20260718 fired ~35-40s after
  the display floored to LIVE.
- On re-pin the prime kept the last 2 segments (~20s behind the live edge)
  (`StreamSource::hlsConnect`). So even a well-timed re-pin lands audio ~20s behind
  the manifest live edge.
- The clip is VARIABLE and alignment-dependent (0 to ~30s), not a fixed 20-30s:
  clip ~= max(0, (repin_fire - broadcast_song_start) - ~20s prime). Cross-referencing
  the three re-pins that overlap the zc4366 tool capture (05:36, 06:17, 06:43), remoct
  landed within ~10s of the broadcast song start (the re-pin happened to fire during
  the ad, before the song = minimal clip). The bad cases Dos hears are short-ad
  alignments where the fixed timer fires into the song.
- CRITICAL: the primary digital manifest shows BLANK SLATE during a break
  (mfCls=None, spotInstanceId blank), never the ad's paid markers and never the
  returning song's music marker. The music flip appears only AFTER the re-pin (fresh
  handshake). So "re-pin when spotInstanceId flips to -1 on the current stream" is
  impossible (the marker is absent, not merely delayed). Confirmed the lessons.md
  section-5 dead-end.

Fix chosen (locked by Dos): prime-to-music-boundary (a), with the live-edge-minus-2
fallback held byte-intact. Lever (b) lower-`LIVE_STALL_MS` held in reserve. No
dual-stream handoff (the clip is a timing problem, not a reconnect-gap problem).

---

## Section A - prime-to-music-boundary

On a re-pin's fresh manifest, scan for the first ad->music boundary and prime from the
song's first segment instead of live-edge-minus-2, so audio lands at the song start.

- `plugins/stream/HlsPrime.h` (new, header-inline for testability): `hlsFirstMusicBoundary(body)`
  returns the 0-based segment index of the first `song_spot=M/F` segment PRECEDED by a
  non-music segment in-window, else -1. On a fresh connect, segment order == pending order.
- `hlsPollMedia`: during a connect poll only (`hls_priming_`), records the boundary into
  `hls_prime_boundary_`. Normal polls untouched.
- `hlsConnect`: sets `hls_priming_`/`hls_prime_boundary_` around the connect poll(s). The
  prime primes-to-boundary ONLY when `hls_repin_active_ && digital_active_ &&
  hls_prime_boundary_ > 0`; otherwise it keeps the unchanged `pending.end()-2` prime.
- `producerWorkerAAC`: sets `hls_repin_active_` around the re-pin's `connect()` so first
  tune-in and ordinary reconnects keep the old behaviour.

FAIL-SAFE (hard requirement): any ambiguity -> `hlsFirstMusicBoundary` returns -1 ->
caller keeps the unchanged live-edge-minus-2 prime. Cases returning -1: window opens on
music (start fell off window), no music, no `song_spot`, single music segment. All three
new-state members are producer/connect-thread-owned (like `hls_`), no lock, no atomic.

Byte-identity of the fallback: `hls_pipeline_test` (non-re-pin connect) still asserts
prime-to-live 2 segs and passes unchanged; `hls_prime_test` unit-proves every -1 case.

---

## Section B - F6 re-pin mode (off / on / smart)

- `Config.repin_mode` int 0=off / 1=on / 2=smart, default 2 (behaviour change; see
  Divergences), persisted like `prefer_digital_stream`.
- Plumbing: `AudioManager::setRepinMode` -> `PluginSource::setConfig("repin_mode", ...)`
  -> `StreamPluginAdapter sp_set_config` -> `StreamSource::setRepinMode` (atomic
  `repin_mode_{2}`). Read live by the producer/SM, so F6 needs no reconnect.
- Gating:
  - Ad-onset immediate re-pin (`hlsPollMedia`): fires only in mode `on` (1). smart/off
    suppress it; the arm/cooldown bookkeeping is untouched so a live switch stays coherent.
  - LIVE-floor stall (`IHeartNowPlayingSM::tick`): `repinMode` picks the threshold -
    off never fires; on = `LIVE_STALL_MS` (35s, original); smart = `SMART_STALL_MS`
    (150s) so short aligned breaks ride out and only long pods trip the re-pin.
- `IHeartTick.repinMode` defaults to 1 so existing SM unit tests are unchanged.
- UI: F6 cycles `(repin_mode+1)%3`, saves, applies, redraws. Independent of Ctrl+K.

NOTE (design constraint, ties to Part A): the brief asked to pass the paid-ad flag into
the SM for pod-awareness. Empirically the primary manifest shows BLANK SLATE (not
paid-ad markers) during a break, so the paid-ad flag is absent mid-break and cannot gate
smart. Smart therefore uses floor DURATION as the pod-length proxy (a long blank floor ==
a long pod). `SMART_STALL_MS = 150000` is conservative; Dos tunes on-air.

---

## Section C - confirm-on-change mode indicator

- Drawn in `drawProgress` stream row, at the row's left inset, in yellow (`CP_MODE`,
  COLOR_YELLOW), format `<feed> - <repin>` e.g. `digital - smart` / `raw - off`. iHeart
  streams only (host sniff for `ihrhls`); the now-playing title shifts right by the tag
  width WHILE it shows. Empty tag (idle or non-iHeart) -> byte-identical to the plain layout.
- Confirm-on-change, NOT always-on: a Ctrl+K or F6 toggle stamps `mode_tag_at_` with the
  wall-clock; the tag shows for `kModeTagMs` (~5s) then clears so the now-playing text
  reoccupies the lower-left. Non-blocking: the window is checked in the normal draw loop
  (the live-stream row already redraws on the scanner heartbeat), never a sleep/wait.
  Revised from the first always-on design after seeing it live: an always-on mode tag
  competes permanently with the now-playing text for lower-left space, and the mode only
  matters at the moment you toggle it -- so confirm-then-fade (the old toast behaviour,
  in the persistent location) is correct.
- `CP_MODE = 18` registered in both `initColours` (Classic) and `applyAwesomeTheme`
  (Awesome) as basic yellow on default bg, theme-independent.
- Replaces the transient Ctrl+K toast: Ctrl+K flashes the feed half, F6 the re-pin half,
  each showing the current combined state for ~5s.

---

## Gates

- ctest both toolchains green: Windows UCRT64 34/34, Linux Debian 35/35.
- New: `hls_prime_test` (boundary + all fallback -> -1 cases, escaped and bare
  song_spot forms); `iheart_sm_test` extended (off never fires, smart fires only past
  ~150s not at 39s, on unchanged at 35s).
- Section A's REAL gate is Dos's on-air by-ear test: re-pin through a morning-drive ad
  break, confirm landing at/near song-start in good-alignment cases and safe fallback in
  the edge cases; verify off/on/smart differ on-air; eyes-on the yellow indicator both
  themes/builds. Machine gates are necessary-not-sufficient for A.

## Divergences from the brief (flagged, none blocking)

1. The clip is intermittent/alignment-dependent, not a fixed 20-30s (Part A diagnosis).
2. "Pass the paid-ad flag into the SM" is unfulfillable as literally stated - the marker
   is absent on the primary mid-break; smart uses floor duration as the proxy instead.
3. `docs/repin-refine-plan.md` did not exist at implementation time (this file); created
   from the reviewed diagnosis + this brief.
4. Version: 1.3.0 was UNRELEASED. First bumped to 1.3.1, then reverted to 1.3.0 on Dos's
   call - this whole cycle (AAC/M4A, convert, art-embed, encoder-bitrate, repin-refine)
   is one unreleased 1.3.0; there is no 1.3.1. CHANGELOG kept in the single 1.3.0 bucket.

## Follow-up (post first commit 87ff1a2)

- Indicator changed from always-on to confirm-on-change-then-fade (Section C above).
- Version reverted 1.3.1 -> 1.3.0; CHANGELOG merged back to the single 1.3.0 bucket.
- Neither touches the audio path; Section A stays exactly as committed in 87ff1a2.
