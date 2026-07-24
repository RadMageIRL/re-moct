# Crossfade correctness + queue-aware preload — design of record

Campaign: **XF**. Branch `experimental/win-pdcurses`, designed against tip `decf950`.
Status: design only. No production code written. Supersedes the XF-R1/XF-R2 analysis
brief where the two disagree; every §1 item below was settled by the XF-P probe, not
by reading.

Probe evidence lives in `tests/xfade_handoff_test.cpp` (blocks `P1`/`P2`).

---

## 0. What the probe settled

| finding | evidence |
|---|---|
| Swap-install loss is real | V0: `currentTrack()` = A while B is audibly playing |
| Torn identity | V0: path A / `dur_field` 2 against `durationSec()` 3.00 |
| Moving the flag clear inside the `if` is refuted | V1 installs C — armed but never audible |
| Root cause is a single-slot `next_track_info_` | V1 vs V2 differ only in drain order |
| Drain-before-clobber works | V2: path B, `dur_field` 3, `durationSec()` 3.00, coherent |
| Repeat-one contamination is real and recurs | P2: 97-98% of `crossfade_secs`, every loop, full scale |

Anchors below were re-read against the working tree. Line numbers drifted from the
snapshot in the brief only trivially; corrections are noted inline.

---

## 1. New finding — §2 is reachable, and it has a corollary

**The §2 path is confirmed**, with one refinement the brief did not state: it requires
that *nothing is armed* at the moment the repeat-one track ends. That is the normal
repeat-one state, because `UIManager::maybePreloadNext` (`src/UIManager.cpp:952-958`)
refuses to arm under repeat-one — so the path is not exotic, it is the default.

1. Repeat-one ON, queue holds a track, nothing armed.
2. Track ends. With nothing armed the callback takes the silence-fill `else`
   (`src/AudioManager.cpp:874-879`) and sets `track_ended_flag_`.
3. `on_track_end_` (`src/main.cpp:199`) runs the queue-priority loop, pops, calls
   `play_path(qpath)`.
4. Inside `play_path` (`src/main.cpp:222-226`) the queue is now empty, so it arms
   `preloadNext(peekNext())` — with **no repeat-one guard**, unlike
   `src/UIManager.cpp:953` and `src/main.cpp:301`.
5. `PlaylistManager::peekNext()` returns `entries_[current_].path` under repeat-one
   (`src/PlaylistManager.cpp`, repeat-one branch) — the **playlist** current track,
   not the queued track now playing. A foreign decoder is armed.
6. Every boundary thereafter mixes that foreign track at full gain for the whole
   fade, forever. This is P2's mechanism reached through product control flow.

### Corollary — queue starvation in repeat-one (new, not in any prior brief)

Once step 5 has armed something, the crossfade-completion path sets
`preload_next_flag_` and **not** `track_ended_flag_` (`src/AudioManager.cpp:857-858`),
and the repeat-one early return in `initCrossfade()` never sets `track_swap_flag_`.
So `on_track_end_` never fires again. Any *further* queued track is never drained —
the queue silently stops working until the user leaves repeat-one.

Both the bleed and the starvation are closed by XF-R1a/b: with the trigger guarded,
repeat-one falls to the silence-fill `else`, which restores `track_ended_flag_` and
therefore restores queue drain. **XF-R1 alone fixes the audible symptom of §2** —
the `main.cpp` guard is hygiene, not the cure. See D4.

---

## D1 — swap payload ownership

**Decision: drain-before-clobber, helper on `AudioManager`, `:156` deleted outright.**

```cpp
// AudioManager, private
void installPendingSwap();   // main thread only
```

```cpp
void AudioManager::installPendingSwap() {
    if (track_swap_flag_.exchange(false)) {
        current_track_ = next_track_info_;
        retired_src_.reset();
    }
}
```

This is the existing `pollEvents()` block (`src/AudioManager.cpp:552-559`) lifted
verbatim. Call sites: top of `pollEvents()` (replacing the inline block) and top of
`teardownNext()` (before anything can clobber `next_track_info_`).

**Locking.** `installPendingSwap()` takes no lock and must not. `pollEvents()` calls
it with no `state_mutex_` held; `teardownNext()` calls it while its callers
(`preloadNext`, `clearNext`, `play`, `stop`, `openCD`, `beginStream`, `~AudioManager`)
hold `state_mutex_`. That asymmetry is safe because **every one of those callers is the
UI thread** — verified: all seven `teardownNext()` call sites are main-thread, none is
reachable from the audio callback. So the two entry points can never run concurrently,
and the helper acquires nothing, so it cannot deadlock against a held `state_mutex_`.

The cross-thread edge is unchanged: the audio thread finishes reading
`next_track_info_` before its `track_swap_flag_.store(true)`, and the seq_cst
`exchange` here synchronizes-with that store. `retired_src_.reset()` still happens on
the UI thread only.

**Is draining always semantically correct? Yes.** If `track_swap_flag_` is set, the
swap *already happened* on the audio thread — `file_src_` **is** the new track and is
audible. Refusing to install only desynchronizes the UI from reality. There is no
caller that wants the old cancel:

| caller | pending swap at entry | effect of draining |
|---|---|---|
| `preloadNext` | the P1 case | installs B correctly — the fix |
| `clearNext` | user hits `q`/`r` just after a swap | installs B instead of stranding A |
| `play(newfile)` | possible | installs B, then overwritten by the new file's info |
| `stop()` | possible | installs B, then `current_track_ = {}` clears it |
| `openCD` / `beginStream` | possible | installs B, then deliberately cleared |
| `~AudioManager` | possible | harmless |

The last four are no-ops in net effect, so drain is never wrong and is load-bearing
for the first two. The comment at `:154-155` explains a hazard that drain removes at
the source, so the `track_swap_flag_.store(false)` at `:156` is **deleted**, not moved.
V1 measured what moving it does.

---

## D2 — does drain close torn identity everywhere?

**No, and the design deliberately accepts the residue. Stated plainly so it is not
assumed handled.**

`cached_duration_` is stored at `src/AudioManager.cpp:382`, `track_swap_flag_` at
`:384`. A UI read landing between those two statements sees the new duration against
the old title on *every* normal crossfade, independent of the clobber bug.

That window **cannot** be closed by deferring the `cached_duration_` store into
`installPendingSwap()`, because `cached_duration_` is read **by the audio thread** —
`durationSec()` feeds the crossfade trigger's `dur - pos` test at `:794`. Deferring it
would leave the trigger comparing against the *previous* track's duration for up to a
full UI tick (~80ms), which could fire a spurious crossfade immediately after a swap.
That is a worse bug than the tear.

So: the two values are the same number (`cached_duration_` is assigned
`next_track_info_.duration_sec`, which is exactly what `current_track_.duration_sec`
becomes at install) published at two instants nanoseconds apart.

**Decision: accept as a sub-frame transient.** What D1 buys is the important half —
it converts a *permanent* tear (V0: A's title against B's progress for the entire
track) into a two-statement one that the next `pollEvents()` corrects. Hardware gate
item 12 is written against the settled state, not the sub-frame state. Not fixed;
recorded.

---

## D3 — XF-R1 final shape

Confirmed as briefed, with the additions reasoned through.

**R1a — trigger guard.** `src/AudioManager.cpp:791`, add `&& !repeat_one_.load()`.
Plain `load()` to match the existing style at `:358`. (Correction carried forward: the
earlier brief called `:358` a relaxed load; it is a default **seq_cst** load. The
recommendation is unchanged, the justification was wrong.)

**R1b — gapless splice guard.** `:862`, gate the `next_decoder_initialised_` branch on
`!repeat_one_.load()` so the tail-fill at `:867` cannot sound the next track.
Repeat-one falls to the silence-fill `else` at `:874-879` — which is the path it
already takes today whenever nothing is armed, and which restores `track_ended_flag_`
and therefore the queue drain (see §1 corollary).

**R1c — clear the flag on the repeat-one early return.** `:358-362`, add
`next_decoder_initialised_.store(false, std::memory_order_release)`. Precedent is
`:134`, **not** `:374` — `:374` is a plain seq_cst store, so the brief's cited
precedent was wrong even though the recommendation is right.

**No `reset()`, no `retired_src_`** in that branch, confirmed and for a sharper reason
than "it frees heap": `retired_src_` is reaped *only* inside the `track_swap_flag_`
block (`:558`). Retiring from a branch that never sets that flag means the object is
never reaped, and a **second** repeat-one early return would run
`unique_ptr::operator=` over a non-null `retired_src_` — freeing heap on the audio
thread, exactly the hazard `:365-371` was written to eliminate.

**R1d — unconditional `next_src_.reset()` in `teardownNext()`.** Confirmed and needed
*because of* R1c: after R1c the decoder is unpublished but still owned, and today's
`next_src_.reset()` lives inside `if (next_decoder_initialised_)`, which R1c has just
made false. Move the reset outside that block, after the un-publish/device-stop logic.
Safe: with the flag false, both audio-thread read sites (`:825` crossfade mix, `:862`
gapless) are gated off, and the only paths that clear the flag are `teardownNext`
itself (which already performs the device-stop quiesce) and `initCrossfade` on the
audio thread (which is done reading by then). Without R1d the stale decoder lingers
until the next `preloadNext` or destruction.

---

## D4 — where the §2 fix belongs

**Decision: land the guard in the XF-R1 commit, one line at `src/main.cpp:222`.**

The audible bleed is already closed by R1a/b, since the guarded trigger never fires
under repeat-one no matter what is armed. The guard's job is narrower: stop arming a
*foreign* decoder at all. That still matters — it is a wasted file open plus a TagLib
parse on the UI thread on every queue-drain under repeat-one, and it leaves a decoder
armed that is wrong by construction.

Shape: mirror `src/main.cpp:301` — `if (playlist.repeatMode() != RepeatMode::One)`
around the arm inside `play_path`.

**Interim under the alternative** (defer to XF-R2's consolidation, which deletes the
site): no audible defect, because R1 already fixed that. The user would experience only
the invisible cost — a redundant decoder opened per queue-drain in repeat-one. That is
tolerable, so this is a weak preference, not a blocker. It is one line and it makes
XF-R1 self-contained, which is why it goes there.

---

## D5 — XF-R2, respecified against the D1 protocol

D1 structurally closes the hazard that dominated the earlier R2 design: with drain in
`teardownNext()`, a poll calling `preloadNext()` can no longer destroy a pending swap.
That removes the need for any swap-pending guard in the poll.

**Resolver** (`UIManager`, UI thread only, returns the authoritative next path or empty):

1. repeat-one → empty
2. queue non-empty → `queueAt(0).path`
3. else → `peekNext()`
4. reject CD paths and stream paths → empty

**State.** `AudioManager` gains `bool hasNextArmed() const` returning
`next_decoder_initialised_.load(std::memory_order_acquire)` — atomic, race-free, sits
beside the existing `isCrossfading()`. `UIManager` keeps `armed_next_` (UI-thread-only
mirror of the last path handed to `preloadNext`) and `failed_next_` (the latch below).
**No string accessor on `AudioManager`**; `next_path_` stays audio-thread-written and
is never read from the UI thread.

**`reconcileNextArm()`**, called in `run()` immediately **after** `audio_.pollEvents()`
(`src/UIManager.cpp:1272`). The ordering is load-bearing: `pollEvents()` fires
`on_preload_next_` and `on_track_end_`, which advance the playlist index, so the
resolver must run after them or it resolves against a stale index.

```
if (audio_.isCrossfading()) return;              // committed fade — leave it alone
desired = resolveNext()
if (desired.empty()) {
    if (audio_.hasNextArmed()) audio_.clearNext();
    armed_next_.clear(); failed_next_.clear(); return;
}
if (desired == failed_next_) return;             // don't hammer an unopenable file
if (audio_.hasNextArmed() && armed_next_ == desired) return;   // already correct
if (audio_.preloadNext(desired)) { armed_next_ = desired; failed_next_.clear(); }
else                            { failed_next_ = desired; armed_next_.clear(); }
```

`preloadNext()` calls `teardownNext()` internally, so no explicit `clearNext()` is
needed on the re-arm path — and skipping it avoids a redundant lock round-trip.

**`isCrossfading()` guard rationale.** Without it, any mutation that changes the
resolver's answer mid-fade (`queueRemoveAt`, `queueClear`, playlist `removeAt`,
shuffle toggle) would reach `clearNext()`'s device stop/start branch (`:143-148`) and
click. A committed fade stays committed — consistent with the residual hard cut the
R2 brief already accepts for `q`.

**Failure latch rationale.** The poll re-arms whenever `hasNextArmed()` is false. If
`preloadNext()` fails, that stays false forever, so without the latch the poll retries
a doomed file every ~80ms, each retry a file open plus TagLib parse on the UI thread.

**Protocol interaction.** `track_swap_flag_`: closed by D1. `preload_next_flag_`: the
callback at `src/main.cpp:185-196` keeps its `playlist.next()` — that index advance is
**not** reproducible by the poll — and loses only its arming tail (`:190-192`). That
tail is a 16th arming site the earlier R2 brief's removal list omitted.

**Consolidation.** All 13 `maybePreloadNext()` sites (`src/UIManager.cpp:1064, 6542,
6765, 6771, 6789, 6795, 7805, 7863, 8351, 8452, 8478, 8562, 8589`) are the identical
shape — *"we just called `play()`; arm the follower"* — with no side effects, and all
are reproducible by the poll. Remove them and the function. Remove `src/main.cpp`
`:222-226`, `:301-304`, and the `:190-192` tail. Keep the instant clears at
`src/UIManager.cpp:7228` (`q`) and `:7497` (`r`) — polling those would let a fade
complete and swap, reintroducing the contamination.

Behavioural delta: arming moves from synchronous-with-`play()` to up to one tick
(~80ms, `timeout(80)` at `src/UIManager.cpp:306`) later. Irrelevant against a 2s fade.

---

## D6 — commit plan

Four commits. Each is independently revertable and has a machine test that fails
before and passes after.

**C1 — swap payload drain (D1).**
`installPendingSwap()` + both call sites + delete `:156`.
*Bisect story:* convert probe block P1 from printf to assertion —
`CHECK(currentTrack().path == C_expected_B)` plus a duration-coherence `CHECK`.
Fails on `decf950`, passes after. Smallest commit, and R2 depends on it.

**C2 — XF-R1 repeat-one contamination (D3 + D4).**
R1a trigger guard, R1b splice guard, R1c flag clear, R1d unconditional reset, plus the
one-line `src/main.cpp:222` repeat-one guard.
*Bisect story:* convert P2 to an assertion — `CHECK(max_viz_peak < 0.01)` under
repeat-one with an armed next. Fails on `decf950` (measured 0.8977), passes after.
Also restores queue drain in repeat-one (§1 corollary); worth a second assertion that
`track_ended` fires under repeat-one with something armed.
*CHANGELOG:* user-facing — repeat-one no longer bleeds the next track, queued tracks
play under repeat-one.

**C3 — XF-R2 queue-aware preload (D5).**
Resolver, `hasNextArmed()`, `reconcileNextArm()`, the 16-site consolidation.
*Bisect story:* resolver precedence table (repeat-one / queue head / `peekNext` / CD /
stream / empty playlist / empty queue / shuffle / repeat-all wrap) plus a convergence
test asserting the poll reaches the correct armed state within one tick from every
starting state, mutating through every queue and playlist API.
*CHANGELOG:* user-facing — queue transitions crossfade instead of hard-cutting.

**C4 — null-backend test enablement (D7), optional, Dos's call.**
Un-gate `xfade_handoff_test` from Windows using the null backend.
*Bisect story:* the test runs and passes on both toolchains rather than skipping.
Sequenced last so C1-C3's assertions exist to be run under it; could equally go first
if Dos wants Linux/TSan coverage while C1-C3 are being written.

Ordering constraint: **C1 before C3** (the poll relies on drain). C2 is independent of
both and could move.

---

## D7 — null backend: feasible, and it refutes the earlier estimate

**Measured, not estimated.** The prior conclusion — "un-gating pays zero because the
test needs real hardware" — is **wrong**, and the premise the brief flagged was indeed
false.

Route is cleaner than the `MA_NO_*` list: `lib/miniaudio.h:6696` gates the null backend
on `(!defined(MA_ENABLE_ONLY_SPECIFIC_BACKENDS) || defined(MA_ENABLE_NULL))`, so
defining **`MA_ENABLE_ONLY_SPECIFIC_BACKENDS` + `MA_ENABLE_NULL`** selects null and
excludes every real backend in one pair of defines. `MINIAUDIO_IMPLEMENTATION` is
compiled into the test target's own copy of `src/AudioManager.cpp`, so
`target_compile_definitions` on that target alone works with **zero production change**.

Verified on this machine by building and running the full suite that way:

```
P1 RESULT:   currentTrack=xfp_a2.wav [A]  dur_field=2  durationSec=3.00  -> MISMATCH
P2 RESULT:   contamination=PRESENT  max_viz_peak=0.8977  first=609ms
P2 episode 1: 609..1594ms  span=985ms  (98% of crossfade_secs)
P2 episode 2: 2609..3594ms  span=985ms  (98% of crossfade_secs)
xfade_handoff_test: ALL PASS
```

All five pre-existing regression cases pass. P1 reproduces identically. P2 reproduces
at 98%/98% against 98%/97% on WASAPI. The re-validation the brief asked for is done:
the null backend's timer-driven callbacks hold realtime cadence closely enough that
P1's `Sleep(5)` margin and P2's figures both survive unchanged (P2 first-contamination
609ms vs 563ms on WASAPI — ordinary jitter).

**Remaining cost to actually un-gate** (not done, C4): swap three hardcoded
`platform/win/` sources for the existing `${HTTP_IMPL}` / `${CDIO_IMPL}` /
`${PLUGINLOADER_IMPL}` vars (all present, `tests/CMakeLists.txt:8-41`), replace
`GetTickCount`/`Sleep` with `std::chrono` / `std::this_thread::sleep_for`, move the
target out of `if(WIN32)`, add `${CDIO_LIBS}`. Estimate 1-2 hours. The payoff is real
after all: device-free runs on WSL2 and CI Debian, and the only viable route to
ThreadSanitizer over this code — which is the one tool that would have caught the
`track_swap_flag_` race by construction instead of by argument.

---

## What this design does NOT fix

Listed so nothing is assumed handled.

1. **Sub-frame torn identity** between `:382` and `:384` (D2). Accepted, not closed.
2. **Residual hard cut** when `q` lands during an already-mixing fade. Intended; the
   audio is committed and aborting it clicks.
3. **`teardownNext()`'s device stop/start** (`:143-148`) remains a click on mid-fade
   abort. Replacing it with a gain ramp-out is a separate slice, out of scope.
4. **Resolver oscillation between two unopenable files** would defeat the single-entry
   failure latch. Judged not worth a set; noted.
5. **CD and stream transitions** stay hard cuts by construction (resolver rejects them).
6. **Queue reordering UI and queue persistence** — untouched, not in scope.
7. **`crossfade_secs` config, schema, key bindings, `Ctrl+T`** — untouched. Queue
   crossfade follows `crossfade_secs` with no new setting.
8. **ThreadSanitizer over this code** is enabled by C4 but not run by it. If Dos wants
   the race class closed by tooling rather than by review, that is a further slice.
