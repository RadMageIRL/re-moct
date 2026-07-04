# RE-MOCT - iHeart Radio: Algorithms, Functions & Lessons Learned

> Reference for a local Claude agent working on RE-MOCT's iHeart digital-stream
> handling (`StreamSource.*`, `IHeartRadio.*`, `IHeartDeepLog.*`).
> **Legend:** ✅ confirmed from logs/code · 🟡 strong hypothesis (not fully proven)
> · ❌ ruled out / dead end · 🔧 current mechanism in code.
> Test station throughout: **zc4366 "Sacramento's Relaxing Favorites"** (a
> "manifest-primary" station - see §3). Behavior differs by station class.

---

## 1. How RE-MOCT pulls iHeart (architecture)

- iHeart live stations are **HLS** (`.m3u8` + AAC segments) served from
  `*.revma.ihrhls.com`. Canonical entry: `https://stream.revma.ihrhls.com/zc<id>/hls.m3u8`.
- ✅ **Two renditions:**
  - **Raw broadcast** (default) - plain `hls.m3u8`.
  - **Digital / web-player** ("Ctrl+K" mode) - same URL with the web-player
    handshake query params appended (`hlsBuildDigitalUrl`). This is the **same
    stream the iHeart web player uses**, *not* a raw-only bypass. (Earlier notes
    that called digital mode a "raw-feed bypass of SSAI" were **wrong** - see §8.)
- 🔧 Producer thread pulls segments over WinINet → FDK-AAC decode → resample
  48k→44.1k → int16 SPSC ring → audio callback (`readFrames`). Mirrors CDSource.
- 🔧 `hlsPollMedia` re-fetches the media playlist on the target-duration cadence
  (~10s) and appends new segment URIs to `hls_.pending`; `hlsRawRead` drains them.

### The digital handshake (Ctrl+K)
Observed real web-player params (all on the variant URL):
```
aw_0_1st.playerid=iHeartRadioWebPlayer  clientType=web  deviceName=web-mobile
dist=iheart  host=webapp.US  pname=live_profile  playedFrom=157
listenerId=<32hex>  stationid=<id>  terminalId=159  territory=US  us_privacy=1-N-
```
- ✅ RE-MOCT sends a fresh random `listenerId` and **no** `profileId`/`skey`.
  The real web player sends a stable real `profileId`/`skey`, empty `listenerId`.
- ❌ **Handshake identity is inert for ad *content*.** A 4-variant concurrent
  capture (baseline / web-player-clone / emptyid / synthetic profileId) returned
  **byte-identical** manifests at the same instant. `profileId`/`skey`/`listenerId`
  do **not** change what's served at a given moment. Don't try to "crack" profileId.

---

## 2. Metadata sources & reliability

Three independent channels, **only one is audio-aligned**:

| Source | What it gives | Reliability | Timeline |
|---|---|---|---|
| **Manifest EXTINF** (`song_spot`, `title`, ids) | per-segment class + title | primary on manifest-stations | **~live edge (ahead of audio)** |
| **trackHistory / currentTrackMeta** (`currentTrackMeta` JSON API) | current track + album art | often **HTTP 204** (empty) on this station | ✅ **audio-aligned** ("rides the same timeline as audio") |
| **In-band ID3** (in AAC segments) | - | ✅ useless here: `tag found (ver=2.4 size=63) but no TIT2/TPE1` | n/a |

- ✅ Album art is sourced from **currentTrackMeta (`ctm.imagePath`)**, gated on a
  title-match against the committed label. Because ctm is audio-aligned, **art is
  already correctly timed**; the *label* was the thing running ahead (see §6).
- 🟡 currentTrackMeta returning 204 a lot is why the LIVE-floor logic exists - when
  ctm is blank and the manifest is ambiguous, we have no clean now-playing.

---

## 3. Manifest EXTINF structure (the key data)

On manifest-primary stations the media playlist tags **every segment** in its
EXTINF `title`/`url` attributes (quotes are backslash-escaped in the raw body:
`song_spot=\"T\"`):

```
#EXTINF:10,title="Local Driver Tech Inc.",artist=" ",
  url="song_spot=\"T\" spotInstanceId=\"66332184\" length=\"00:00:30\"
       cartcutId=\"9280676001\" amgArtworkURL=\"\" ..."
```

Fields RE-MOCT parses:
- ✅ **`song_spot`** - `M`/`F` = music, `T` = ad/spot. The primary class signal.
- ✅ **`spotInstanceId`** - `-1` = station id / promo / voice-track ("spot-id");
  a real id = a **paid** commercial.
- ✅ **`cartcutId`** - the ad's identity. **Same id repeating = a stuck loop;
  a sequence of distinct ids = a genuine long pod.** (Empty for music / boundary
  segments - the parser must handle empty values; see the `attr()` lambda, which
  was fixed so an empty `KEY=\"\"` doesn't swallow the next field name.)

### `cls` (derived class), logged in the poll line and deep log
`music` (song_spot M/F) · `spot-ad` (T, paid) · `spot-id` (T, id=-1, promo) ·
`other` (no recognized tag / slate) · `none`.

> ⚠️ **Station classes differ.** zc4366 carries rich per-segment `song_spot`.
> Z100-class stations report a permanent/uninformative manifest and need
> trackHistory as primary. Don't assume `song_spot` is present everywhere.

> ⚠️ Some **songs** arrive as `cls=spot-id` on this station (e.g. "Heart - These
> Dreams" played for minutes as `spot-id`). `spot-id` ≠ "is an ad break."

---

## 4. Now-playing reconciliation (`updateIHeartNowPlaying`)

🔧 A debounced state machine, enum `IHNow { Live, Song, Ad }`:
- **Target derivation** (priority): manifest `mfSong` (song_spot=M + title) →
  `cls==Ad` → "<station> - Commercial break" → trackHistory current → else
  **LIVE floor** ("<station> - LIVE").
- **Asymmetric debounce:** Song commits in 1 tick (trusted), Ad needs ~3 ticks
  (phantom-ad-during-song protection), Live ~2. Junk labels ("- LIVE",
  "- Commercial break") are not scrobbled.
- ✅ All of `mfSong`/`cls` come from the **newest** manifest segment = the live
  edge = **~one buffer-depth ahead of the audio** (see §6).

---

## 5. The re-pin / LIVE-floor self-heal 🔧

- During an ad break, metadata goes blank → state floors to LIVE.
- `LIVE_STALL_MS` (currently **35s**) timer: once the floor has been held that
  long, request a **live-edge re-pin** (`hls_repin_pending_`).
- Re-pin = **re-handshake to a fresh variant + `ringClear()` flush + re-prime**
  to the live edge. Rejoins the live program in step with where broadcast is now.
- ✅ Works: it reliably escapes long breaks and lands in a song. The overnight
  run (16 re-pins, ~9h) showed clean recovery, scrobbles landing, no runaway.
- 🔧 **Re-pin fade-in** (`FADE_IN_FRAMES`, ~0.5s): after `ringClear`, `readFrames`
  ramps gain 0→1 so the new song eases in instead of a hard cut. Only touches the
  real-audio path (not the prebuffer-silence path), so it can't burn off on silence.
- 🔧 `ringClear()` is the canonical re-pin reset hook: it also resets the label
  publish queue (§6) and arms the fade-in.

### Why "we lose the front of the song" on a re-pin - ✅ measured
On a break, the **primary session's own edge stays `cls=other` (slate) at
`edgeLag=0`** the entire floor - it never reveals the returning song. The song
only appears via the **fresh handshake** done by the re-pin itself. So:
- A "music-edge trigger reading the *primary's* cls" **cannot work** - the primary
  never sees music until it re-pins. (This is the SSAI stale-session effect, §8.)
- How far into the song we land = how long after the song actually went live at
  the edge the **35s timer** happened to fire. Random alignment → variable loss.
- ⚠️ By-ear loss estimates are inflated by the **display/Discord publish-delay
  (~10–15s, by design §6)**; true audio loss is smaller. Trust log timestamps,
  not by-eye counts.

---

## 6. Display vs audio timing - the publish-delay 🔧

- The label commits off the live edge (ahead). To align the **TUI + Discord +
  scrobble** with what's actually heard, each committed now-playing string is held
  in a queue and released after the **measured buffer depth** (ring seconds +
  undecoded pending). Self-calibrating; reset on `ringClear` (re-pin) and on
  `open()` (source switch).
- Result: label/Discord/art/scrobble all move together, ~a buffer behind the edge
  = aligned to the ear. Art (ctm) was already audio-aligned, so they now agree.
- 🟡 Residual: at a song→song seam the art gate (title-match on the live-edge
  label) can briefly show the logo with the correct title. Cosmetic.

---

## 7. Instrumentation (read these to analyze)

**Poll line** (`[stream] hlsPollMedia`):
```
target=10000ms mseq=… total=3 new=N disc=D pending=P newest=… nextPlay=…
edgeLag=L(~Ls) ringSec=R cls=… pdt=0 digital=1 armed=A
```
- `edgeLag` - segments behind our own manifest's newest (self-relative, ~10s each).
  Healthy/normal-break = 0–1. **Does not climb on normal breaks.**
- `ringSec` - seconds of decoded audio buffered (consume-side margin, ~5–6s steady).
- `cls` - newest-segment class (§3). `armed` - re-pin one-shot armed/cooldown.
- `pdt` - `EXT-X-PROGRAM-DATE-TIME` present. ✅ **Always 0 on this station** →
  absolute pdt-vs-wallclock drift is **not measurable** here.

**Deep log** (Ctrl+A, NDJSON, `%APPDATA%\RE-MOCT\logs\remoct-deep-analysis-*.log`):
- One record per reconciliation tick; first line `_meta`. 30s heartbeat + on-change.
- Key fields: `mfCls`, `mfSong`, `mfSeq`, `connectSeq` (**= session epoch, bumps
  per re-pin**), `cartcutId`, `spotInstanceId`, `spotPaid`, `audioSec`/`posSec`
  (age proxy), `thEnded`, `ctm*`, `writeReason`.
- `cartcutId` is in the **change signature**: distinct ads each write a record
  (dense); a stuck loop holds one id → heartbeat-only (sparse). The write *density*
  itself encodes loop-vs-pod.

**Staging-peek log** (Stage A, see §9):
```
staging: arming parallel digital session
staging: open OK, prebuffering parallel session
staging: peek cls=<music|ad|other|none> prebuf=<0|1> +<N>s   ← per poll while armed
staging: READY at music edge +<N>s - clean blend point reached
staging: primary break cleared (no blend needed) -> teardown
```
The `+Ns` on `peek cls=music` vs the 35s timer = the true, display-independent
measure of how much sooner a peek-driven re-pin could land in the song.

---

## 8. SSAI / stale-session behavior - the central insight 🟡→✅

- iHeart inserts ads server-side (SSAI) into the **same** stream the web player
  uses. We are on that stream in digital mode (§1).
- ✅ **The primary session's edge is served slate (`cls=other`) during a break
  while a fresh handshake gets the actual program.** Directly observed: primary
  `cls=other` at `edgeLag=0` for the whole floor; re-pin → fresh edge `cls=music`
  immediately.
- 🟡 **Why the web player never hits 15–20 min blocks but a long-running raw poll
  could:** session aging. A fresh/known session gets bounded, decisioned pods; an
  aged/anonymous session drifts toward longer house/slate fill. The web player's
  cap is likely a side effect of staying continuously registered (a periodic
  keepalive call we don't make). **Not yet proven** - needs the web player's full
  network trace through a break to find the recurring keepalive call.
- ✅ Overnight reality check: with the re-pin active, **no 15–20 min block ever
  appeared** (longest run ~5.7 min, of *distinct* paid ads = a genuine pod). The
  re-pins appear to keep things contained. cartcutId showed **distinct-pod** and
  **slate** signatures but **no stuck-loop** all night.

---

## 9. Staging lane (dual-stream peek) - current state 🔧

- 🔧 **Stage A (shipped, observer only):** on entering the LIVE floor, the
  coordinator spawns a second `StreamSource` (constructed `is_lane_=true` so it
  can't recurse and is side-effect-inert - scrobble/Discord poll the primary
  instance only; `is_lane_` also suppresses the lane's deep-log writes). It opens
  a parallel digital session on a detached thread (never blocking the primary
  producer), prebuffers, and watches **its own** fresh edge for `cls=music`.
  Audio path untouched; existing re-pin still drives playback.
- ✅ Overnight: lane **armed 11×, reached READY only 1×** - the primary's 35s
  re-pin/break-clear beats it ~10/11. The one success reached READY in ~20s.
- **Stage B (not built):** when the lane's fresh edge hits `cls=music`, **promote**
  (swap the lane's ring in) instead of tearing down - a clean swap landing on a
  music edge rather than a blind hard snap.
- **Stage C (not built):** equal-power crossfade over the swap (the only piece
  that touches `readFrames`/mixing - the real risk; do last).
- 🟡 Stage B is the *only* thing that can land near a song's start, because only
  the fresh peek sees the song before the timer (§5/§8). Whether it's worth it
  depends on the `peek cls=music +Ns` numbers (§7): big gap (20s+) on the breaks
  where we lose the most → build it; small gap (~5s) → leave it.

---

## 10. Dead ends & rejected approaches

- ❌ Cracking/cloning `profileId`/`skey`/`listenerId` - inert for content (§1).
- ❌ Gating the re-pin on `edgeLag` alone - `edgeLag` stays 0–1 through normal
  breaks *and* (apparently) through the slate floor, so it can't see the stall.
- ❌ "Music-edge trigger on the **primary's** cls" - primary edge is stuck on
  slate; never shows music pre-re-pin (§5).
- ❌ Faster manifest polling / staggered-peek machinery - parked; not needed yet.
- ❌ The 4×-fresh handshake capture for issue-1 root cause - measures fresh-vs-fresh
  at one instant, not aging over a long-running session. Wrong axis.
- 🟡 `bam.nr-data.net` POSTs in the web player = New Relic telemetry, **not** a
  stream control call. Red herring.

---

## 11. Current open items

1. **Staging `peek cls=music +Ns` data** (step-1 logging just added) - collect a
   session with several ad→song transitions; the spread of `+Ns` vs the 35s timer
   decides whether Stage B is worth building.
2. **Web-player keepalive hunt** (issue-1 root cause) - needs a full browser
   network trace of one web-player session held through a break, to find the
   *recurring* (not startup) iHeart API call we don't make.
3. **Stage B / C** - pending (1).
4. **True 15–20 min block** - still never captured with deep log armed (re-pins
   may be preventing it). If one occurs, read `cartcutId`: distinct = real pod
   (keepalive territory) · same repeating = stuck loop (re-pin escapes it).

---

## 12. Hard-won principles (apply when editing this code)

- Ring-buffer state transitions are subtle; `prebuffered_` + `ringClear()` in both
  producer re-pin paths. Changes to concurrency-sensitive paths need explicit
  justification.
- `readFrames` / `producerWorkerAAC` are the audio hot path - keep them
  byte-identical unless the change *is* the audio change. Verify with a diff.
- A second `StreamSource` instance is naturally side-effect-inert (scrobble/Discord
  poll the primary only); `is_lane_` covers the one global (deep log).
- Build on the files actually current; brace-balance + grep audit before handoff;
  prefer surgical diffs. Trust **log timestamps over by-ear estimates** (display
  lags audio ~10–15s by design).
- Distinguish ✅ confirmed from 🟡 hypothesis. Several past wrong turns came from
  treating a confident theory as fact - verify against a log before changing code.
