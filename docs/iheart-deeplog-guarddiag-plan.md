# iheart-deeplog-guarddiag - design of record

Scope: make every trackHistory freeze in the iHeart deep log self-explaining. Record
which guard held the display and the monotonic ceiling vs the newest-aired startTime it
held on, plus the repin-armed flag, so Z100's LIVE-while-music episodes can be attributed
to a cause (monotonic regression / feed reorder / none-aired) and correlated with re-pins.

Nature: pure instrumentation. Observation-only. Does not change control flow, return
values, guard behaviour, or `accepted_max_start_`. Rides the existing Ctrl+A deep log.

Branch: experimental/win-pdcurses. Version stays 1.3.0 (unreleased cycle).

Context: Phase 1 (off-hours capture) and its cross-reference against the runtime logs
established that the trackHistory LIVE/staleness state is metadata-feed blindness, not
ads or dead air - the 888s Breeze gap played three songs in-band with no ad break, and
was bounded by the staleness path (`heldBy==""`), not by a guard trip. That is the same
shape as the reported Z100 symptom (song playing, screen stuck on LIVE for minutes). The
existing Record already lets a derived `LIVE_while_music` fire from `tgtKind`/`mfCls`/
`thEnded`; this slice adds the CAUSE so the Phase 2 drive-time differential can classify
each LIVE-while-music episode instead of just counting it.

---

## What the diagnostics answer

For every deep-log tick, `thHeldBy` names the guard that suppressed the display:

- `""` - served (or staleness/LIVE path; the feed simply has no newer entry - feed blindness)
- `monotonic` - the feed offered a newer-but-lower entry and the guard rejected it;
  the pair `(thAccMaxStart, thNewestStart)` reads as ceiling-vs-offered and proves the regression
- `none-aired` - nothing in `data[]` has aired yet (with `thFutureSkip` > 0 = not-yet-aired entries)
- `no-data` / `parse` / `poll-fail` / `unresolved` - upstream failures
- `empty-meta` - newest aired entry carried neither artist nor title

`thChosenIdx != 0` = the scan picked a non-[0] entry, i.e. Z100 resurfaced an old entry
at data[0] above newer ones (feed reorder). `repinArmed` mirrors `hls_repin_armed_` so an
episode onset can be separated into re-pin-caused vs re-pin-coincident (auto-repin clusters
on the ~90s ad-pod cooldown, exactly where Z100's feed misbehaves - a real confound).

## Change set (6 files)

1. `IHeartRadio.h` - `PollDiag` struct (acceptedMaxStart / newestAiredStart / entryCount /
   chosenIdx / futureSkipped / heldBy) + `pollNowPlaying(long* endedSecsAgo = nullptr,
   PollDiag* diag = nullptr)`. `heldBy` holds static string literals only.
2. `IHeartRadio.cpp` - the ceiling is captured at ENTRY (before the accept path mutates
   `accepted_max_start_`), then each existing guard branch tags `diag`. Every write is
   `if (diag)`-guarded; no branch condition, return value, or mutation was changed.
   `futureSkipped` counts only genuinely-future entries (`s > now + FUTURE_GRACE`, which
   implies `s > 0`), not undated ones. `chosenIdx`/`newestAiredStart` are set after the
   scan on the accept path, once `best`/`bestStart` are final.
3. `IHeartDeepLog.h` - seven fields appended to `Record` beside the trackHistory group.
4. `IHeartDeepLog.cpp` - seven keys serialised in the NDJSON writer; `kSchema` 2 -> 3;
   `sigOf` (the change/heartbeat dedup signature) gains `thHeldBy`, `thNewestStart`, and
   `repinArmed`. It deliberately does NOT gain `thChosenIdx` (a resurfacing event always
   co-emits: it either trips a monotonic hold - thHeldBy changes - or the song is served
   and `th` changes). `thNewestStart` is constant across a freeze, so heartbeat-collapse
   is preserved; the three additions emit immediately only on a real edge.
5. `StreamSource.h` - `IHeartRadio::PollDiag iheart_th_diag_;` member.
6. `StreamSource.cpp` - a local `thDiag` is passed to the poll and cached into the member
   inside the ~9s throttle block, then read into the Record. This mirrors how
   `iheart_th_cache_`/`iheart_th_ended_` are cached: the poll runs ~every 9s while the
   Record block runs every tick, so a local alone would read as zeros on non-poll ticks
   and desync from the cached `th`/`thEnded`. (This corrected the original brief, which
   prescribed a bare local on the mistaken premise that same-function scope was
   sufficient - the throttle, not scope, is the reason a member cache is required.)

## Boundaries held

- Audio thread untouched: `updateIHeartNowPlaying` runs on the producer thread;
  `frames_drained_` stays a relaxed-atomic read; `PollDiag` carries no atomics.
- No change to the monotonic guard, `FUTURE_GRACE`, or the staleness -> LIVE logic.
- No manifest-fetch-per-tick (that is Phase 2A, a separate slice). No re-pin reason
  tagging (the `repinArmed` bool is enough for this slice). No new key binding; no PDT work.
- No `ar_crc`/CD-path touch.

## Gates

- Machine gates green both toolchains: Windows UCRT64 34/34, WSL Debian 35/35.
- Behaviour-preservation audited from the diff: every existing branch/return byte-identical,
  every diag write `if (diag)`-guarded; with `diag == nullptr` (probe/legacy path) the
  function behaves exactly as before. New key strings + heldBy literals present in the
  built `remoct_stream.dll`.

## Open (Dos, live / eyes-on)

The deep-log-on behaviour is verified live, and lands naturally in the Phase 2 drive-time run:

- Ctrl+A on emits the new keys; `_meta` shows `schema:3`.
- Short Breeze capture reads `thHeldBy:""` throughout with `thChosenIdx:0` (clean-monotonic
  proof; the IHeartRadio.cpp "provable no-op on a clean station" comment).
- An observed monotonic hold on Z100 shows `thHeldBy:"monotonic"` with
  `thNewestStart < thAccMaxStart`.

## Phase 2 analysis framing

The unit of analysis is the LIVE-while-music episode (a run of ticks with `tgtKind=="Live"
&& mfCls=="Song"`). Do not pre-filter to guard trips - Phase 1's own clincher (the 888s
Breeze feed-blindness gap) was `heldBy==""`, so restricting to HELD/FUTURE/EMPTY would
score the real bug as clean. For each episode record duration and classify by cause:
feed-blindness (`thHeldBy==""`, `thNewestStart` unchanged, `thEnded` climbing), monotonic
hold (`thHeldBy=="monotonic"`, `thNewestStart < thAccMaxStart`), resurfacing
(`thChosenIdx != 0` near the edge), or future/empty. The differential is Z100 vs Breeze on
episode rate, duration, and cause-mix - not raw LIVE%. Correlate onset with `repinArmed`.
Confound to state honestly: Phase 1 Breeze was off-hours, so station and time-of-day are
confounded; get one Breeze drive-time deep-log run in the same window type if feasible.
