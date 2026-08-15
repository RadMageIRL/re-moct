# RECON — bit-perfect playback, and the m4b symptom

**Design note. Not greenlit. Untracked until it is.**

**Locked list.** Not the CD path, so the `AR_PREGAP` / `ar_crc.*` / read-addressing declaration does
not apply. **The audio thread is locked and was read only** — no design follows, and the one place
below where a feature would have to touch it is named as a stop, not a plan. Nothing was changed.

Branch `experimental/win-pdcurses`, tip `06be569`. Everything marked **measured** was run on 2026-08-12
against Dos's own files through RE-MOCT's production decoder configuration.

---

## B-R2 FIRST — the m4b case, measured

### The headline: this is not a rate problem, and bit-perfect would not fix it

The decode path is **correct on every axis I can measure without knowing the symptom.** Probes built
out of tree against the real `AacDecoder` + `CustomBackends` (`scratchpad/m4b_probe.cpp`,
`m4b_seek.cpp`), replicating `LocalFileSource::open_decoder` verbatim — `ma_format_f32`, 2 channels,
44100, same custom-backend array.

| file | native | production output | duration native → out |
|---|---|---|---|
| `AlicesAdventuresInWonderland.m4b` | **24000 Hz, 1 ch**, AAC-LC | 44100, 2 ch, f32 | 9759.445 s → **9759.445 s** |
| `AdventuresOfSherlockHolmes.m4b` | **24000 Hz, 1 ch**, AAC-LC | 44100, 2 ch, f32 | 37085.312 s → **37085.312 s** |
| `AdventuresOfSherlockHolmes_librivox.m4b` | 32000 Hz, 1 ch, AAC-LC | 44100, 2 ch, f32 | 40486.816 s → **40486.816 s** |

- **Duration is preserved exactly.** 234,226,688 frames at 24000 → 430,391,539 at 44100 is the same
  9759.445 s to three decimals. **A pitch or speed error would show here and does not.**
- **The mono upmix is correct:** L and R differ on **0 of 163,840 frames** on all three.
- **No dropouts or short reads** in the first 10 s: 108 reads, 0 short, ~98.5 % non-zero samples,
  peak below full scale (no clipping).
- **Deep seeking is exact.** Resume positions from `remoct.conf`, plus arbitrary ones, on the
  10-hour file: seek to 7458.71 s, 20000 s, **36000 s** — every one lands with **+0.000 s drift**,
  returns audio, and repeats identically. Backward seek and re-seek to the same point are byte-stable.

**So: the 24 kHz→44.1 kHz conversion, the mono→stereo upmix, and the seek arithmetic are all clean.**
Whatever "not playing right" is, it is not resampling, and a bit-perfect mode would change none of
the above.

### The code already knows about this exact case, and it points the other way

`src/LocalFileSource.cpp:161-170`, verbatim:

> *"Force a fixed 44100/2 output… ma_decoder upmixes/resamples the source to it, so the playback
> device is ALWAYS 44100/2 and a file's odd native format (**e.g. a 24000 Hz mono audiobook**) never
> becomes the device format. **Opening the device at a file's quirky native rate/channels was the
> cause of the chirp-then-silence on a cold first play of such a file.**"*

**A 24 kHz mono audiobook opening the device at its native rate is a bug this project already had,
diagnosed, and fixed by forcing 44100.** Bit-perfect playback is, mechanically, that bug's cause
turned into a feature. That does not make it wrong — but it means the burden is on the feature to
show it will not reintroduce a failure the tree carries a fix for.

There is a second, related scar in `AudioManager::initDevice` (`:258-292`): a throwaway warm-up
device exists solely because *"the very first `ma_device_init` … when that first bring-up coincides
with the custom FDK-AAC ma_decoder … the device ends up producing silence for the rest of the
session — a chirp then nothing."* **The m4b files are that FDK-AAC path**, and the fragility is in
device bring-up, which exclusive mode does more of.

### What "not playing right" is: unresolved, and it needs Dos

The code does not make it obvious, because the decode is provably fine. **This has to be asked
rather than guessed**, and the answer decides whether it is a bug to fix now or nothing at all:

| what he hears | what it would point at | is it this feature? |
|---|---|---|
| **Too quiet** | measured: Sherlock's opening runs **rms 0.00117, peak 0.078** — about 30 dB below the Alice file. Could simply be the source, or ReplayGain | **No.** Separate, and possibly not a bug |
| **Muffled / dull** | expected and correct — a 24 kHz source has no content above 12 kHz. Upsampling cannot invent it | **No.** Bit-perfect would sound identical |
| **Stutter / dropouts** | buffer or callback timing, not rate | **No** |
| **Chirp then silence** | the known WASAPI/FDK first-init fragility above; would mean the warm-up workaround is not holding | **No** — and it would be urgent |
| **Pitch or speed wrong** | would contradict the measurements above | Would need re-measuring on his machine |
| **Position/time display wrong** | 10-hour file; the status line shows source rate, not output | **No** |

**Only the last two would be new information.** The first four are all *not* this feature, and three
of them are not resampling at all.

---

## B-R1 — the current path

**Nothing negotiates. Every rate decision is made at the decoder, and the device is told what to be.**

`LocalFileSource::open_decoder` (`:170`):

```cpp
ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
```

`ma_decoder` therefore inserts its own `ma_data_converter` chain and does **all** resampling and
channel conversion. The custom AAC backend reports its **native** format upward
(`AacDecoder.cpp:247-248`, `get_data_format` at `:300`) — 24000/1 — and never converts; the
conversion is entirely miniaudio's.

The device is then opened at whatever the decoder was forced to be
(`AudioManager::initDevice`, `:294-303`):

```cpp
cfg.playback.format   = ma_format_f32;
cfg.playback.channels = file_src_->channels();     // always 2 (forced in open_decoder)
cfg.sampleRate        = file_src_->sampleRate();   // always 44100 (forced)
```

`LocalFileSource::sampleRate()` returns `decoder_.outputSampleRate` — the **post-conversion** rate,
so this is 44100 by construction, not by coincidence. The CD path (`:1265-1273`) and the stream path
(`:1390-1398`) hard-code the same 44100/2/f32.

**Nothing is negotiable:** `ma_format_unknown` / `0` (miniaudio's "use the device's native format")
appears nowhere. `shareMode` is never set, so every device is **shared mode**, the miniaudio default.

**Opened once per track, not once per session.** `play()` calls `teardown()` — which does
`ma_device_stop` + `ma_device_uninit` (`:112-116`) — then `initDevice()` re-inits. **But the format
is identical every time, so today's reopen never changes rate.** That distinction is the whole of
B-R4.

---

## B-R3 — what bit-perfect would require

**miniaudio exposes exactly one knob, and its failure mode is total.**

`ma_device_config::playback.shareMode` ∈ `{ma_share_mode_shared, ma_share_mode_exclusive}`
(`miniaudio.h:6930-6932`, `:7215`). The documented contract (`:8760-8763`):

> *"if you specify exclusive mode, but it's not supported by the backend, **initialization will
> fail**. You can then fall back to shared mode if desired by changing this to
> `ma_share_mode_shared` and reinitializing."*

- **Windows/WASAPI:** exclusive mode. `wasapi.noAutoConvertSRC` (`:7143`) additionally disables
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`, which is what stops WASAPI silently resampling behind us —
  **needed for the claim "bit-perfect" to be true at all**, and separate from `shareMode`.
- **Linux/ALSA:** miniaudio maps share mode onto device-name selection —
  `ma_context_open_pcm__alsa(…, shareMode, …)` (`:28687`) picks `hw:` style names for exclusive and
  `plughw`/`default` for shared. `hw:` is the no-conversion device.

**Failure modes, all of which surface identically as `ma_device_init` returning non-success:**

| cause | what happens |
|---|---|
| another app holds the device exclusively | init fails |
| the DAC does not support the rate (a USB DAC with no 24 kHz mode is normal) | init fails |
| the driver misreports support | init **succeeds** and the output is wrong — the one case no return code catches |
| exclusive unsupported by the backend | init fails |

**Cost of "try native, fall back, tell the user":** one extra `ma_device_init` attempt per open on the
failure path — the same call already made once per track today. The mechanism is cheap. **The
expensive part is not the retry, it is B-R4.**

---

## B-R4 — switching cost, and this decides the shape

**Gapless and crossfade work by swapping DECODERS inside one continuous device session. A rate change
forces a device reopen exactly where they currently guarantee there is none.**

Today's two paths are different and only one of them reopens:

| transition | device | why |
|---|---|---|
| user changes track | **reopened** (`teardown` → `initDevice`) | but always to the same 44100/2/f32, so it is a stop/start, not a format change |
| **gapless / crossfade** | **not touched** | the audio thread swaps `next_decoder_` and the main thread installs the swap afterwards (`AudioManager.cpp:130-180`, `teardownNext`) |

So:

- **Gapless across a rate change becomes impossible without a reopen**, and a reopen is a stop/start
  of the output — the gap gapless exists to remove. An album at one rate is unaffected; a playlist
  mixing 44.1 and 48 kHz would gap at every boundary that currently does not.
- **Crossfade across a rate change is structurally impossible, not merely costly.** A crossfade mixes
  two decoders into one device. One device has one rate. There is no arrangement of exclusive-mode
  reopening that lets both tracks be bit-perfect while overlapping.
- `teardownNext` already has to `ma_device_stop()` when a crossfade is in flight (`:172-178`) to
  quiesce the callback before freeing — so the machinery for stopping mid-crossfade exists, and its
  comment is explicit that it is scoped to that window precisely because a stop is otherwise avoided.

**This is the finding that shapes the feature.** The brief's own line — *"a bit-perfect mode that
breaks gapless is a worse trade than resampling"* — is the right test, and on this evidence a
whole-session bit-perfect mode fails it. What survives the test is narrower: bit-perfect **while the
rate does not change**, resampling **across a change**, or a per-source opt-in where an audiobook
(one file, hours long, no gapless neighbour) is exactly the safe case and a mixed playlist is exactly
the unsafe one. **That is an observation about the constraint, not a proposal.**

---

## B-R5 — what the user would see

**Nothing on the playback screen says anything about output format, and the one rate it does show is
the file's, not the output's.**

- The status line's meta block (`UIManager.cpp:6103-6110`) prints `track.sample_rate` and
  `track.channels`. Those come from **TagLib** (`:5815`, `ap->sampleRate()`) — the **source** file's
  properties. **A 24 kHz mono audiobook displays "24.0 kHz  mono" while the device runs 44100
  stereo, and nothing on screen mentions the conversion.** That is defensible today (the field
  describes the file) and becomes actively misleading the moment "bit-perfect" is a claim the player
  makes, because the same string would mean "this is what you are hearing" in one mode and "this is
  what is on disk" in the other.
- **Precedent exists and is convert-scoped.** `drawConvert` (`:13210`) states *"Output is 44.1 kHz
  (sources are resampled)"* unconditionally, and adds *"Source is %d kHz; output will be 44.1 kHz"*
  in an error colour — but **only when `sr > 44100`** (`:13221`). It warns about losing 48/96 kHz
  material and says nothing when a 24 kHz source is upsampled. So the project already accepts that
  this needs saying; it just says it in one modal, in one direction, for a different operation.
- The CD path hard-codes the string `"1411 kbps  44.1 kHz  stereo  CD"` (`:6091`), which is true for
  CD by definition.

**There is a place to put an honest indicator and no indicator in it.** Whether the status line
should show source, output, or both is a design question this recon does not answer.

---

## Noticed in passing — one line each, no action implied

- The warm-up device in `initDevice` is a workaround for a **WASAPI first-bring-up fragility specific
  to the FDK-AAC decoder** producing "chirp then nothing" for a whole session; exclusive mode would
  exercise device bring-up harder, and this is the path the m4b files take.
- `AdventuresOfSherlockHolmes.m4b` opens at **rms 0.00117 / peak 0.078**, roughly 30 dB below
  `AlicesAdventuresInWonderland.m4b` — if the complaint is "too quiet", that is measurable and has
  nothing to do with rate.
- The convert modal's source-rate warning fires only for `sr > 44100`, so a 24 kHz source converts
  silently upward with no note.
- `track.sample_rate` on the status line is the file's rate on every path except CD, where the string
  is hard-coded.
