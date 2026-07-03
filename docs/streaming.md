# RE-MOCT — streaming internals

## iHeart structural reality
RE-MOCT pulls **raw `revma.ihrhls.com` HLS** — closer to the broadcast source — which
bypasses iHeart's client-side ad-stitching and metadata-sync layer. Consequences:
- Metadata desync has two root causes: **timeline divergence at ad zones**, and
  general **clock skew / feed freeze**. These are structural, not just parsing bugs.
- **Session aging** (not identity parameters) is the likely driver of long ad blocks.
  Handshake params `profileId`, `skey`, `listenerId` are **confirmed inert** — a dead
  end, don't chase them again.

## iHeart metadata state machine
- Asymmetric debounced `IHNow` enum (Live / Song / Ad):
  - Songs commit **instantly**.
  - Ads require **3 ticks + trackHistory corroboration**.
  - Dual-blind floors to "LIVE".
- Monotonic `startTime` guard rejects out-of-order / future-dated trackHistory entries.
- In-band ad signal (`cls == Ad`) overrides trackHistory.
- `looksLikeRealTrack` exact-match guard prevents "LIVE" being scrobbled.
- The now-playing fixes (ctm priority, overhang/flap) are landed.

## Drift instrumentation (already in place)
- Poll log includes: newest segment number, `nextPlay`, `edgeLag`, `ringSec`, `cls`, `pdt`.
- `IHeartDeepLog` includes `pdt` / `spotPaid`, with a 5MB roll limit.

## The rabbit-hole capture — DONE, both questions answered (2026-07-03 analysis)
The captures landed (38 deep-log files, 4,783 ticks / 34.2 h in `logs/iheart/`,
plus the 07-02 operational log with Stage-A peek data) and were analyzed:
- **`edgeLag` does NOT climb — flat at 0–2 segments through every ad block**
  (598 polls, incl. a 1,091 s ad span). We never fall behind the manifest edge;
  the desync is in WHAT the edge serves, not our lag. edgeLag-gated re-pin: dead.
- **The fresh-edge peek was built (Stage A) and its data KILLS Stage B:** in 8/8
  instrumented break episodes the fresh parallel session's live edge served ads
  the whole time (up to 115 s of peeks) until the primary's break cleared on its
  own. iHeart's SSAI stitches new joiners into ad pods too — there is no earlier
  music edge to promote into. **Stage-B promotion, faster manifest polling, and
  the staggered-peek machinery are all rejected on this evidence** (roadmap Done
  entry has the full numbers). The Stage-A scaffold is inert; retiring it is a
  parked optional cleanup.
- What remains fixable is the **Class-B hole** (session loops a stale slate while
  trackHistory/ctm prove the broadcast returned, ~1 per ~6 h, 131–300 s) — the
  OOB-gated re-pin candidate parked in roadmap.md, design-first when chosen.

## Scrobble duplicate
Back-to-back duplicate scrobbles come from a mid-song `IHNow` flicker creating two
commit boundaries. Proposed fix: source scrobble commits from the **debounced `IHNow`
state** rather than raw now-playing changes, with a TTL-based dedup set as backstop.
Needs log inspection on 7of9 to confirm the mechanism before coding.

## ICY / SHOUTcast
- Metadata is a **structural protocol limitation** — improvement is bounded.
- ICY ingest sanitization (station-ID stripping, alternate separators) is deferred;
  it won't cause wrong covers because of the dual-field (artist+title) validation.

## Standalone probes (probe-first methodology)
- `SniffIHeartRadio.exe` (probe; `-P` / `-M` flags).
- `iheart_http_dump.cpp` (4-variant concurrent session probe).
- `radio_probe.cpp` (stream connectivity probe).
Validate new parsing/protocol logic in a probe before integrating.

## Device / playback fixes (landed, for reference)
- `AudioManager::setDevice()` handles radio-stream and CD modes (separate `ma_device_`
  init paths), not just local-file playback.
- Audiobook-first cold-start wedge fixed: a one-time warm-up device init, plus forcing
  every decoder to output fixed 44100/2 so no odd native format reaches the device.
- Streaming toast / play indicator uses `stationLabel(streamUrl())` (not cursor-dependent
  `playlist_.current()`), fixing stale identity across radio/file transitions.
