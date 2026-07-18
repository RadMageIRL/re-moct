# Stream recording R2 — the [Rec] panel (surface) — PLAN

**Stage:** PLAN deliverable for slice R2. Findings from the live tree, the
panel design, wiring, hazards, verification. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `c333428` (R1 engine
merged). R1's engine is FROZEN — R2 calls its existing API; the single
sanctioned engine-file touch is the deinvertArtist adoption in the tag pass
(brief §1, explicitly ordered), nowhere near ring/worker/tap.

---

## 1. Discovery findings (brief §3) — all anchors resolved

### 1.1 The rip modal as template (§3.1)

`drawRipConfirm` (src/UIManager.cpp:1666) + the input block
(src/UIManager.cpp:4874-4938). **Reusable shape (cloned, not shared code):**

- Window mechanics: centered `newwin(BOX_H, BOX_W=68)`, `wbkgd` themed for
  Awesome vs Classic (:1682), `panelFrame(w, title, true)` + the manual
  Classic centered title (:1685-1691).
- The MAX_DIR path-truncation idiom (:1709-1712) — the Out-dir row uses it
  verbatim.
- Data-driven row block over a format table with digit hints (:1747-1768) —
  R2's is a 2-row single-select over the Mp3/Opus subset.
- Right-aligned summary + `CP_STATUS_ERR|A_BOLD` emphasis (:1735-1745) —
  reused for the dropped-frames line.
- Input disambiguation is STRUCTURAL (:4875-4903): N/Esc first (cancel
  always works), digits act-and-stay-open, letters commit. R2 keeps the
  shape; the rip modal's empty-selection guard complexity DROPS — a
  single-select radio always has exactly one format chosen.

**Rip-specific, not carried over:** drive/disc/MB info rows, the
[A]/[C]/[Y]/[B] mode block, the `cd_ripper_.start` commit tail, the
selection-normalization loop.

**Plumbing:** `UIOverlay` enum (include/UIManager.h:36) gains `RecPanel`;
draw dispatch at src/UIManager.cpp:1459-1461 gains one line; input intercept
beside :4874. One new `drawRecPanel()` + one input block.

### 1.2 ^E wiring (§3.2) — RE-CONFIRMED FREE

Re-scanned the live tree: no `case 5:` and no `== 5` key binding anywhere in
UIManager.cpp/main.cpp (UIManager was untouched by R1). ^E goes in the main
key switch beside ^Y, mirroring its guard shape (src/UIManager.cpp:5636-5658):

- `ui_overlay_ != None` → break (already showing a modal).
- `audio_.streamMode()` → open `UIOverlay::RecPanel` (settings view when
  idle, state view when recording — reopening while recording shows
  status + [X] stop, the ^Y-cancels-while-ripping spirit without making
  bare ^E a blind toggle-stop).
- else → hint, no panel (§1.7).

### 1.3 The StreamRecorder API the panel drives (§3.3) — SUFFICIENT AS-IS

include/StreamRecorder.h, all existing, no engine edit:
`start(const RecOptions&, station, out_dir)` (false + `lastError()` on
refusal), `stop()` (idempotent), `onTitle(raw)` (internal dedup; no-op when
idle or split-off), and the state surface: `recording()`, `droppedFrames()`,
`totalFrames()`, `bytesWritten()`, `elapsedSec()`, `cutIndex()`,
`currentTitle()`, `lastError()`, `cuts()`. Reached via
`audio_.streamRecorder()` (include/AudioManager.h). `RecOptions` carries
`formats` / `mp3_vbr_q` / `opus_bitrate` / `split_on_meta` — exactly the
panel's knobs. Start seeds the first label with
`onTitle(audio_.streamNowPlaying())` immediately after `start()` (the R1
harness pattern, now production).

### 1.4 The per-tick poll site (§3.4)

The main loop (src/UIManager.cpp:1105-1112): `audio_.pollEvents()` +
`updateScrobbler()` per tick. The split-trigger wiring is ONE statement
there:

    if (audio_.streamMode() && audio_.streamRecorder().recording())
        audio_.streamRecorder().onTitle(audio_.streamNowPlaying());

`onTitle` dedups internally (a repeat can never cut twice — R1 contract), so
unconditional per-tick calls are safe; cost is one `nowPlaying()` fetch the
scrobbler/art paths already make 2-3 times per tick. The panel's live
"current cut" row reads the same source.

### 1.5 The badge site (§3.5)

`drawTitleBar()` (src/UIManager.cpp:1917), the MOC-style modes string
(:1978-1994: `[REPEAT*]`, `[SHUFFLE]`, `[Q:n]`, `[TOAST]`, `[±n%]`). Add:

    if (audio_.streamRecorder().recording()) modes += " [REC]";

One relaxed atomic per frame. **Stale-badge impossibility is structural:**
there is NO UI-side recording flag — the badge derives from the recorder's
own `recording()`, which every R1 teardown hook already clears (stop, file
switch, openCD, station switch, dtor). Badge lifecycle == engine lifecycle
by construction (hazard §5.4 solved by design, verified live anyway).

### 1.6 deinvertArtist hoist (§3.6) — trivially behavior-neutral, one caveat

File-static at src/UIManager.cpp:4441-4456 with EXACTLY ONE caller
(:4663, `updateScrobbler`). Hoist = verbatim move to StringUtils.h as
header-inline (beside R1's `sanitizePathComponent`), caller line unchanged.
Pure function + single call site + identical body = behavior-neutral;
re-proven by ctest + the live scrobble path.

Adoption point in the recorder: **inside `parseNowPlaying`'s artist output**
(one home), so FILENAME and TAG stay consistent ("Shins, The - Song" →
`The Shins - Song.opus` with ARTIST "The Shins"). This touches
StreamRecorder.cpp's parse/tag line only — the §1-sanctioned exception; the
existing `stream_record_test` contract cases are unaffected (none use
inverted artists) and a new parse case pins the adoption.

**Caveat the brief should know (divergence §4):** `deinvertArtist`
de-inverts BARE ARTICLES only ("Surname, The/A/An" → "The Surname") — by
design, so "Tyler, The Creator" and "Earth, Wind & Fire" pass untouched. It
does NOT transform "Lastname, Firstname" (the brief's §6 example). Keeping
the guarded semantics as-is; the verification case becomes an article
inversion.

### 1.7 Panel-open precondition (§3.7)

The stream-active check is `audio_.streamMode()` (the gate every stream
consumer uses). With no stream playing, ^E emits a toast —
`showTrackToast("Stream recording", "Start a stream first (Ctrl+U)", "")`
(src/UIManager.cpp:314; the toast is the idiom in stream-land — cf. the
connect/fail toasts at :1124; ^Y's `rip_status_` line has no stream
equivalent). CD or local-file playback gets the same hint. The "Rip in
progress" toast (:964) is the tone template.

## 2. Config keys + session seeding

Mirrors the rip pattern exactly (config is load-once; the panel mutates
session state and never writes back — src/UIManager.cpp:225-228):

- `rec_format` = `opus` | `mp3` (default **opus**) → seeds `rec_fmt_sel_`.
- `rec_split`  = on/off (default **on**) → seeds `rec_split_`.
- `rec_dir`    = path (default empty → `CDRipper::recordingsDir()` derived
  at start time, so a music-root move is picked up live) → `rec_dir_`.
- Quality reuses the EXISTING `opus_bitrate` / `mp3` keys — one quality
  truth, zero new knobs (the rip modal's quality column idiom).

Parse in the Config.cpp else-if chain (:197 shape) + save block (:325
shape). The dir override edits via the existing `openInputBar` prompt (new
`InputMode::RecDir` case), prefilled with the current value.

## 3. The panel (drawRecPanel, BOX_W 68)

Settings view (idle):

    ┌─ STREAM RECORDING ─────────────────────────────────┐
    │  Station  KWIN 97.7                                 │   stationLabel()
    │  Out      ...\Music\re-moct\recordings\KWIN 97.7\   │   MAX_DIR idiom
    │                                                     │
    │  Format                              (1-2 select)   │
    │   (*) 1  Opus   128 kbps VBR                        │   radio, not toggle
    │   ( ) 2  MP3    LAME V0 VBR                         │   quality from config
    │  [S] Split on track change : ON                     │
    │  [D] Output dir (edit)                              │
    │                                                     │
    │  Note: pausing playback while recording leaves a    │
    │  silence gap - the paused-over airtime is not       │
    │  captured.                                          │   the R1 finding, plain
    │                                                     │
    │  [R] Record          [N/Esc] Close                  │
    └─────────────────────────────────────────────────────┘

Recording view (same overlay; ^E reopens to this while active):

    │  * REC   Dua Lipa - Don't Start Now                 │   currentTitle()
    │  elapsed 3:42    cuts 2    written 8.4 MB           │   atomics
    │  dropped frames: 1024                               │   ONLY if >0, CP_STATUS_ERR
    │                                                     │
    │  [X] Stop            [N/Esc] Close (keeps recording)│

- **Single-select justification (the brief's "propose which"):** digits 1/2
  move a radio marker — recording both formats doubles encode+disk for the
  same lossy source; the engine keeps the fan-out vector underneath, so this
  stays UI policy, reversible later without engine work.
- Live refresh: while `ui_overlay_ == RecPanel && recording()`, the tick
  loop requests a redraw at ~2 Hz (elapsed/cuts/size tick along); zero work
  when the panel is closed — the badge is the only ambient cost (one atomic).
- [R] start path: build `RecOptions` (selected format; `parseMp3VbrQ
  (config_.mp3)`; clamped `config_.opus_bitrate`; split toggle), station =
  `stationLabel(audio_.streamUrl())` sans the "RADIO: " prefix, dir =
  override or `CDRipper::recordingsDir()`; `start()`; seed
  `onTitle(streamNowPlaying())`; on false → toast `lastError()`, panel stays.
- [X] stop: `stop()` (finalizes the in-flight cut — R1 teardown discipline),
  panel flips to settings view with a "saved N cuts" line from `cuts()`.

## 4. Divergences / decisions to confirm

1. **deinvertArtist scope** (§1.6): articles only, deliberately — the §6
   "Lastname, Firstname" example overstates what the helper does (and
   should do). Verification uses an article case ("Shins, The - ...").
2. **Adoption point** is `parseNowPlaying` (filenames AND tags consistent),
   not the tag pass alone — one home, same §1 sanction.
3. New `InputMode::RecDir` case for the dir edit (the ^U prompt idiom).
4. Panel refresh at 2 Hz while open+recording (a tick-loop line) — the only
   per-tick addition beyond the badge atomic and the onTitle poll.
5. `rec_dir` default resolves at START time (not load time) so relocating
   the music root between sessions Just Works.

## 5. Hazards (brief §5) — answers

1. **UI-responsiveness:** ambient additions = one atomic in drawTitleBar +
   one deduped `onTitle` per tick while recording; panel work only while
   open, redraw-gated at 2 Hz. Nothing computes; everything reads R1
   atomics. Gated eyes-on (§6).
2. **State consistency:** the UI owns SETTINGS only (format/split/dir
   selection); every lifecycle datum (recording/elapsed/cuts/dropped/title/
   error) is read from the recorder per render. No duplicate state exists
   to drift.
3. **deinvert hoist:** verbatim move, one caller, ctest + live scrobble
   re-prove; new parse test pins the recorder side.
4. **Badge lifecycle:** derived, never stored (§1.5) — stale-on is
   structurally impossible; still verified across station-switch/stop/quit.
5. **Pause:** surfaced in copy (§3 panel note), behavior untouched — the
   keep-draining-while-paused plugin conversation stays recorded for later.

## 6. Verification (on greenlight; brief §6 + specifics)

- **UI-responsiveness (the R1-deferred gate, now drivable):** ^E-armed real
  recording, several minutes; scroll/panes/focus — zero perceptible lag.
  Eyes-on: Dos.
- **Playback-uninterrupted** through the real UI path: listen while
  recording via the panel. Eyes-on: Dos.
- **End-to-end via UI:** ^E → format/split/dir → [R] → real station → a real
  on-air split → [X] → cuts under `recordings/<Station>/`, valid, tagged,
  play in RE-MOCT. Both formats (one run each).
- **Badge:** [REC] on arm; clears on [X], station switch, ^U new stream,
  file/CD switch, quit — never stale (machine-assisted where possible, the
  teardown hooks are already engine-tested).
- **Precondition:** ^E with no stream → toast hint, no panel; ^E during CD/
  file playback → same.
- **Pause copy:** note renders in both themes; an actual pause-during-record
  produces the stated silence gap, no crash/truncation (R1 probe already
  pinned the mechanics).
- **deinvert:** scrobble path unchanged (existing behavior); a recorded cut
  from an article-inverted StreamTitle tags/names de-inverted; new
  stream_record_test parse case green both toolchains.
- **ctest both toolchains** (engine tests unchanged and green); **CHANGELOG**
  gains the user-facing stream-recording entry (hyphens only, slop stated
  honestly).

## 7. Recorded for later (unchanged from the brief §8)

Ad-aware split (`looksLikeRealTrack` seeds it); copy/remux (additive ABI
fn); per-cut cover art; split-trim; keep-draining-while-paused (the plugin
conversation); MP3 RG read-back (pre-existing TXXX gap, rips + recordings
equally); log semantics ("derived, never verifiable").
