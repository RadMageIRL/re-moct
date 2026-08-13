# DESIGN — bit-perfect playback, lossless local files: proposal

**Design note. Not greenlit. Untracked until it is.**

**Locked list.** Not the CD path, so that declaration does not apply. **The audio thread is locked.**
This proposal does not reach it — every change proposed below is in device setup or the UI, and the
one place a fuller version *would* reach it is named and stopped at. `abi_version`, the stream
plugin and `libfdk-aac-2.dll` are untouched and unmentioned beyond the scope exclusion.

Follows `docs/RECON-bit-perfect.md`. Branch `experimental/win-pdcurses`, tip `06be569`. Everything
marked **measured** was run on 2026-08-12 on Dos's own machine.

---

## THE HEADLINE — the feature cannot deliver its claim on this hardware, and that is the finding

The brief asked whether the current shared-mode path is already sample-accurate, and said that if it
were, the finding would be worth more than the feature. **It is not sample-accurate — and the reason
is not RE-MOCT's code, and exclusive mode does not fix it.**

**Measured, every playback endpoint on this machine** (`scratchpad/dev_probe.cpp`, WASAPI, 11
endpoints — GoXLR System/Music/Game/Chat/Sample, Realtek Digital Output, two monitors, Jabra headset,
two Steam Streaming):

> **Every single one reports native 48000 Hz. Not one supports 44100.**

So a 44.1 kHz CD rip — which is what Dos's library is — **cannot reach any of his DACs at its own
rate**. Something resamples it, always, no matter what this feature does.

### The three cases, measured rather than argued

| what we ask for | result | who resamples |
|---|---|---|
| **shared f32/2/44100** (what RE-MOCT does today) | init OK, miniaudio reports internal **44100** | **Windows.** The client stream is 44.1; the audio engine runs at the endpoint's 48000 mix format and SRCs below miniaudio's visibility (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`) |
| **exclusive f32/2/44100** | **init OK** — internal **s16 2ch 48000** | **miniaudio.** Its own converter, because the hardware refused 44.1 |
| **exclusive s16/2/48000** | init OK — internal **s16 2ch 48000** | **nobody. Exact.** The only combination on this machine that converts nothing |

Even `exclusive f32/48000` converts — the endpoint's true native is **s16/48000**, and asking for
float gets a conversion to integer.

### The trap this exposes, and it is the most important line in this document

**Exclusive mode returned `MA_SUCCESS` while silently resampling 44.1 → 48 kHz.**

A straightforward implementation sets `shareMode = ma_share_mode_exclusive`, sees success, and lights
up a "bit-perfect" indicator — **while the samples are being converted twice over** (rate *and*
format). That is not a hypothetical: it is what this machine does, today, to the exact request RE-MOCT
already makes.

`ma_device_init` succeeding means *"a device was opened"*, **not** *"you got what you asked for"*. The
only thing that means bit-perfect is comparing the device's **internal** format against the source
after init:

```
d.playback.internalFormat     == source format
d.playback.internalChannels   == source channels
d.playback.internalSampleRate == source rate
```

**Any bit-perfect feature that does not do that check is a feature that lies.** By this project's own
standard — do not silently do something other than what was asked — the check is not an
implementation detail, it is the feature.

---

## What RE-MOCT itself does to the samples: at defaults, nothing

Worth stating because it is the half that is already correct, and it is not obvious from the code's
size.

`maDataCallback`, file branch, in order — and every stage is **conditional**:

| stage | default | touches samples at default? |
|---|---|---|
| ReplayGain (`:935-944`) | `replaygain_enabled_ { false }` | **no** |
| Balance (`:947-955`) | `balance_ { 0.0f }`, skipped when 0 | **no** |
| EQ (`:958-961`) | `eq_enabled = false` | **no** |
| Visualiser feed (`:964-970`) | always | **no — reads only** |
| Mute (`:973-974`) | `muted_ { false }` | **no** |
| miniaudio master volume (`:20392`) | `volume_ { 1.0f }`, guarded `if (masterVolumeFactor != 1)` | **no** |

**At stock settings the float samples reach WASAPI exactly as the decoder produced them.** The FLAC
s16 → f32 conversion is a divide by 32768, which is exact and reversible.

**One thing worth flagging, not as a bug but as a fact about the claim:** ReplayGain applies a fixed
**+6 dB preamp** (`:938`, `gain *= pow(10, 6/20)`), so with RG *enabled* the gain is ~1.995 even for a
track with no RG tag — `gain != 1.0f` is always true and every sample is scaled. That is a
deliberate loudness choice and out of scope to change; but **"bit-perfect" and "ReplayGain on" are
mutually exclusive by construction**, and the indicator has to know that.

---

## What to build

### 1. Verification, not assumption — the core

A `bit_perfect` config flag, off by default, applying **only to lossless local files** (FLAC, WavPack,
WAV — the scope from the roadmap). When on, device open becomes:

1. Ask the decoder for the file's **native** format (rate, channels, bit depth) *before* forcing the
   output config. Today `open_decoder` forces 44100/2/f32 unconditionally; the native format is
   available from the backend and is already read (`RECON` B-R1).
2. Attempt exclusive at the file's native format.
3. **Compare `internal*` against what was asked.** Exact match → bit-perfect, claim it. Anything
   else → **uninit and fall back**, do not keep a device that converts while claiming it does not.
4. Fall back to today's shared 44100/2/f32 path, unchanged, and say so.

**The fallback is the normal case on this hardware, not the error case.** Step 3 will reject every
44.1 file on every device Dos owns. That is correct behaviour and the indicator must not present it
as a failure.

### 2. Mixed rates at a track boundary

The roadmap scope removes most of this: a folder of CD rips is all 44.1, so gapless never meets a
rate change. What remains is a lossless library mixing 44.1 with 48 or 96.

**Proposal: resample across the transition and say so, rather than reopen and gap.** Reasons:

- Gapless exists to remove a gap. A mode that reintroduces one at album boundaries breaks the more
  valuable, older guarantee to serve the newer, narrower one.
- The transition is exactly where a reopen is most audible.
- The next track's *own* playback can be bit-perfect once it starts; only the seam is resampled.

**Crossfade across a rate change stays structurally impossible** — one device, one rate, two decoders
— and the honest behaviour is to say so rather than silently degrade one side. **Proposal: when
bit-perfect is on and the next track's rate differs, crossfade does not engage for that transition**
and the indicator says why. Not "crossfade is disabled"; "these two tracks cannot be crossfaded at
their own rates."

### 3. THE INDICATOR — and on this hardware it is the whole deliverable

Because bit-perfect will be unachievable for Dos's library, the part that actually changes what he
knows is the part that tells him what is happening. Today **nothing does**: the status line prints
`track.sample_rate` from TagLib — the **file's** rate — and nothing anywhere names the output.

Proposal, in the status-line meta block that already carries rate and channels:

| state | shown | meaning |
|---|---|---|
| bit-perfect off (default) | `44.1 kHz` *(unchanged)* | the file's rate, as today — no regression, no new noise |
| bit-perfect on, achieved | `44.1 kHz **=**` | source and device agree, nothing in between |
| bit-perfect on, not achieved | `44.1 kHz **→ 48**` | asked for 44.1, the device is running 48 |
| bit-perfect on, DSP active | `44.1 kHz **→ dsp**` | ReplayGain / EQ / balance is scaling samples, so the claim cannot hold regardless of the device |

Three characters, in a field that already exists, in the same place Dos already reads the rate. **No
new panel, no redesign.** The `→ 48` case is the one that earns the feature on this machine: it makes
a resample that has always happened, and has never been visible, visible.

**Precedent for the wording** is the convert modal's *"Output is 44.1 kHz (sources are resampled)"*
(`UIManager.cpp:13210`) — the project already accepts that this needs saying; it just says it in one
modal, for one operation, in one direction.

---

## The two scars: cleared, explicitly

The brief asked for this to be stated rather than implied.

- **`LocalFileSource.cpp:161`** forces 44100 because opening at a file's native rate caused
  chirp-then-silence, naming *a 24000 Hz mono audiobook*. **An audiobook is lossy AAC and is out of
  scope.** Under this proposal the forced 44100/2 path stays exactly as it is for every lossy file,
  which is every file that scar was about.
- **The warm-up device in `initDevice`** exists because the first WASAPI bring-up coinciding with the
  **FDK-AAC** decoder produced *"a chirp then nothing"* for the session. **Also the lossy path**, also
  out of scope, and the warm-up itself is untouched by this proposal — it stays first, unconditional.

**Neither scar is on the FLAC/WavPack/WAV path.** The scope excludes them by construction rather than
by workaround, which is what the brief asked the feature to demonstrate.

**One honest caveat:** exclusive mode does more device bring-up than shared, and both scars are
bring-up fragilities. They are on a different decoder, so the specific failures should not recur —
but "more device init on Windows audio" is the general shape of both, and the first live test should
watch for it.

---

## Cost, and my recommendation

**Cost is low and concentrated in device setup**: a native-format query before open, an exclusive
attempt with an exactness check, a fallback that is today's code path unchanged, a config flag, and
three characters in the status line. **The audio thread is not touched.** If a later refinement wants
per-transition rate switching inside a continuous device session, that *would* reach the audio thread
and **stops there for a ruling** — it is not part of this.

**My recommendation: build the indicator first, and the exclusive path second — or not at all.**

The measurement says bit-perfect cannot succeed on any device Dos currently owns, so the exclusive
path would ship as a switch that always falls back. The indicator, by contrast, tells him something
true that he cannot currently see: **his 44.1 rips are being resampled to 48 kHz by Windows, on every
output, and always have been.** That is worth knowing whether or not the feature is ever built, and
it is worth knowing before deciding whether a 44.1-capable DAC is something he wants.

**If the answer is "build both", the order still matters**: the verification check is what makes the
indicator honest, so it is shared work, not sequential work.

---

## Noticed in passing — one line each

- Dos's **default device is the GoXLR**, a USB broadcast mixer that exists to mix multiple
  application streams at a fixed 48 kHz; bit-perfect through it is a category error regardless of
  what RE-MOCT does.
- **`exclusive f32/48000` converts to s16** — asking for float on this hardware costs a format
  conversion that asking for s16 does not.
- The probe reports `flags=0x00000002` on the second native format of every endpoint, which is
  miniaudio's "exclusive mode only" marker — i.e. **the s16 format is the exclusive-only one**, which
  matches the exactness result exactly.

---

## BUILT 2026-08-12 — indicator and verification, together

Approved and shipped as proposed, with two things worth recording.

**The mismatch indicator is NOT gated behind the flag**, per Dos's ruling: if the output rate differs
from the file's, the status line says so whether or not anyone opted in. Requiring an opt-in to learn
that Windows resamples everything would bury the one finding that applies to the hardware he owns.
The `=` and `-> dsp` states stay gated, because a claim is only meaningful about a mode you chose.

**Interpretation called out, because the ruling was ambiguous and it changes what he sees.** The
ruling said lossy files are "unchanged in every respect", and also that a rate mismatch is reported
always. Those collide for a 44.1 MP3. I read "unchanged" as *playback and device behaviour* - no
exclusive attempt, no format change, nothing touching the FDK-AAC path - and let the indicator apply
to every local file, because the mismatch is a property of the DEVICE, not of the file's lossiness.
**Dos confirmed that reading on 2026-08-12 and it stands.**

**Scope actually delivered.** The exclusive attempt is made at the decoder's output format, which for
a 44.1 lossless file already equals its native format - so Dos's whole library is covered without
touching `open_decoder`'s forced 44100/2, and therefore without going near either scar. **A 96 kHz or
48 kHz lossless file is still decoded to 44.1 before the attempt**, so bit-perfect cannot be achieved
for it yet; native-rate decoding for non-44.1 lossless is the follow-up, and it is the part that
would reach the forced-format path. Nothing claims otherwise: such a file simply fails the exactness
check and falls back, like everything else on this hardware.

**Not testable here, and recorded on the roadmap as such:** the achieved (`=`) state needs a DAC that
accepts 44.1, and no endpoint on this machine does. What was gated is the indicator, the fallback,
and that nothing false is claimed.

**The ASCII arrow is deliberate** - `->`, not `→`. That row draws with the narrow curses calls, which
do not decode UTF-8; an arrow glyph there is the 1.6.1 fold trap in a new costume.

### CONFIRMED ON HARDWARE 2026-08-12 — all four states, and one finding nobody predicted

Dos ran it. Every state behaves as designed:

| state | seen on | verdict |
|---|---|---|
| `44.1 kHz -> 48` | a FLAC rip | correct — the resample that was always happening, now visible |
| `-> dsp` | EQ on, **and independently** ReplayGain on | correct — either one trips it, as designed |
| *(nothing)* | **a 48 kHz Opus file** | correct, and see below |
| `=` | — | **unreachable on this hardware**, as predicted |

**THE FINDING: a 48 kHz Opus file shows no indicator, because its rate already matches the endpoint.
It is reaching the DAC untouched. The FLAC rips never have.**

Nobody predicted that and it inverts the intuition the whole feature was built on. The lossy format
is the one getting an unmodified path to the hardware today, and the lossless one is the one being
silently converted — **because "lossless" describes the file's relationship to its master, and
"untouched" describes the file's relationship to the DAC, and those are different properties that
happen to share a vocabulary.** A 48 kHz Opus on a 48 kHz endpoint is bit-perfect *as a signal path*
while being lossy *as a recording*; a 44.1 FLAC on the same endpoint is the reverse.

**This is also the strongest argument for the indicator having shipped ungated.** Gated behind
`bit_perfect=1` on lossless files only, this fact would have been structurally unobservable — the
one file type that demonstrates it is the one type the flag excludes.

It has a practical edge too: if Dos ever wants an untouched path for his CD rips, the options are a
44.1-capable DAC or resampling the library to 48 once, deliberately, rather than letting Windows do
it silently on every play. **Not a recommendation — just the shape of the choice, now that it is
visible.**
