# Stream recording with metadata-driven track splitting — PLAN (stream-record)

**Stage:** PLAN deliverable. Findings from the live tree, architecture, panel
design, decisions, verification, and a slicing proposal. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `2e53de7` (rip overhaul
complete; CI green on both matrix legs).

---

## 1. Discovery findings (§3 of the brief) — every anchor resolved

### 1.1 PCM is host-side; the ABI is untouched (§3.1) — CONFIRMED

The plugin hands the host **decoded PCM**, never compressed frames. The frozen
v1 ABI's audio entry is:

    uint32_t (*read_frames)(void* self, float* dst, uint32_t frame_count);
    — include/core/remoct_plugin.h:172, documented "stereo f32 @ 44100 Hz",
      audio-thread, lock-free, silence-pads while buffering/paused.

Host-side call chain: `AudioManager::onDataCallback` stream branch →
`stream_plugin_.readFrames(out, frame_count)` (src/AudioManager.cpp:604) →
`PluginSource::readFrames` → the loaded module's `read_frames`. The stream
device is initialised f32 / 2ch / 44100 (src/AudioManager.cpp:1263-1266).

**The re-encode tap point is src/AudioManager.cpp:604-608 — immediately after
`readFrames`, BEFORE volume/balance/EQ/mute.** The recorder captures the
undecorated broadcast signal, exactly the doctrine the CD live-BPM tap states
at src/AudioManager.cpp:634-637 ("analyses the undecorated signal — EQ would
otherwise reshape…"). User volume, mute, and EQ never color the capture.

Consequence for the later copy/remux slice (§10): compressed frames do NOT
cross the boundary today. Copy capture would need either an additive ABI
function (the versioning contract at remoct_plugin.h:205-214 explicitly
permits appending a fn pointer without a version bump) or plugin-side capture
via `set_config`. Assess then; nothing in slice 1 forecloses either.

### 1.2 Title changes host-side: poll-and-compare, and the published title is
### already boundary-aligned (§3.2) — CONFIRMED, with a bonus

There is no push event. Every consumer (scrobbler, Discord, radio art, toasts)
polls `audio_.streamNowPlaying()` on the UI tick and compares against a cached
key:

- Accessor: `AudioManager::streamNowPlaying()` → `PluginSource::nowPlaying()`
  (include/AudioManager.h:143, src/PluginSource.cpp:50) — an ABI
  `now_playing` call, host/UI-thread per the ABI's threading contract.
- Change-detect idiom: `UIManager::refreshRadioArt` (src/UIManager.cpp:3691,
  `song_key != radio_bytes_key_`) and `updateScrobbler`'s radio branch
  (src/UIManager.cpp:4617).

**Bonus finding:** `StreamSource::nowPlaying()` (plugins/stream/
StreamSource.cpp:1373-1383) returns the **published** title through
`np_pub_q_` — the iHeart reconciliation's release-tick publication queue.
iHeart titles surface when they are *scheduled to be audible*, not when the
API delivers them; ICY StreamTitle commits directly. Splitting on this same
polled value means the recorder inherits the desync-reconciliation alignment
for free and always agrees with what the UI/scrobbler shows. The split
trigger is therefore: **UIManager's tick polls (as it already does), detects
change, and hands the recorder the new title** — no plugin change, no audio-
thread involvement, one more consumer of an existing pattern.

### 1.3 IEncoder drops in clean (§3.3) — CONFIRMED, one seam note

`IEncoder` (include/IEncoder.h) is pure PCM-to-file: `open(path,
total_frames)` / `writeFrames(const int16_t* interleaved, size_t frames)` /
`finalize(bool ok) -> bool`. No CDRipper types in the interface; tagging/RG
are deliberately outside it. All five encoder headers are public in
`include/`. The factory `makeEncoder` is file-static in CDRipper.cpp:845 —
irrelevant: the recorder constructs `Mp3Encoder(vbr_q)` /
`OpusRipEncoder(bitrate)` directly (2 lines), no lift.

Seam note: **writeFrames takes s16; the tap yields f32.** The worker converts
f32→s16 (clamp + round, the exact inverse of the decode side's x/32768)
off-thread before feeding encoders. `total_frames` is 0 (unknown) — the
header documents 0 as valid (FLAC-only hint anyway).

`CDRipper::tagFile` (src/CDRipper.cpp:658) is **not reusable as-is** — its
signature is MBRelease/MBTrack/ARTrackResult/RGResult-shaped. The recorder
gets its own small tag pass reusing the same primitives tagFile proves:
TagLib `MPEG::File`+ID3v2 for MP3 (the :699-733 shape), TagLib
`Ogg::Opus::File`+XiphComment for Opus with **R128 dialect via the shared
R128Gain.h** (`r128FromDb`, the one-home rule — src/CDRipper.cpp:766-791 is
the reference; R128_TRACK_GAIN only, no album gain, no REPLAYGAIN_* keys).
Partial reuse, not the function — divergence from the brief's "existing tag
writers" phrasing, same spirit.

### 1.4 Free-key scan (§3.4) — full scan, recommendation Ctrl+E

Scanned both binding forms across src/UIManager.cpp (case labels AND
`if (ch == …)` forms) plus main.cpp.

Ctrl keys TAKEN: ^A(1 deep log) ^B(2 ListenBrainz) ^D(4 Discord) ^F(6 MB
search) ^G(7 Last.fm) ^K(11 digital/raw) ^L(12 redraw, UIManager.cpp:1383)
^N(14 nerd icons) ^P(16 identity probe) ^Q(17 quit) ^R(18 MB lookup)
^T(20 theme) ^U(21 stream URL) ^Y(25 rip).
Unusable as keys: ^H/^I/^J/^M (backspace/tab/enter aliases), ^C (SIGINT),
^Z (SIGTSTP on Linux — slice-1 termios kept ISIG), ^S (flow-control history;
IXON is cleared but PDCurses/Windows terminals vary — avoid).
FREE and terminal-safe: **^E(5), ^O(15), ^V(22), ^W(23), ^X(24)**.
Plain letters: effectively all bound (a-z and most A-Z appear as case labels).

**Recommendation: Ctrl+E** ("rEc" — the closest mnemonic the map allows),
mirroring ^Y's guard shape (src/UIManager.cpp:5636-5658): only when
`ui_overlay_ == None`; opens the panel when `audio_.streamMode()`, else a
hint-toast ("Ctrl+E: start a stream first (Ctrl+U)"). While recording, ^E
reopens the panel (status + stop), like ^Y-cancels-when-ripping.
Fallbacks if ^E displeases: ^O, ^W. Dos confirms.

### 1.5 Tap format (§3.5) — CONFIRMED

f32 interleaved stereo @ 44100 at the tap (device config
src/AudioManager.cpp:1263-1266; ABI contract). Ring carries f32 verbatim
(zero conversion on the audio thread); the worker converts to s16 for
IEncoder and feeds ebur128 from the f32 before conversion.

### 1.6 libebur128 per-cut RG (§3.6) — FEASIBLE, propose IN for slice 1

Already linked host-side (src/CDRipper.cpp:33). Per-cut =
`ebur128_init(2, 44100, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)` at cut
open, `ebur128_add_frames_float` per drained block (the :1166-1168 shape,
frame count NOT divided by channels — the recorded gotcha),
`ebur128_loudness_global` + true peak at finalize (:1263-1269), destroy.
All worker-thread, cheap, no album pass (no album exists — track gain only,
which is also all R128 Opus tagging wants). Recommend: in slice 1; it is
~20 lines against existing linkage and makes cuts volume-behave in every
player including our own decode side.

### 1.7 Output base dir (§3.7) — FOUND; needs a small extraction

`CDRipper::buildOutputDir(const MBRelease&)` — src/CDRipper.cpp:109-163
(public static; UIManager already calls it at :4918). The music root
(Windows `FOLDERID_Music`→USERPROFILE fallback; Linux XDG_MUSIC_DIR parse of
user-dirs.dirs → ~/Music) is computed INLINE at :110-151, then
`return music + kSep + "re-moct" + kSep + folder;` (:162).

**Plan: extract :110-151 into a new public static `CDRipper::musicRoot()`;
`buildOutputDir` calls it (behavior-identical — verbatim move), and the
recorder derives `musicRoot() + kSep + "re-moct" + kSep + "recordings"`.**
One root, both consumers, relocating Music moves rips and recordings
together. `sanitizePath` (the :95-107 static) is made accessible the same
way for station/artist/title sanitization. CDRipper.cpp is touched →
the trimmed CD gate runs at verification (see §7).

### 1.8 Unplanned finds worth having

- `UIManager::stationLabel(audio_.streamUrl())` (src/UIManager.cpp:4427) —
  the station display name (config station name, else URL-derived label).
  Source for the `<Station>/` directory; strip the "RADIO: " prefix,
  sanitize.
- `looksLikeRealTrack()` (src/UIManager.cpp:4458) — the existing junk-
  metadata gate (ad/station-ID markers, "LIVE" floor, Unknowns) and
  `deinvertArtist()` (:4441). Slice 1 reuses `deinvertArtist` for tag
  hygiene; `looksLikeRealTrack` is the natural seed for the later ad-aware
  slice (it already encodes the junk vocabulary).
- The RipConfirm modal input block (src/UIManager.cpp:4881-4938) +
  `drawRipConfirm` (:1666) + `UIOverlay` enum (include/UIManager.h:36) —
  the panel clones this idiom: a new `UIOverlay::RecPanel` value, one draw
  fn, one input block, structural disambiguation (Esc/cancel first, digits
  toggle-and-stay, commit letters guarded).
- Silence-pad reality: `read_frames` silence-pads during buffering AND
  pause. Slice 1 records what plays (honest timeline, silence included);
  see open question Q3.

## 2. The two scoping calls — both taken as recommended

1. **Re-encode first (§0.1): TAKEN.** PCM is host-side f32 (§1.1); the whole
   IEncoder fan-out drops in (§1.3); zero ABI surface. Copy/remux stays a
   recorded later slice with the ABI-additive path noted.
2. **Lossy-only (§0.2): TAKEN.** Panel offers MP3 + Opus, default Opus
   (128k VBR = the existing `opus_bitrate` config default). Lossless rows
   deliberately absent — encoding a lossy broadcast losslessly is a footgun.
   The engine keeps the `vector<unique_ptr<IEncoder>>` fan-out shape
   internally, so widening later is a table row, not a redesign.

## 3. Architecture

```
audio thread                      worker thread                UI thread
────────────                      ─────────────                ─────────
onDataCallback stream branch      StreamRecorder::workerLoop   tick: poll
  readFrames(out, n)                drain ring (f32)             streamNowPlaying()
  rec tap: if (capturing_)          convert f32→s16              on change →
    ring.push(out, n)  ← ONLY       ebur128_add_frames_float     recorder.onTitle(...)
    a bounded memcpy +              encoder->writeFrames        panel: arm/stop,
    2 atomic ops; full →            split_pending_? →            state readouts
    dropped_frames_ += n,             finalize+tag old cut,
    NO block, NO alloc                open new cut
  volume/EQ/viz/mute (unchanged)
```

- **`StreamRecorder`** (new: include/StreamRecorder.h, src/StreamRecorder.cpp)
  owns: the ring, the worker thread, encoders, the tag pass, ebur128 state,
  naming, counters. Audio-thread API: `capture(const float* frames,
  uint32_t n)` — lock-free push. UI-thread API: `start(RecOptions, station,
  dir)` / `stop()` / `onTitle(artist, title, raw)` / state accessors
  (recording, current cut, elapsed, cut count, dropped frames, bytes).
- **Ring:** SPSC lock-free, preallocated `float[2^20]` samples
  (= 524,288 frames ≈ **11.9 s**, 4 MiB), power-of-two masking, atomic
  head/tail (acquire/release) — the same discipline as the viz ring
  (`pushVizSample`, src/AudioManager.cpp:486) and the plugin's own audio
  ring. The worker (woken ~every 50 ms, and by a cv nudge on push… no —
  plain 50 ms poll sleep, no cv: the audio thread must not notify) drains
  continuously; Opus/LAME encode many× realtime, so 11.9 s of headroom is
  generous.
- **Full-ring policy: drop-incoming.** If free < n frames, the block is not
  written; `dropped_frames_ += n` (relaxed atomic). The file stays valid
  (a skip, not corruption); the panel shows the count — degradation is
  visible, never silent. Overwrite-oldest is rejected (would corrupt the
  already-ordered queue for zero benefit).
- **The tap is provably non-blocking:** one branch on a relaxed atomic bool
  (`capturing_`), one bounded memcpy (≤ frame_count×2 floats — ~4 KB at a
  typical 512-frame callback), two atomic stores. No lock, no allocation,
  no syscall. Strictly lighter than the EQ biquad pass and the per-sample
  `pushVizSample` loop already running in the same branch.
- **Split:** `onTitle` stores {artist,title,raw} under a small mutex
  (UI↔worker only — never the audio thread) and sets `split_pending_`
  (atomic). The worker checks between drained blocks: finalize current
  encoders → tag the finished cut with the *outgoing* cut's metadata → RG
  from its ebur128 state → open the next cut under the new title. Split
  granularity = one drained block (≤50 ms) — noise against the inherent
  broadcast slop (§5).
- **Lifecycle / teardown ordering:** `stop()` sets a flag, joins the worker,
  finalizes+tags the last (partial) cut. AudioManager calls
  `recorder_.stop()` in every stream-teardown path BEFORE
  `stream_plugin_.close()` (stop(), station switch/connectStream, dtor —
  the `stream_mode_.store(false)` sites at src/AudioManager.cpp:124, 385,
  1105, 1197, 1257; enumerated at IMPLEMENT). The audio thread's tap is
  guarded by `capturing_`, cleared first, so the ring is quiet before the
  join — no teardown race class.
- **Ownership:** AudioManager owns the `StreamRecorder` member (the tap
  needs it in the callback); UIManager drives it through thin AudioManager
  accessors, mirroring how rip state flows today.

## 4. The [Rec] panel (UIOverlay::RecPanel)

Clone of the RipConfirm idiom (open guard, full-input intercept, structural
disambiguation):

```
┌─ Record stream ──────────────────────────────────────────────┐
│  Station : KWIN 97.7                                         │
│  [1] MP3   V0                                                │
│  [2] Opus  128k VBR          ← default                       │
│  [S] Split on track change : ON                              │
│  [D] Dir : ~/Music/re-moct/recordings/                       │
│                                                              │
│  Enter/Y start    N/Esc close                                │
│  (cuts split on station metadata; boundaries are ±1-2 s)     │
└──────────────────────────────────────────────────────────────┘
```

- **Format = single-select** (digits 1/2 move the selection; not the rip
  modal's multi-toggle). Rationale: recording both formats doubles encode
  work and disk for the same lossy source — no real use; the engine
  underneath keeps the fan-out vector so this is a UI policy, not a
  capability limit. (Proposal per the brief's "propose which".)
- **S** toggles split-on-metadata (OFF = one continuous file per session,
  timestamp-named). **D** opens the existing input bar (the `openInputBar`
  pattern) to override the dir for this session.
- **While recording**, the panel (reopened via ^E) shows state instead:
  current cut title, elapsed, cut count, dropped-block count (only if >0 —
  honesty line), approximate MB written; `X`/Enter stops, Esc closes the
  panel with recording continuing.
- **Ambient indicator:** a `[REC]` badge beside the stream's `[LIVE]` badge
  while recording with the panel closed — recording must never be
  invisible.
- Config seeds (defaults, session-overridable in the panel, matching the
  `rip_formats` pattern of Config.h:38-46): `rec_format` (opus|mp3, default
  opus), `rec_split` (default on), `rec_dir` (default = derived
  recordings root). Reuses `opus_bitrate`/`mp3` quality keys — one quality
  truth, no new knobs.

## 5. Split, tagging, naming, partials

- **Split trigger:** the polled published title (§1.2). Slop is inherent
  (~1-2 s, metadata-late broadcasters) — stated in the panel footer and
  CHANGELOG; no sample-accuracy implied. iHeart titles already ride the
  reconciliation queue; ICY commits immediately. Lead/lag trim is the
  recorded later slice.
- **Parsing:** first `" - "` splits "Artist - Title" (the scrobbler's exact
  idiom, src/UIManager.cpp:4617-4621); trim; `deinvertArtist` applied.
  "A - B - C" → artist "A", title "B - C" (first-dash rule, matching the
  scrobbler). Title-only/empty/unparseable → the cut still records; tags
  carry what exists (title-only → TITLE set, ARTIST empty); the FILENAME
  falls back to timestamp (below). No crash path, no empty-named file.
- **Naming (proposal: group by station):**
  `<musicRoot>/re-moct/recordings/<Station>/<Artist> - <Title>.<ext>`
  Station from `stationLabel()` minus the "RADIO: " prefix, everything
  through `sanitizePath`. Timestamp fallback
  `<Station> - <yyyy-mm-dd HHMMSS>.<ext>` whenever the parse yields no
  artist+title pair. Collisions (same song again in rotation): append
  ` (HHMMSS)` — never overwrite a previous cut.
- **Partials (proposal: keep + mark):** first cut (joined mid-song) and
  last cut (stopped mid-song) get a ` (partial)` filename suffix; nothing
  is dropped — the user prunes with full information. (Dropping the first
  partial silently discards audio the user asked to record; rejected.)
- **Tags:** MP3 → ID3v2 TITLE/ARTIST (+TENC "RE-MOCT v…"), REPLAYGAIN_TRACK_
  GAIN/PEAK TXXX (tagFile's MP3 RG shape); Opus → XiphComment TITLE/ARTIST
  (+ENCODER note) and R128_TRACK_GAIN via R128Gain.h (§1.6) — track gain
  only, both dialects from the one home. No album, no track number, no AR
  fields — absent because they're false, not forgotten. Station name goes
  in a COMMENT/TXXX ("Recorded from <Station> by RE-MOCT") — cheap
  provenance.
- **Never a master:** recordings are derived lossy captures by construction
  — flagged for the deferred log-semantics slice; recordings write no rip
  log in slice 1.

## 6. Hazards (§7) — answers

1. **Audio-thread boundary:** tap = branch + bounded memcpy + 2 atomics
   (§3); lighter than code already on that path. Proven by the listening
   gate + the induced-overflow test (below) + code inspection at review.
2. **Ring overflow:** drop-incoming + atomic counter + panel display; ctest
   forces it with a tiny ring + stalled worker and asserts the count,
   file validity, and that capture continues.
3. **Split slop:** panel footer + CHANGELOG state ±1-2 s; boundaries are the
   broadcaster's, not ours.
4. **Generation loss:** accepted for the use case; lossy-only output makes
   the tradeoff visible (no fake-lossless).
5. **ABI:** untouched — verified at the header level (§1.1); the recorder
   consumes host-side f32 that already exists. plugins/ has zero diff.
6. **Parse robustness:** §5 rules + timestamp fallback + unit cases
   (title-only, empty, multi-dash, whitespace junk, unicode) in the ctest.
7. **Disk:** unbounded by design (it's a recorder); elapsed + MB + cut
   count visible while recording; no auto-cap in slice 1.

## 7. Verification (to run on IMPLEMENT greenlight)

Machine gates:
- **New `stream_record_test`** (both platforms): drives StreamRecorder
  headless — a producer thread pushes a deterministic PCM pattern through
  `capture()` at paced blocks; scripted `onTitle` changes; asserts cut
  count, each cut decodes (TagLib/probe read-back), tag correctness per
  dialect (ID3 vs Xiph R128), split boundary within one drain block of the
  event, timestamp fallback on malformed titles, and the induced-overflow
  case (tiny ring + stalled worker → counted drops, valid files, no
  corruption).
- Full **ctest both toolchains** (Windows MSYS2 UCRT64 + WSL2 ~/rb).
- **Trimmed CD gate** (2-track [A], GHD3N, full TOC, cancel-at-3, canonical
  Relish CRCs a377572a/a0c5b3c2 conf 200 + byte-identity vs
  C:\Users\david\smoke baselines): required because CDRipper.cpp is touched
  (the musicRoot extraction) — proves the extraction moved nothing.
- ABI guard: `git diff --stat plugins/ include/core/remoct_plugin.h` = empty.

Hardware/eyes gates (Dos's, per standing rules):
- **Playback-uninterrupted (the defining gate):** record several minutes of
  a live ICY station AND an iHeart HLS station while listening — no glitch,
  stutter, or drop. Dropped-block count stays 0 in the panel.
- Cuts split at title changes (±slop), each cut tagged and playing back in
  RE-MOCT (the .opus/.mp3 decode paths just landed — dogfood both).
- Panel eyes-on: rows, defaults, [REC] badge, stop paths (panel X, station
  switch, stream stop, quit) each finalize a valid last cut.
- CHANGELOG entry (user-facing, hyphens only).

## 8. Slicing proposal

- **Slice R1 — the recorder engine, headless-proven:** StreamRecorder
  (ring/worker/split/tag/RG/naming) + the `CDRipper::musicRoot()` +
  `sanitizePath` extraction + `stream_record_test` + the AudioManager tap
  (capture flag default-off, nothing arms it). Machine gates only (ctest ×2
  + trimmed CD gate for the extraction). The engine's contract is frozen by
  test before any UI exists — the probe-first doctrine.
- **Slice R2 — the [Rec] panel + live gates:** UIOverlay::RecPanel, ^E (on
  confirm), draw + input blocks, title-poll wiring, teardown hooks, config
  keys, [REC] badge, CHANGELOG. Ends with the full hardware gate (ICY +
  iHeart live recordings, the listening pass).
- **Later (recorded, §10 of the brief, unchanged + one addition):**
  copy/remux (ABI-additive path per §1.1), ad-aware splitting (seed:
  `looksLikeRealTrack`, §1.8), per-cut cover art, split lead/lag trim,
  log-semantics tie-in.

## 9. Open questions for Dos (small; defaults proposed)

- **Q1 — hotkey:** Ctrl+E (recommended §1.4)? Alternates: ^O, ^W.
- **Q2 — format select:** single-select (recommended §4) vs rip-style
  multi-toggle?
- **Q3 — pause while recording:** slice 1 records the silence pad (honest
  timeline, simplest). Alternative: auto-stop capture on pause. Recommend
  record-through; revisit with the trim slice.
- **Q4 — per-cut ReplayGain in slice 1:** recommend IN (§1.6, ~20 lines);
  say the word to defer.
- **Q5 — partials:** keep + ` (partial)` suffix (recommended §5) vs drop
  the first partial?
