# ABI cluster — keep-draining + copy/remux — DESIGN PROPOSAL

**Stage:** DESIGN PROPOSAL for review and finalization. Not a plan, not
implementation. No source, no commit.
**Baseline:** `experimental/win-pdcurses` @ `8038368`.
**Scope:** the frozen stream-plugin C ABI opens ONCE, for exactly these two
additions, then closes again.

---

## 1. The current ABI and what "additive" actually means (§1.1)

The contract is `include/core/remoct_plugin.h` (frozen at Phase 4 slice (a)):

- One export: `remoct_plugin_query()` → a static `const RemoctPlugin*`.
- `RemoctPlugin`: `abi_version` + `struct_size` + name/display, then the fn
  table — REQUIRED: `create/destroy/handles_url/open/read_frames/close`;
  optional spine: `get_caps/position_sec/duration_sec/seek_to/set_paused/
  is_buffering`; optional metadata: `now_playing/art_url/last_error`; and
  `set_config(key, value)` — "one typed-enough knob channel."
- Injected the other way: `RemoctHostServices` (its own `struct_size`).
- **The versioning rule, verbatim in the header (:205-214):**
  - `REMOCT_ABI_VERSION` is the MAJOR gate — the host REFUSES a plugin whose
    `abi_version` differs. It exists for breaking changes only.
  - **Additive growth does NOT bump it.** A new capability is a new fn
    pointer APPENDED after `set_config`; both sides carry `struct_size`; the
    host reads `min(host_size, plugin_size)` worth of fields and null-checks
    any optional pointer before calling.
- Loader policy (`PluginHost::validatePlugin`): null/AbiMismatch/TooSmall/
  MissingRequiredFn reject paths — already production-tested (slice-a tests,
  the slice-c load-failure grace path).

**Premise correction the design must carry: there is no version "bump."**
Bumping `REMOCT_ABI_VERSION` would REJECT every existing plugin — the exact
opposite of graceful. The single "opening" the brief asks for is **one
struct growth**: the cluster's fn pointers appended together, once, with
`REMOCT_ABI_VERSION` staying 1. Detection is the contract's own mechanism:
`struct_size` reaches the new field AND the pointer is non-NULL, checked
**per function** — which gives per-feature capability granularity for free.

How each mismatch resolves (the §1.4 matrix, mechanism level):
- New host + old plugin: plugin `struct_size` stops before the new fields →
  host treats them as absent. No call, no crash.
- Old host + new plugin: host reads only its own known prefix; never sees
  the appended fields; never sends the new signals. Plugin defaults keep v1
  behavior exactly (tee OFF, drain-through-pause OFF). No call, no crash.

## 2. Keep-draining — the gapless design (§1.2)

### 2.1 Root cause, precisely (from the R1 probe + sources)

Three sites, ALL plugin-side, one host-side consequence:

1. `StreamSource::readFrames` (plugins/stream/StreamSource.cpp:1647):
   `if (paused_ || !prebuffered_) → memset silence, return` — the ring is
   not drained while paused.
2. HLS/segment producer (:1458) and the AAC/ICY producer (:1536):
   `if (paused_) { sleepMs(20); continue; }` — the NETWORK stops being
   consumed (probe: `segment_gets` frozen through a pause).
3. Host consequence: the tap (which sits after `read_frames` in the
   callback) receives the silence pads → gap in the file; the paused-over
   broadcast is never even received.

### 2.2 The mechanism — one truth in the plugin, two views in the host

**The pivotal existing fact: the audio callback KEEPS RUNNING during a
stream pause** (the host never stops the device — R1 probe). So a consumer
for the ring already exists during a pause; today the plugin just refuses to
use it. The design exploits that:

- **New appended fn: `int32_t (*set_record_active)(void* self, int32_t on)`
  → returns 1 if honored.** The host calls it on recorder start/stop. While
  active, the plugin's THREE pause sites change meaning:
  - `readFrames`: the `paused_` short-circuit is skipped — the ring drains
    normally and returns REAL frames (the `!prebuffered_` arm stays).
  - Both producers: the `paused_` freeze is skipped — the network keeps
    being consumed, decode keeps filling the ring.
  With `record_active` off (or an old plugin): all three sites behave
  byte-for-byte as today.
- **The playback/recording divergence happens IN THE HOST CALLBACK, after
  the tap.** Stream branch ordering while paused-and-recording:
  `read_frames` (real audio) → `recorder.capture()` (real audio into the
  file) → **host memsets the output buffer to silence** → volume/EQ/viz see
  zeros (the UI looks paused, exactly as today) → speaker silent.
  The plugin holds one truth (the real broadcast); the host renders two
  views (file: real; playback: silence). No plugin-side forking of paths.

An explicit ack (return 1) rather than a fire-and-forget `set_config` knob:
the host KNOWS whether the plugin honors it, which drives the honest panel
copy (§2.5) — a knob with no acknowledgement can't.

### 2.3 The gapless proof-of-continuity argument (§0's acceptance bar)

The recording path during a pause is **bit-identical to the recording path
while playing**: same producers, same ring, same `read_frames` drain, same
callback cadence, same tap. Pause changes exactly ONE thing — a post-tap
memset in the host. Therefore no new gap source exists to prove absent: the
paused recording pipeline IS the unpaused pipeline. Continuity reduces to
what is already gated (dropped-frames == 0 through the session), and the
verification (§5) demonstrates it directly: pause mid-song for 30+ s → the
cut's frame count equals elapsed broadcast time, no silence window, RMS
continuous across the pause region.

Resume is equally trivial: un-pause stops the memset. The listener rejoins
the LIVE broadcast (radio has no pause position — `position_sec` advances
through the pause; accepted and stated).

### 2.4 Buffering/backpressure — structurally bounded (§3 Q2)

There is NO unbounded buffer, by construction: the consumer (the audio
callback) never stops, so the plugin's existing ring drains at exactly the
rate it fills, paused or not. A long pause during recording is
indistinguishable, resource-wise, from a long recording. Nothing to cap,
nothing to drop-oldest. (This is the decisive advantage of draining through
the ring over any "producer-side spool while paused" design, which WOULD
grow unboundedly.)

### 2.5 Scope + degradation (§3 Q1)

Scoped to recording: the host sends `set_record_active(1)` at recorder
start, `(0)` at stop. Non-recording pause behavior is unchanged for every
consumer. Old plugin (`set_record_active` absent or returns 0): the R2
pause-gap note stays in the panel; with a capable plugin the note flips to
"pause mutes playback - the recording continues" — truth either way, driven
by the ack.

## 3. Copy/remux — zero-loss capture (§1.3)

### 3.1 What the plugin actually carries pre-decode (§3 Q4)

| Source path | Pre-decode bytes | Codec |
|---|---|---|
| ICY continuous MP3 | post-metaint byte stream = self-framing MP3 frames | MP3 |
| ICY continuous AAC | post-metaint ADTS stream | AAC-ADTS |
| iHeart HLS | fetched segment payloads (ADTS; may carry interleaved timed-ID3) | AAC-ADTS |

**One exposure fn covers both transports** — the tee is normalized
post-transport (ICY: after metaint stripping; HLS: segment payloads
concatenated) into a single codec-tagged byte stream, with a
**discontinuity flag** raised at HLS segment boundaries after a re-pin /
reconnect (the muxer resyncs on the next frame header). Segmented vs
continuous does not need different ABI surface — it needs one bit.

### 3.2 The appended contract (declared with §2's fn — the boundary opens once)

    /* appended after set_config, in this order (v1 additive growth): */
    int32_t  (*set_record_active)(void* self, int32_t on);      /* §2 */
    /* copy/remux tee - OFF by default (zero cost when unused): */
    int32_t  (*encoded_caps)(void* self);                        /* REMOCT_CODEC_* now flowing, 0 none */
    void     (*set_encoded_capture)(void* self, int32_t on);     /* arm/disarm the tee ring */
    uint32_t (*read_encoded)(void* self, uint8_t* dst, uint32_t cap,
                             int32_t* codec_out, int32_t* discont_out);
    /* + enum REMOCT_CODEC_NONE=0, MP3=1, AAC_ADTS=2 */

- `read_encoded` is a host/WORKER-thread pull (never the audio thread) from
  a plugin-side SPSC tee ring the producers feed when armed — the
  `read_frames` idiom applied to bytes. Host buffers, host cadence (~50 ms
  polls); nobody frees across the line; bounded tee ring with a
  drop-oldest+counter policy surfaced like the recorder's (overflow honest,
  never blocking the producer).
- Slicing note: all four pointers are DECLARED and the struct grows once in
  slice A; a plugin build may ship `read_encoded = NULL` until slice B —
  per-fn null-checking makes that a clean intermediate state, not a hack.

### 3.3 Host-side muxing per codec

- **MP3 → `.mp3`: raw frame append.** Tagging = the EXISTING tagCut MP3
  path unchanged (ID3v2 + APIC + RG TXXX are container-level, encoder-
  independent) — copied MP3 cuts arrive fully tagged and RE-MOCT-playable.
  Near-free.
- **AAC-ADTS → `.aac`: raw frame append** (self-framing, plays in
  foobar/VLC). Honest limits, stated up front: (a) TagLib does not tag raw
  ADTS — v1 copy cuts of AAC are UNTAGGED (filename carries the identity);
  (b) RE-MOCT's own player does not currently open bare `.aac` — flagged as
  a small later decode-side addition, NOT part of this cluster; (c) a real
  `.m4a` mux needs box-writing (a new dep or hand-rolled) — explicitly OUT,
  recorded as later polish. Strip interleaved timed-ID3 from HLS payloads
  at the tee (it is transport metadata, not audio).
- No Ogg-framed source exists in the plugin today → out of the matrix.
- Both muxers ride a tiny frame-parser (MP3 header → frame length; ADTS
  ditto) the writer already needs for split alignment (§3.4).

### 3.4 Split, trim, tag, RG on copy

- **Splits land on frame boundaries** — the writer emits whole frames only
  (~26 ms MP3 / ~23 ms AAC granularity, noise under the 1-2 s slop).
- **The trim hold composes**: frame headers give exact per-frame duration
  (1152/sr MP3, 1024/sr AAC) — the hold counts encoded-frame durations
  instead of PCM frames. Same knob, same semantics.
- **Ad-aware composes**: classification is title-string-based, upstream of
  the writer — routing/suppression identical in copy mode.
- **No per-cut ReplayGain on copy** (it would require decoding — the exact
  thing copy avoids). Stated; the re-encode mode remains the RG-bearing
  option.

### 3.5 Panel model (§3 Q3)

Copy and re-encode are **mutually exclusive per recording** (different tap
points: PCM ring vs encoded tee). The format radio gains a third entry:

    ( ) 3  Copy    as broadcast (MP3 -> .mp3 / AAC -> .aac, no re-encode)

Greyed with a dim note when the plugin lacks `read_encoded` (old plugin) or
`encoded_caps` reports none. Quality column shows the live codec when
available. Config: `rec_format = copy` joins opus|mp3.

## 4. Compat matrix (§1.4) — no crash in any combination

| Host \ Plugin | v1 (old) | cluster-A (drain only) | cluster-B (full) |
|---|---|---|---|
| old (v1) | today | today (new fields never read; defaults OFF → v1 behavior) | same |
| new | today + pause-gap note; Copy hidden | gapless pause; Copy hidden (read_encoded NULL) | gapless + Copy offered |

Mechanism per cell: `min(struct_size)` + per-fn null-check (the existing
loader/consumer discipline) + `set_record_active`'s ack. `abi_version`
stays 1 everywhere; nothing ever rejects, nothing ever calls a missing fn.
The in-tree compiled-in reference (`remoct_stream_plugin_query`) and the
parity/loader tests extend to pin exactly this matrix (a v1-shaped
descriptor fixture keeps the old-plugin column honest forever).

## 5. Slicing recommendation (§1.5) — one opening, two slices

- **Slice A — the contract + keep-draining (first, the high-value bar):**
  header grows once (all four fns + codec enum declared), plugin implements
  `set_record_active` + the three-site behavior, host wires
  start/stop + the post-tap mute + the panel-note flip, compat fixtures.
  **Acceptance = the gapless gate:** record, pause 30+ s mid-song, resume —
  cut frame count == elapsed broadcast time, zero silence window, RMS
  continuous; plus ctest both toolchains and the R1-class live gates.
  Being a plugin change, the slice-c/d discipline applies: the loaded-
  module parity and live gates re-run.
- **Slice B — copy/remux on the open surface:** tee ring + `read_encoded`
  plugin-side; host mux writers (MP3 append, ADTS append + ID3 strip),
  frame-aligned split + duration-based trim, panel third mode, overflow
  honesty. Acceptance: byte-level — a copied MP3 cut's frames are a
  byte-subset of the broadcast stream (tee-vs-file compare in a fixture),
  both live paths (ICY + HLS) produce playable cuts, splits land on frame
  boundaries.

Sequenced this way, keep-draining never waits on the (larger) muxing work,
and the boundary still opens exactly once — in slice A, structurally.

## 6. Open questions resolved vs flagged (§3)

- **Q1 scoped-vs-always:** scoped via `set_record_active` (with ack) — §2.5.
- **Q2 long-pause buffering:** structurally bounded, nothing grows — §2.4.
- **Q3 copy-vs-re-encode exclusivity:** confirmed; third radio — §3.5.
- **Q4 ICY vs HLS treatment:** one fn + a discontinuity bit — §3.1.
- **FLAGGED for review (decisions we need):**
  1. AAC copy cuts untagged in v1 (raw ADTS has no TagLib home) — accept,
     or gate copy to MP3-only streams until an .m4a mux exists?
  2. Bare-`.aac` playback in RE-MOCT is a separate later slice — accept
     that copied AAC verifies in external players only, for now?
  3. The tee ring's size/overflow counter default (propose: 2 MiB,
     drop-oldest, surfaced like dropped-frames) — number to bless.
