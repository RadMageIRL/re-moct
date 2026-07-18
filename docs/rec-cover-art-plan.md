# Cover art per recorded cut — PLAN (rec-cover-art)

**Stage:** PLAN deliverable. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `76798ad`.

## 1. Discovery findings

### 1.1 The player's radio art lookup (§3.1)

`UIManager::startRadioArtFetch(artist, title, station_art)`
(src/UIManager.cpp:3792-3808): a single-in-flight detached thread
(`radio_art_active_` guard) that resolves the iHeart digital cover URL when
the station supplies one, else `CoverArt::urlBySong(artist, title)`
(iTunes → Deezer song lookup, include/CoverArt.h:30), then
`CoverArt::bytesByUrl` — which **already validates the body is a real
raster image** (guards HTML/JSON error pages, include/CoverArt.h:22-25).
Keyed on `artist + "\t" + title` (the scrobbler's committed-song identity).
Result lands in `radio_bytes_` / `radio_bytes_key_` with a negative cache
(`discord_art_neg_`) for known misses. Trigger + pickup live in
`refreshRadioArt` (:3813-…): change-detect on the polled now-playing,
start fetch, consume `radio_art_done_` into the cache.

### 1.2 Cached vs per-fetch (§3.2) — cached-reuse EXISTS but is pane-gated

`radio_bytes_` is exactly the cached current-song image the brief hopes
for — **but `refreshRadioArt` only runs while the Info pane's radio art
box is being drawn** (call site :3947, inside the pane draw). Record with
the playlist pane up and the cache never fills. So pure cached-read is not
sufficient on its own.

**Proposal — cached-reuse made pane-independent (no new provider, no new
network path):** extract `refreshRadioArt`'s trigger block and pickup
block into two small UI-thread helpers (`radioArtKick(artist,title)` /
`radioArtPickup()`), called exactly as today from `refreshRadioArt`, AND
from the existing per-tick recording wiring (the R2 `onTitle` site) while
a recording is active. Same single-in-flight guard, same negative cache,
same provider order, same thread — the fetch machinery just stops caring
whether the pane is open. On pickup, the tick hands the resolved bytes to
the recorder (below). This is the plan's one structural note: a small
UI-side extraction, zero engine change, zero new provider.

### 1.3 The recorder's tag pass (§3.3) — PREMISE CORRECTION

**`tagCut` embeds no art today** — grep finds no `addPicture`/`APIC` in
StreamRecorder.cpp. The brief's "the recorder's tag pass already embeds
art" is false; R1 explicitly excluded art (its plan §2). Both embed paths
get ADDED to `tagCut`, mirroring `tagFile`'s proven shapes verbatim:
- MP3: `ID3v2::AttachedPictureFrame` FrontCover (the :752-759 shape).
- Opus: `FLAC::Picture` FrontCover via `XiphComment::addPicture`
  (the :807-814 shape).
Small, reference-anchored, in scope per the brief's §4.4 spirit (it
anticipated the MP3 half; in fact both halves are absent).

### 1.4 Timing vs the cut roll (§3.4) — art typically resolves mid-cut

Sequence: title change → split → new cut opens → the same tick kicks the
art fetch → fetch resolves in seconds; songs run minutes; the cut closes
at the NEXT title change. So in the common case the cut's art is resolved
long before the cut finalizes → **attach at cut-close (tag time)**, no
post-close retag needed. Exceptions (very short cut, session stop seconds
after a split, fetch miss): the cut finalizes fully tagged, no art.

### 1.5 Off-thread (§3.4b)

The recorder never fetches. Fetching stays on UIManager's existing
detached art thread; the recorder receives bytes via a UI-thread call and
attaches them in the worker's tag pass (already off the audio thread).
The audio thread and the ring drain are untouched by construction.

## 2. Design (answers to §4)

- **Flow:** UI tick (while recording): poll now-playing → `onTitle`
  (existing) + `radioArtKick` for the same parsed artist/title (respecting
  the negative cache and station-art precedence) → on `radioArtPickup`,
  call the new `StreamRecorder::onArt(artist, title, bytes)` — stores
  `pending_art_{key, bytes}` under the existing `meta_mtx_` (UI↔worker
  only, never the audio thread; the `onTitle` pattern exactly).
- **Alignment rule (the important one): key-match at tag time.** `tagCut`
  embeds the pending art iff its key equals the closing cut's parsed
  (deinverted) artist+title key. A stale or early image whose key doesn't
  match the cut is NOT attached — **no art rather than wrong art**. Edge
  cuts inherit the documented 1-2s slop; the key rule means slop can cost
  a cover, never mislabel one.
- **Missing/failed/late art:** cut finalizes fully tagged without art —
  never a stall (the attach is a mutex-guarded vector read at tag time,
  the fetch is never awaited), never a drop, never an empty picture block.
- **Validation:** bytes non-empty + magic check (FFD8 → image/jpeg,
  89504E47 → image/png; anything else skipped) layered over
  `bytesByUrl`'s existing raster guard. tagFile hardcodes image/jpeg; the
  recorder sets MIME from the magic — strictly safer, same providers.
- **Format parity:** both Opus and MP3 per §1.3.

## 3. Hazards (brief §5) — answers

1. **Cut lifecycle:** attach = one locked read + a TagLib call in the tag
   pass that already runs post-finalize on the worker. Roll timing,
   dropped-frame behavior, teardown: untouched.
2. **Wrong-cut art:** structurally prevented by the key match (§2) —
   stated limit: an edge cut may LACK art it "deserved", never carry a
   neighbor's.
3. **Provider sprawl:** none — the recorder holds zero provider knowledge;
   UIManager's one path (station art → iTunes → Deezer, negative-cached)
   serves both the pane and the recorder.
4. **Malformed picture:** double-guarded (raster guard + magic/MIME).
5. **Off-thread:** §1.5; the audio thread's code path is byte-unchanged.

## 4. Verification (on greenlight)

- **stream_record_test extension (machine, both toolchains):** drive
  `onArt` directly — matching key → picture embedded in BOTH formats
  (TagLib read-back: MP3 APIC frame list, Opus `pictureList()`) with
  correct MIME; mismatched key → NO picture; garbage bytes → NO picture,
  cut still valid; no-art run unchanged. No network in tests.
- **Live capture (mainstream station, e.g. KKJO):** cuts land with correct
  embedded covers, visible in RE-MOCT's art pane on playback and in
  foobar2000/dBpoweramp — both formats. Recording with the Info pane
  CLOSED also gets art (the pane-independence point).
- **No-art case:** an art-miss station/segment → cuts finalize fully
  tagged, no art, no stall, split timing and drop counters identical.
- **Playback-uninterrupted** still holds during a capture with art
  fetching active (Dos's listen, rides the live run).
- ctest both toolchains; CHANGELOG user-facing (recordings now embed
  cover art; hyphens only).

## 5. Divergences

1. **Premise correction:** `tagCut` embeds no art today — both format
   paths are added (mirroring tagFile's proven shapes), not fed.
2. **Cached-reuse needs a small UI-side extraction** (§1.2): the fetch
   machinery is pane-gated today; `radioArtKick`/`radioArtPickup` make it
   recording-aware with zero provider/network change. UIManager-only; the
   engine lanes stay frozen.
3. The alignment rule chooses "no art rather than wrong art" — an edge
   cut may miss a cover under slop; stated, not hidden.
