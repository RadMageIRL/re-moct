# Split-trim + Ad-aware split — PLAN (two slices, shared surface)

**Stage:** PLAN deliverable for both slices. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `cb8baf1`.
**Sequencing per the brief:** split-trim builds on approval; ad-aware
returns for DESIGN review (§2 answers below) before any code.

## 0. The shared split-decision surface (discovery)

All of it lives in StreamRecorder, worker + UI lanes only:

- **Trigger:** `onTitle` (src/StreamRecorder.cpp:127-137) — dedup + set
  `pending_raw_` + `split_pending_` under `meta_mtx_`.
- **Decision/act:** the worker loop's roll check (:545-551) — after
  draining the ring snapshot: `split_pending_ && cut_open_ &&
  cut_frames_ > 0` → `rollCut(false)` → adopt the pending title.
- **Relabel-in-place:** `adoptIfCutEmpty` (:508-515) for a flag arriving
  before any audio is attributed.
- Cut identity/naming/routing all resolve at `openCut()`/`rollCut()` on
  the worker. The audio-thread tap and the SPSC ring see none of this.

Both slices act here: **trim decides WHEN the roll happens; ad-aware
decides WHERE the cut goes (and whether it is written at all).** They
compose by construction — §2.2.4.

---

## SLICE 1 — split-trim (build on approval)

### 1.1 Mechanism — the hold is frame-deferral; no ring lookback (confirmed)

`split_offset_ms` converts to frames (`ms * 44100 / 1000`). On the worker's
split detection, instead of rolling after the current drain pass, arm
`hold_frames_remaining_`; subsequent drained frames keep attributing to the
OLD cut (they are arriving audio — the ring is untouched, nothing reaches
backward) until the countdown hits zero, then roll and adopt. Frame-based
(not wall-clock) deferral is real-time-equivalent at 44100 Hz AND
deterministic for the test. `stop()` during a hold finalizes immediately
(session end outranks the hold). A second title change DURING a hold
overwrites the pending title (its would-be cut is sub-offset-length by
definition); stated, not hidden.

**The signed knob, honestly split by mechanism:**
- **Positive (hold/lag) — the settled default direction:** pure deferral as
  above. Zero new buffering. Default **1200 ms** (inside the brief's
  1000-1500 band; Dos dials by ear).
- **Negative (lead) — requires a worker-side holdback, reported as the
  brief's §1.2/§1.3 tension:** cutting EARLIER than the flag means the last
  |offset| frames already encoded into the old cut belonged to the new one
  — unrecoverable after encode. A lead therefore needs a small worker-side
  delay line (frames held between ring-drain and encoder feed; the RING is
  untouched, the audio thread is untouched, but it IS new plumbing, ~30
  lines, active only when offset < 0). **Recommendation: ship slice 1 with
  the positive hold implemented and the config key signed as designed;
  negative values clamp to 0 with a log note until a lead is actually
  wanted** — the lead's holdback FIFO is a 15-minute add later if a real
  station demands it. Dos overrides if the full signed range should land
  now; both variants are specced above so the choice is a one-word
  greenlight.

### 1.2 Config + panel

- `split_offset_ms` config key (int, clamp 0..5000 under the
  recommendation; -5000..5000 if the FIFO ships), session-seeded like
  `rec_split` (Config.h/cpp parse+save chains, ctor seed beside rec_*).
- Panel settings view: a `Split hold : 1200 ms` row under the split
  toggle; **editable via `[T]`** through the existing input bar (a new
  `InputMode::RecOffset`, the RecDir pattern — prefill, clamp on submit,
  reopen panel). Cheap enough that read-only would save nothing.

### 1.3 Honest limit (CHANGELOG + panel footer)

Cuts approximate the broadcast; edges reflect the station's own
transitions (segues/crossfades have no clean seam to find). The hold
protects outros from typical metadata earliness; it cannot manufacture
sample-clean seams. Comfort adjustment, not correctness.

### 1.4 Verification

- `stream_record_test`: offset N>0 → the flag's cut rolls exactly
  N*44100/1000 frames later (previous cut longer by exactly that, next
  cut's frames start after it — frame-exact via the existing CutInfo
  accounting); offset 0 == today's behavior (the regression anchor,
  existing scenarios unchanged); stop-during-hold finalizes cleanly.
- Live dial-in (Dos, by ear): outros survive at 1200 ms on a couple of
  stations — the tuning gate; the number is empirical.
- ctest both toolchains; CHANGELOG user-facing (hyphens). No Joan Osborne
  gate (recorder-only).

---

## SLICE 2 — ad-aware split (DESIGN REVIEW — the §2.2 answers)

### 2.1 The signal discovery that reshapes (and simplifies) the design

**The iHeart structural Song/Ad/Live signal already crosses the ABI as a
string.** `IHeartNowPlayingSM` (plugin-side since Phase 4) publishes the
LIVE-floor display — `"<Station> - LIVE"` (or bare `"LIVE"`) — whenever
its reconciler decides no song is playing (ads, talk, station segments;
plugins/stream/IHeartNowPlayingSM.cpp:39). Real songs publish
"Artist - Title". And `looksLikeRealTrack()` (src/UIManager.cpp:4458)
already recognizes exactly that floor (`t == "live"`, exact match) plus
the junk vocabulary. So **the two signals CONVERGE host-side into ONE
string classifier with two signal qualities**: for iHeart the string is
structural (the SM already decided — reliable); for generic ICY the same
classifier is heuristic (title vocabulary — best-effort). **No ABI
change, no plugin change, no new plumbing** — the classifier is
`looksLikeRealTrack` on the cut's parsed title, hoisted to StringUtils.h
(the deinvertArtist precedent) so the recorder and UIManager share one
home.

### 2.2 The design answers

1. **What counts as an ad, per signal:**
   - Classified NON-SONG (→ ads/ on Save, omitted on Discard): the LIVE
     floor (`title == "live"` exact — iHeart's structural ad/talk signal,
     and any ICY station using the same convention); empty/Unknown artist
     or title on a PARSEABLE "A - B" string; the ad vocabulary
     (`ad|`/`commercial break`/`advertisement`/`station id`/`spot
     block`/…) in either field.
   - **Unparseable titles (no " - ") stay in MAIN with today's timestamp
     fallback** — deliberately conservative, diverging from "non-song →
     ads/" for this one class: a real song on a title-only station must
     never route to ads/ (Save) or be dropped (Discard). Known
     false-positive mode that remains: a real song literally titled with
     ad vocabulary classifies non-song — the informed Discard cost the
     brief already accepts, and on Save it is merely misfiled.
2. **Live/talk:** → `ads/` (as the brief recommends — the folder's
   semantic is "not a song, prune me"; a third bucket buys nothing).
3. **Transitions:** already covered — the split fires on ANY published
   title change (onTitle dedup), so song→ad (title → floor/ad string) and
   ad→song each split naturally today; no split-logic extension. Each
   metadata-distinct ad segment becomes its own small cut, routed by
   class.
4. **Composition with split-trim:** trim = WHEN (the hold defers every
   roll uniformly, class-blind); ad-aware = WHERE (class evaluated at
   `openCut` from the adopted title, deciding path/suppression). No shared
   state beyond the roll sequence; a held boundary simply delays when the
   next class takes effect by the same offset. Verified together in the
   test (§2.4).
5. **Naming on Save:** `recordings/<Station>/ads/` (created on first
   use), `<yyyy-mm-dd HHMMSS> - <sanitized raw label | "ad">.<ext>` —
   sortable, self-describing; existing `sanitizePathComponent` +
   collision-suffix machinery reused verbatim.

### 2.3 Mechanics (worker-only)

- Toggle: `[A] Ad segments : Save / Discard` in the panel settings view +
  `rec_ads` config key (`save` default / `discard`), seeded like the rest.
- Classification at `openCut` on the cut's raw title: song → main path as
  today; non-song + Save → the ads/ path + timestamp-label name; non-song
  + Discard → a SUPPRESSED cut: no encoders open, drained frames are
  dropped after ebur/convert are skipped, counters still advance, CutInfo
  recorded with a `discarded` flag (the panel can show "ads skipped: N",
  and the honesty surface survives). Never destroys on Save — routing
  only; Discard omits only positively-classified non-songs.

### 2.4 Verification (refined at design review)

- `stream_record_test`: scripted song/ad(live-floor)/ad(vocab)/song
  sequence → Save routes non-songs to ads/ with sortable names and songs
  to main; Discard omits the non-songs (no files, `discarded` CutInfo),
  keeps songs; unparseable-title cut stays in main both modes; composes
  with `split_offset_ms > 0` (boundaries deferred, routing unchanged).
- Live (Dos): an iHeart station (structural floor) and a generic ICY
  station (heuristic) — routing/omission per toggle; false-positive rate
  livable; Save demonstrably never loses a song.
- ctest both toolchains; CHANGELOG (hyphens; states Discard trusts
  station metadata and can rarely mis-drop).

## 3. Divergences (both slices)

1. **The signed-knob tension (slice 1):** the brief's §1.2 wants one
   signed knob but §1.3 forbids reaching backward — a lead inherently
   needs a worker-side holdback delay line (not ring lookback, but real
   plumbing). Recommendation: positive-only now, signed config reserved,
   FIFO specced for when a real station needs it. Dos picks.
2. **Dual-signal collapses to one classifier (slice 2):** the iHeart
   structural state arrives pre-encoded in the published string — better
   than hoped (no ABI work), but it means iHeart ad detection is only as
   good as the SM's floor decisions (the reconciliation work says: good).
3. **Unparseable titles stay in main (slice 2)** — deliberately more
   conservative than the brief's "non-song → ads/" for that one class.
4. `looksLikeRealTrack` hoists to StringUtils.h (the deinvertArtist
   precedent) — UIManager's scrobble caller unchanged.
