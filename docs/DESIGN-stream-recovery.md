# DESIGN — stream recovery: proposal

**Design note. Not greenlit. Untracked until it is.**

**Locked list.** Stream path, not CD path — the `AR_PREGAP` / `ar_crc.*` / read-addressing
declaration does not apply and using it here would be a false statement. Locked items in reach, and
what this proposal does with each:

| locked | this proposal |
|---|---|
| **The audio thread** | **Items 1 and 2 do not touch it.** Item 3 changes its observable behaviour without changing a line of it — declared in full at item 3, which then **stops**. |
| **`abi_version = 1`** | **Untouched.** Item 1 uses `last_error`, which is a **v1 slot** (`remoct_plugin.h:190`, above the appended-fields block at `:198`). No new field, no append, no bump. |
| **dynamic `libfdk-aac-2.dll`** | Not discussed. Its linkage appears nowhere in this. |
| **The re-pin machinery** | Read for comparison at item 2, **not modified**. |

Companion to `docs/RECON-stream-network-recovery.md`, whose findings are assumed rather than
repeated. Branch `experimental/win-pdcurses`, tip `3861838`.

---

## Item 1 — a dead stream stops looking like a slow one

### Which of the three mechanisms: `last_error`, and it is not close

- **The failure toast** is connect-only by construction. Its latch, `stream_just_failed_`, is set in
  three places, all inside the connect worker. Reusing it means setting a "connect failed" latch on
  something that is not a connect, from a thread that is not the connect worker — the name would
  stop being true, which is how a second meaning gets bolted onto a flag.
- **A new watchdog** would be the fourth mechanism the brief warned against. It would also have to
  invent a liveness predicate, and the only one available (`buffering()`) is exactly the signal that
  cannot distinguish "retrying" from "dead".
- **`last_error` already carries the answer, already crosses the ABI, and is dropped one function
  short of the screen.** The producer writes `"stream lost (max reconnect attempts)"`, the adapter
  marshals it (`StreamPluginAdapter.cpp:94`), the vtable binds it (`:160`). The only missing piece is
  a host-side accessor.

**It is also the only one of the three that already distinguishes the two states.** `last_error_` is
cleared in `open()` (`:48`) and written only on terminal failures. **Non-empty while `stream_mode_`
is true means the stream is dead** — not slow, not retrying. That predicate needs nothing invented.

### The shape

1. **`PluginSource::lastError()`** — one accessor, modelled line-for-line on `buffering()`
   (`PluginSource.cpp:46-48`), including the per-fn null check the ABI contract requires. A v1 slot,
   so every conforming plugin has it and old plugins keep working.
2. **`AudioManager::streamLost()`** — `stream_mode_ && !stream_plugin_.lastError().empty()`.
   Deliberately **not** a latch: it is a state, and a latch would need clearing rules that the
   existing `stop()` already provides for free.
3. **The UI tick polls it** in the block that already polls `takeStreamConnected()` /
   `takeStreamFailed()` (`UIManager.cpp:1622-1630`) — same place, same cadence, no new timer.
4. **On true: call `AudioManager::stop()`, then toast.**

### Why `stop()` rather than new teardown

`AudioManager::stop()` (`:464`, stream branch `:481-492`) already does exactly what "stopped, and it
says why" requires, in the right order:

```
endRecording();                     // finalize an in-flight capture FIRST
stream_plugin_.close();
stream_mode_.store(false);
state_.store(PlaybackState::Stopped);
ma_device_stop/uninit;
track_ended_flag_ / track_end_advanced_ cleared
```

**Reusing it fixes a second bug for free, and this is the part worth noticing.** Today a stream that
dies while recording leaves the recording *open indefinitely* — the tee stops receiving, nothing
finalizes it, and `endRecording()` is never reached because nothing calls `stop()`. Any teardown
written fresh for this would have to remember that; `stop()` already does it, first.

### What the user sees

`showTrackToast("Stream lost", <reason>, "")`, where the reason is the string the producer already
wrote. The stream bar stops rendering (the app is out of stream mode), transport shows Stopped, the
scanner stops sweeping because there is nothing to sweep for.

**The `[BUFFERING]` marker keeps its one meaning** — "wait a moment" — because the state it could not
distinguish itself from no longer persists. That is the whole of the complaint, answered by making
the ambiguous state terminate rather than by adding a second marker to disambiguate it.

### One correctness point that must be in the build, not discovered later

**`last_error_` is an unsynchronised `std::string`** (`StreamSource.h:291`). It is written by the
producer thread and would now be read by the UI thread — a data race, and the kind that survives
every test and shows up as a corrupt toast once a month. `now_playing_` next to it is guarded by
`now_playing_mtx_` for exactly this reason.

**Fix inside the plugin, no ABI implication:** guard `last_error_` with a small mutex (or reuse
`now_playing_mtx_`), set under lock in the producers, return a copy under lock from `lastError()`.
Entirely contained in `StreamSource`; the ABI still sees one `size_t(*)(void*, char*, size_t)`.

### Scope

Two new functions (one accessor, one predicate), one call site in the UI tick, one mutex inside the
plugin. **No change to the retry loop, the producers' control flow, the ABI, or the audio thread.**

---

## Item 2 — the `ringClear()` disagreement

### Which is right: the re-pin path's *intent*, at the network path's *moment*

The re-pin path is correct that stale audio must not be replayed. Copying its placement, however,
would be wrong — and this is the substance of the item.

**The two events are not the same event:**

| | ad-onset re-pin | network loss |
|---|---|---|
| outage | **none** — the socket is healthy, we chose to jump | **yes**, unknown length |
| buffered audio at teardown | **stale by choice** — it is the ad we are skipping | **wanted** — it is the last ~6 s of the programme |
| right thing to do with it | **discard immediately** | **play it, then discard the remainder** |

So `ringClear()` at teardown is right for the re-pin and wrong for network loss: it destroys audio
the listener has not heard yet and has every reason to want.

**Proposal: `ringClear()` on the network-loss path at reconnect SUCCESS, immediately before the first
write of fresh data** — not at teardown. That fixes the reported defect (up to ~6 s of pre-drop audio
replayed after recovery) while leaving the buffered audio available during the outage.

The two paths then agree on the invariant — *the ring never carries audio across a discontinuity into
live playback* — and differ only in when it is enforced, for a stated reason. **Symmetry of
placement would be the wrong kind of agreement.**

### Why the placement is the whole decision

**Clearing at teardown forecloses item 3.** There is no drain-the-buffer feature possible on top of a
path that has already discarded the buffer. Clearing at reconnect-success leaves item 3 buildable
unchanged.

This is the one place where doing item 2 the obvious way would quietly cost the more valuable item,
which is why it is called out here rather than in the commit that follows.

### Scope

One `ringClear()` call added per producer, positioned after a successful `connect()`. **Nothing
removed, no re-pin code touched, no audio-thread change.** Independent of item 1 and buildable in the
same pass.

---

## Item 3 — drain before silence: the shape, and then I stop

### LOCKED-CODE DECLARATION

**This proposal changes the observable behaviour of the audio thread. It changes no line of it.**

Both halves of that are load-bearing and I am not going to soften either:

- **No line changes.** `readFrames` (`StreamSource.cpp:1877-1904`) **already implements
  drain-then-buffer.** Its underrun arm — `if (got < samples_needed) prebuffered_.store(false);`
  (`:1894-1895`) — flips to buffering exactly when the ring actually runs dry, and its comment says
  so: *"Drop back to buffering so we refill instead of dribbling broken audio."* The mechanism the
  feature needs is already there and already correct.
- **The producers preempt it.** Both call `prebuffered_.store(false)` on *deciding* to reconnect,
  before the backoff sleep (`:1730`, `:1794`). That is what starts silence at detection instead of at
  exhaustion. The change is **removing that preemption from the producer threads** — outside the
  audio thread.
- **But the audio thread will do something it does not do today**: emit real frames during a network
  outage instead of silence-padding. Nobody is editing it; its behaviour changes anyway. Calling that
  "not touching the audio thread" would be true by the letter and misleading, and the letter is not
  what the rule protects.

**Assessed against what the lock actually forbids** — *"No blocking, no state mutation from outside
the defined paths"* — this adds no blocking and no new mutation: `prebuffered_` is already written by
`readFrames` itself on this exact transition. So it reads as within the defined paths. **That is an
assessment, not a ruling, and the ruling is Dos's.**

**Stopping here as instructed. Not built in the same pass as items 1 and 2.**

### What it would buy, since that decides whether it is worth a ruling

~6 s of cushion against a retry that starts at 500 ms. **A drop shorter than the cushion would be
inaudible** — the ring covers it and the listener never learns it happened. Against the current
behaviour, where silence begins at detection and the 8 s detection latency alone exceeds the whole
cushion, this is the difference between "most short drops are invisible" and "every drop is audible".

It also makes the `[BUFFERING]` marker more truthful in passing: while the ring drains, audio really
is playing, and the marker would correctly stay `[LIVE]` until the ring runs dry.

### The dependency, stated plainly

**Item 3 requires item 2 to place its `ringClear()` at reconnect-success.** If item 2 lands at
teardown instead, item 3 is dead and no ruling on it is worth making.

---

## Item 4 — HLS resume, report only

`hlsConnect()` discards `hls_ = HlsState{}` as its first act, so `last_seq` — the highest
`EXT-X-MEDIA-SEQUENCE` consumed — is never consulted on reconnect. Implementing resume would mean
preserving `last_seq` across the teardown, and after the fresh poll enqueueing the segments whose
sequence numbers follow it rather than priming at the live edge. The work is small in lines and
awkward in placement: `hlsConnect` is shared by first connect, ad re-pin and network reconnect, and
only the third wants this, so it needs a reason-for-connecting parameter that does not exist today —
and the re-pin path is off limits, which is precisely where such a parameter would have to be
threaded. **What it would buy is narrow and bounded by the server, not by us:** an HLS live window is
typically about three segments at `EXT-X-TARGETDURATION` (default 10 s here, `StreamSource.h:180`),
so roughly 30 s of history exists to resume into at all, and an outage longer than that has nothing
to return to. Since a drained ring (item 3) would already cover outages up to ~6 s invisibly, resume
only pays inside the band **between ~6 s and ~30 s** — and even there it buys continuity of
*content*, not of *time*: the listener falls behind live by the outage length and stays behind, on a
live radio stream, which is arguably not what they want. **Least urgent is the right call, and the
narrowness of that band is the argument for it.** Leaving it.

---

## Detection latency — inherited, then mirrored twice

**8000 ms was chosen once, for a different purpose, and copied to two places that never re-derived
it.** The original is `connect()` (`:292-297`), and its comment states the purpose plainly: *"Bound
the connect/receive so a dead or wedged station can't block the connect worker (and thus a queued
station switch) indefinitely."* That is about **UI responsiveness during a station switch**, not
about detecting mid-stream loss. The other two are explicit mirrors — the Linux ICY path *"mirroring
the WinINet RECEIVE_TIMEOUT failure"* (`:1429-1432`) and `hlsEnsureSession`'s `timeout_ms = 8000;
// mirror connect() timeouts` (`:379`). **No site chose 8 s as a loss-detection threshold; all three
inherited it as a connect bound.**

**Lowering it is two different questions wearing one constant, and they should be separated before
either is changed:**

- **The ICY read-idle timeout is the safe one to lower.** A Shoutcast/Icecast server sends
  continuously; a multi-second gap with no bytes is already abnormal, and the buffer that must cover
  it is only ~6 s. Something in the 2–3 s range would cut detection to well inside the cushion.
  The false-positive cost is bounded: a false trip costs one reconnect, and with item 2's ring
  handling that is a short gap, not a failure.
- **The HLS GET timeout is the dangerous one.** It bounds a *segment download*, and a segment is
  ~10 s of audio. On a slow link a legitimate fetch can genuinely take several seconds, so lowering
  this trades real stalls for manufactured ones — and each manufactured one currently burns a
  reconnect attempt out of the budget of 10. **Recommend leaving the HLS number alone** until
  someone measures real fetch durations on a slow connection.

**Recommendation: split the constant before touching the value.** One number currently means "connect
bound", "read-idle bound" and "segment-fetch bound"; no single value can be right for all three, and
changing it today changes all three at once.

---

## What this proposal does not do

No change to the retry loop, its 10-attempt cap or its linear backoff — the recon found them working
and the brief scoped them out. No re-pin code touched. No ABI change, no `abi_version` bump. No
`libfdk-aac-2.dll` linkage discussion. The three `-Wtype-limits` cooldown bugs and the URL-substring
codec selection stay logged and untouched.

**Buildable now, if approved: items 1 and 2**, independent of each other, one commit each.
**Item 3 awaits a ruling** and depends on item 2's placement. **Item 4 is left alone.**

---

## BUILT 2026-08-12 — items 1, 2, 3 and the constant split

All approved items landed, one commit each, gated on both toolchains. **One thing did not go as
proposed and it is the part worth reading.**

**Item 2's `ringClear()` was the wrong function, and the proposal did not notice.** The design said
"call `ringClear()` at reconnect success". `ringClear()` turned out to do two things: flush the ring
**and** snap the now-playing label / drop the scheduled-publish queue. The second half is an HLS
re-pin concern. `np_pub_q_` is fed only by the iHeart path, so on ICY nothing ever advances
`np_published_` and `nowPlaying()` falls through to `now_playing_`; setting `np_published_` there
makes it permanently non-empty with nothing to move it on, and **the station title would have frozen
at the pre-drop song for the rest of the session.** That is a worse defect than the six seconds of
replayed audio item 2 exists to fix.

`icy_pipeline_test` failed at the post-reconnect title assertion, reproducibly, twice in a row. The
fix was to split the name rather than add a parameter: `ringFlush()` is the ring half, `ringClear()`
is `ringFlush()` plus the label snap, and both re-pin call sites still call `ringClear()` with
identical behaviour — so the re-pin machinery stayed untouched as ruled.

**The lesson is recorded in `docs/lessons.md`:** a function whose name describes half of what it does
will be reused for the half it does not. Reading the body rather than the name is what the proposal
should have done, and the recon that preceded it had already quoted `ringClear()`'s comment without
reading past the first line of it.

Everything else landed as written. Item 3 changed no line of the audio thread and its behaviour
change was the declared one. Item 4 is recorded as declined in `docs/roadmap.md` with the band
argument. The four timeout constants are named and all still 8000.

**Not yet live-tested** — the hardware test is Dos's: pull the network mid-stream on ICY and on
iHeart HLS, short drop and long drop, and kill a stream while recording.
