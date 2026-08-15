# RECON — streaming network recovery: what happens today

**Design note. Not greenlit. Untracked until it is.**

**Locked list.** This is the stream path, not the CD path, so the `AR_PREGAP` / `ar_crc.*` /
read-addressing declaration does not apply — using it here would be a false statement, not a
formality. The two locked items that *are* adjacent were read and **not touched**: the **audio
thread** (`readFrames`, reported below because it is where the silence comes from) and **dynamic
`libfdk-aac-2.dll`** (the AAC producer calls into it; nothing about its linkage is discussed).
**Recon only. No design, no fix, no change to any stream path.**

Branch `experimental/win-pdcurses`, tip `3861838`.

---

## S-R2 FIRST — what the user sees

**The UI does not claim `[LIVE]` during a drop. It says `[BUFFERING]`, which is honest — and then it
says `[BUFFERING]` forever, which is not.**

`UIManager.cpp:6104` is the whole of it:

```cpp
std::string right = audio_.streamBuffering() ? "[BUFFERING]" : "[LIVE]";
```

`streamBuffering()` → `PluginSource::buffering()` → `!prebuffered_`. Both producers clear
`prebuffered_` the moment they decide to reconnect, so the marker flips to `[BUFFERING]` at
detection. **So the worst case in the brief — "UI still shows playing" — is not quite what happens.**

**What happens instead is worse in one specific way: it never resolves.** When the producer gives up
(§S-R1), the thread returns. Nothing sets `stream_mode_` false, nothing sets `state_` back to
`Stopped`, nothing clears `prebuffered_`. The result is a terminal state that looks exactly like a
transient one:

| | during retries | after giving up |
|---|---|---|
| stream bar | `[BUFFERING]` | **`[BUFFERING]`, permanently** |
| KITT scanner | sweeping | **still sweeping** — it animates off the UI tick, not off data |
| `state_` | `Playing` | **`Playing`** |
| audio | silence | silence |
| anything saying why | **no** | **no** |

**Three separate mechanisms exist that could have said something, and none of them fires.**

1. **`last_error_` is set and never read.** `producerWorker` / `producerWorkerAAC` both write
   `last_error_ = "stream lost (max reconnect attempts)"`. It crosses the plugin ABI — the adapter
   implements `sp_last_error` (`StreamPluginAdapter.cpp:94`) and the vtable binds it (`:160`). **The
   host never calls it.** `PluginSource` has no error accessor at all; a grep of
   `include/PluginSource.h` and `src/PluginSource.cpp` for `last_error` returns nothing. The message
   is written, marshalled across the boundary, and dropped.
2. **The failure toast exists and is connect-only.** `showTrackToast("Radio stream connect FAILED", …)`
   at `UIManager.cpp:1629`, latched by `takeStreamFailed()`. Every site that sets that latch —
   `AudioManager.cpp:1302`, `:1387`, `:1409` — is inside the **initial** connect worker. **Losing a
   stream that was already playing never sets it.**
3. **There is no stream health watchdog.** The CD path signals `track_ended_flag_` when playback
   stops (`AudioManager.cpp:813-815`); the stream path has no equivalent. Every
   `stream_mode_.store(false)` is an explicit user action — stop, station switch, `openCD`.

**Net:** a dead stream is indistinguishable from a slow one, for as long as the app is left running.
The one honest signal it does give — `[BUFFERING]` — is also the signal for "wait a moment", so the
information it carries is exactly the information the user cannot use.

---

## S-R1 — what happens on a read failure, per transport

**There are two transports, not three.** `radio-browser.info` is a **directory**, not a transport:
`RadioBrowser` returns `url_resolved` (`include/RadioBrowser.h:9`) and those URLs enter the same two
paths as anything else. Mode is chosen in `StreamSource::open` (`:59-73`) purely by URL text —
`.m3u8` → `Mode::HLS` with the codec **forced** to AAC; everything else → `Mode::Continuous`, codec
by whether "aac" appears in the URL.

### Both producers: identical retry, and it is real

| | value | where |
|---|---|---|
| trigger | a read returning 0 | `producerWorker:1718` (MP3), `producerWorkerAAC:1784` (AAC/HLS) |
| bounded? | **yes — 10 attempts** | `if (++reconnect_attempts > 10)` |
| backoff | **linear, `500 × attempt` ms** | `port::sleepMs(500 * reconnect_attempts)` |
| reset | on **any** successful read | `reconnect_attempts = 0` |
| on exhaustion | `last_error_` set, `playing_` false, **loop breaks** | — |

**Total backoff across a full run: 500+1000+…+5000 = 27.5 s**, plus the connect attempts themselves.
So a genuinely dead network is abandoned after roughly half a minute, silently.

Each attempt is a full teardown and fresh handshake — `uninitDecoder()` (MP3 only), `disconnect()`,
then `connect()`. `disconnect()` also drops the HLS keep-alive session (`:365`), so an HLS retry
re-does master resolve, variant poll and priming from scratch.

### Detection latency before the first retry even starts

- **ICY, Windows:** `INTERNET_OPTION_RECEIVE_TIMEOUT = 8000` ms (`:294-297`). A silently wedged
  socket takes up to **8 s** to surface as `got == 0`.
- **ICY, Linux:** the same 8 s, hand-rolled — `curl_easy_recv` in 100 ms poll slices until
  `(long)(tickMs() - start) >= 8000` (`:1446`).
- **HLS:** session timeout is also **8 s** (`hlsEnsureSession:379`), per GET.

### HLS has a second, nested retry the ICY path does not

`hlsRawRead:1340-1354`, before the producer ever sees a failure:

```cpp
if (!hlsPollMedia()) {
    if (!hlsResolveMaster() || !hlsPollMedia()) {
        if (++repoll_fail >= 3) { ...; return 0; }   // give up -> producer reconnect
        port::sleepMs(300);
    }
}
```

A variant-playlist failure is treated as an **expired session token** and answered by re-resolving
the master, up to **3 times, 300 ms apart**. That is a genuine recovery path and it is the closest
thing in the tree to network-loss handling — but it only covers *manifest* failure.
**`hlsFetchSegment` failure has no inner retry at all**: `:1359` returns 0 immediately, with the
comment *"producer reconnect is the backstop"*.

---

## S-R3 — what is resumable

**The brief's premise is right about the protocols and wrong about this code: neither transport
resumes, and HLS throws away the state that would let it.**

### HLS — the position exists, and is discarded on every reconnect

Position lives in `HlsState` (`StreamSource.h:176-185`): `last_seq` (highest
`EXT-X-MEDIA-SEQUENCE` consumed), `pending` (queued segment URLs), `seg` + `seg_pos` (the current
segment and offset), `variant_url` (the tokenised playlist).

**`hlsConnect()` line 707 is `hls_ = HlsState{};`** — the first thing it does. Every reconnect
therefore re-resolves the master, re-polls the variant and re-primes near the **live edge**
(`:776-790`). `last_seq` is reset to 0 and never consulted for continuity.

So the segment-oriented resume the brief describes is **available in principle and not implemented**.
Whatever the drop cost, the listener is returned to live, and the gap is however long the outage was.

### ICY — nothing to resume, confirmed

`rawRead` reads into `raw_buf_` with `raw_pos_` as the offset (`:1407-1422` Windows, `:1433-1459`
Linux). That is a **transport buffer, not a position** — it is the 8 KB most recently received, and
`connect()` clears it (`:319-320`, `:349-350`). There is no byte offset, no sequence number, and the
protocol offers no way to ask for one. Reconnecting rejoins wherever the station is now. **Confirmed
as stated in the brief.**

### Consequence

Both transports currently behave identically on recovery: **rejoin at live, gap equal to the
outage.** The design difference the brief expected to hinge on is not present in the code today.

---

## S-R4 — the buffer

**The number: ~6 seconds of cushion, 3 seconds to refill. And it buys nothing, because the reconnect
path silences playback at detection rather than letting the ring drain.**

| constant | value | note |
|---|---|---|
| `RING_SECONDS` | **12** | `StreamSource.h:55` |
| effective cushion | **~6 s** | the producer backpressures at `RING_SIZE / 2` (`:1699`, `:1779`), so steady state is half-full — the header comment says exactly this |
| `PREBUFFER_SEC` | **3** | `:56`. No audio flows until 3 s has accumulated |

**Why the cushion does not help.** `readFrames` silence-pads whenever `!prebuffered_`
(`:1884-1887`) — and both producers call `prebuffered_.store(false)` *immediately* on deciding to
reconnect, before the backoff sleep. So:

- Silence starts at **detection** (≈8 s in), not at ring exhaustion.
- The ~6 s already buffered is **not played out**. It stays in the ring, unheard.
- On recovery, 3 s of fresh data must accumulate before `prebuffered_` flips back.

Set against the timings in S-R1, the arithmetic is one-sided: **detection alone (8 s) exceeds the
whole cushion (~6 s)**, so a drop is audible before the retry machinery has run once.

**And the ring is never cleared on this path.** `ringClear()` is called on the ad-onset re-pin
(`:1691`, `:1770`) with the comment *"drop stale buffered audio: jump to live + avoid backpressure
deadlock"* — but **not** on network-loss reconnect. So after a successful recovery the listener hears
up to ~6 s of **pre-drop audio** replayed before the live audio resumes. The two paths do the same
teardown and disagree about the ring.

---

## S-R5 — existing machinery

**Retry and backoff already exist, are wired, and are used** — §S-R1. What is missing is not the
mechanism but the reporting, the ring handling, and any notion of giving up visibly.

**The iHeart re-pin machinery does NOT apply, and it is worth being precise about why.** The
`hls_repin_*` family — `hls_repin_pending_`, `hls_repin_armed_`, the 90 s cooldown (`:543`), the
evidence tracker (`noteRepinEvidence` / `repinHardEvidence` / `repinSpotSegsInWindow`, `:911-940`),
the F6 mode ladder (`repin_mode_`, `:517-547`) — is triggered by **ad-break detection on the
manifest**, not by failure. None of its triggering logic has anything to say about a dead socket.

**What it does share is the teardown-and-rejoin mechanism**, and that part is directly comparable:
`uninitDecoder()` → `disconnect()` → `prebuffered_ = false` → `ringClear()` → `connect()`
(`:1685-1695`, `:1765-1778`). It is the same sequence the network-loss path runs, **plus the
`ringClear()` the network-loss path omits** and plus `hls_repin_active_` gating the
prime-to-music-boundary scan.

Also present, and connect-only: `stream_just_failed_` / `takeStreamFailed()` (§S-R2 item 2). The
staging lane (`serviceStaging:1168`, `is_lane_`) constructs a parallel `StreamSource` for
pre-warming — a second instance of all of the above, with its own cooldown.

---

## Noticed in passing — one line each, no action implied

- Three `-Wtype-limits` warnings in `StreamSource.cpp` (`:546, 1179, 1591`) are the LP64
  `(long)(u32 - u32)` trap already documented twice **in this file**; per `docs/warn-sweep-plan.md`
  §2 those cooldowns **never block on Linux**, and `:546` is the re-pin cooldown and `:1179` the
  staging cooldown — both in the machinery surveyed above.
- `AudioManager::takeStreamFailed()` is the only latch of its kind; there is no
  `takeStreamLost()` counterpart, which is why §S-R2 has nothing to consume.
- Codec selection is substring matching on the URL (`open:62`) — a station whose path happens to
  contain "aac" gets the AAC producer regardless of what it actually serves.
