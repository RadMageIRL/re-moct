# CD-S2 design note - side-products under a partial selection

**Status: GREENLIT and BUILT.** Design of record, commits with the slice.
Follows CD-S1 (`4960e9c`, gated PASS, CI green). Branch `experimental/win-pdcurses`. No UI.
Local's gate.

**Rulings received, both as recommended:**
- **F5 -> (a), `!rg.valid`.** `disc.json` is already changing (`schema_version` 1 -> 2) in this
  slice regardless, and the byte-identity constraint exists to prove *the rip* did not change:
  audio, CRCs, verification, side-products. (a) touches none of those. A failed loudness
  measurement writing `null` instead of `0.0` is the same conflation this slice exists to close.
  **The caveat stands and must stay in the debrief: the gate cannot distinguish (a) from (b),**
  because every *Relish* track measures successfully. A green gate proves less here than it looks.
- **§4 -> yes, record the selection explicitly.** In scope for one reason: the schema is already
  bumping, and adding it later would cost a version 3 for a single field.

**Whole-disc shape of the `selection` block (mine to choose): written ONLY on a partial.** On a
whole-disc rip the TOC already is the record, and omitting the block keeps `disc.json` identical
to today apart from `schema_version`, which keeps the stop condition sharp. Absence is not the
zero-means-unmeasured defect repeated: `schema_version: 2` guarantees this writer would have
emitted the block had the rip been partial, so absent is a discriminated encoding, not a value
collision.

---

## 0. Summary

CD-S1 made the worker able to rip a subset and proved the audio verifies. This slice makes the
things written *beside* the audio correct for a subset, under the refined principle:

> Omit disc-level artifacts that would be WRONG or would misrepresent what the user has.
> Keep accurate records of the disc's identity.

Eight findings from probing the live tree are in §1. Two of them change what the brief asked for
and need a ruling before I build: **F2** (the CTDB mode line cannot be expressed until CD-S3) and
**F5** (the `disc.json` fix changes full-rip output in one rare case, which the byte-identity
constraint forbids as written).

---

## 1. Findings from the probe

Every claim below was read out of the current tree, not assumed.

**F1 - the predicate the brief asks for already exists and is unwired.**
`ripsel::isWholeDisc` ([RipSelection.h:93](../include/RipSelection.h#L93)) has **zero callers** in
`src/`. CD-S1 built it for exactly this slice and never used it. It is already covered by three
assertions in `rip_selection_test` (lines 50, 128, 129). So "one predicate, not the same comparison
written five times" costs one line, not a design.

**F2 - CTDB's mode-line piece CANNOT be expressed today. It must defer to CD-S3.**
The `[C]` row is a static entry in RipConfirm's mode table
([UIManager.cpp:2254](../src/UIManager.cpp#L2254)): `{ "[C]", "CUETools    ", "Online CRC32 check,
one verdict per disc" }`. The product's only rip call
([UIManager.cpp:6605](../src/UIManager.cpp#L6605)) passes no `selected_toc`, so it defaults to
"all". **A partial selection is unreachable from the UI today.** The modal therefore has no partial
state to render, and greying `[C]` would be a placeholder for a condition that cannot occur - which
the brief explicitly forbids. **Proposing to defer only the mode-line rendering to CD-S3**, and to
build the half that is real now (F3).

**F3 - CTDB on a partial is not merely "no partial form", it is broken two independent ways.**
This strengthens the refusal and is worth recording:
1. `ctdb_total_bytes` is summed over the **full TOC**
   ([CDRipper.cpp:1681](../src/CDRipper.cpp#L1681)), but only selected tracks feed bytes. So
   `abs_pos` never reaches `ctdb_end_trim_at` ([:1076](../src/CDRipper.cpp#L1076)) and **the
   end-trim never fires at all.**
2. The CRC covers only the selected audio, in selection order, rather than the disc.

The result is a well-formed 8-hex-digit ID that is wrong for two reasons and would be reported as a
verdict. That is precisely "silently wrong", so the refusal is not cosmetic.

**F4 - omitting album gain needs an "absent" representation, or it repeats the defect.**
`RGResult` ([CDRipper.h:49](../include/CDRipper.h#L49)) carries `valid` for the *track*
measurement, but `album_gain`/`album_peak` are bare `double`s defaulting to `0.0`. **0.0 dB is a
legitimate album gain.** There is no way to say "no album value" without adding one. So the tag
omission and the `disc.json` fix are the *same* fix, one layer apart.

**F5 - the `disc.json` fix changes FULL-rip output in one rare case. This needs a ruling.**
The semantically correct predicate for "unmeasured" is `RGResult::valid`, which is false both for
an unripped track *and* for a ripped track whose loudness measurement failed
([:1358](../src/CDRipper.cpp#L1358) is the only place it is set true). Using it means a full rip
with a failed measurement writes `null` where it writes `0.0` today. That is the defect being
fixed - but it is a change to full-rip output, and the constraint says a full selection must be
byte-identical. **Options in §3.4. My recommendation is stated but this is not mine to decide.**

**F6 - the Opus/R128 path writes an album value the brief's table does not mention.**
[CDRipper.cpp:840](../src/CDRipper.cpp#L840) writes `R128_ALBUM_GAIN` from `rg.album_gain`, gated
only by `if (rg.valid)` - which wraps the track line too. On a partial Opus rip this leaks the
collapsed album value in a different tag dialect. Must be covered by the same omission, with the
gate split so the track line is unaffected.

**F7 - `.m3u8` is correct on a partial and stays. Stated so it is not left floating.**
Measured on `sel=3,7,9`: three entries, real track numbers, files that exist. It is a list of what
was ripped, not a description of the disc. No change, no log line.

**F8 - `disc.toc` and `disc.json` share one guard block.**
Both live under the single `if (!cancel_.load())` at
[CDRipper.cpp:2277](../src/CDRipper.cpp#L2277) (JSON nested at :2306). "KEEP both" means that block
is untouched apart from the ReplayGain fields.

---

## 2. Nothing else is written

Audited against the disconfirmation clause rather than the CD-S1 measurements alone. The complete
set of things a rip writes: the audio files, `<artist> - <title>.cue`, `<artist> - <title>.m3u8`,
`disc.toc`, `disc.json`, `logs/rip_<stamp>.log`, `.ar-db-info/` (the raw AR DB response plus
`accuraterip-id.txt`), and the tags inside each audio file.

`.ar-db-info/` is a verbatim cache of the AccurateRip response for the **disc** and is keyed by the
disc ID, which CD-S1 kept correct by never truncating the TOC. It is accurate on a partial and is
not derived from the selection. **Keep, no change.** No sixth artifact turned up; if one does, it
gets raised, not decided.

---

## 3. The design

### 3.1 One predicate, wired once

At the top of `worker`, immediately after `disc_total`/`sel_count`
([CDRipper.cpp:1674](../src/CDRipper.cpp#L1674)):

```cpp
const bool whole_disc = ripsel::isWholeDisc(disc_total, plan);
```

Every side-product decision below reads that one name. Nothing re-derives it by comparing counts.

### 3.2 `.cue` - skip on partial

The guard at [:2195](../src/CDRipper.cpp#L2195) becomes `if (!cancel_.load() && whole_disc)`, and
the partial case appends one line to the rip log saying the cue was skipped and why. Whole-disc
behaviour is the same statement it is today.

### 3.3 Album ReplayGain - omit the tag pair entirely

Add to `RGResult`:

```cpp
bool album_valid = false;   // false = no album figure exists, NOT "it was 0 dB"
```

Set true only where the album figure is both computed and meaningful - i.e. in the existing
assignment loop at [:2152](../src/CDRipper.cpp#L2152), gated on `whole_disc`. Track gain and track
peak are untouched on every path.

In `tagFile`, the four album writes (FLAC :764-765, :798-799; MP3 :880-881; M4A :923-924) and the
R128 album line (:840, split from the track line per F6) become conditional on `album_valid`.

**Omit the pair entirely rather than write a sentinel.** ReplayGain has no standard "absent" value;
every scanner in the ecosystem treats a *missing* album tag as "not scanned" and will compute the
real thing later, which is exactly the outcome the ruling wants. Any sentinel we invent would be
read as a measurement. This is the same reasoning as F4, applied to a format we do not control.

### 3.4 `disc.json` - unmeasured must not read as measured

`rgj` ([:2380](../src/CDRipper.cpp#L2380)) becomes:

- `album_gain` / `album_peak` -> JSON `null` when `!album_valid`
- per-track `gain` / `peak` -> JSON `null` when the track was not measured
- `schema_version` 1 -> **2**

`null` rather than an omitted key, so the array shape and `n` stay stable for readers. The
`accuraterip` block already models this correctly with `status: "not_queried"` and is untouched.

**The open decision from F5** - what "not measured" means for the per-track values:

| | predicate | full-rip output | partial-rip output |
|---|---|---|---|
| **(a) recommended** | `!rg.valid` | identical **except** a failed measurement writes `null` not `0.0` | correct |
| **(b) conservative** | not in `plan` | byte-identical, always | a *failed* measurement on a ripped track still writes `0.0` |

I recommend **(a)**: it is the honest predicate, a failed measurement is genuinely unmeasured, and
it is the same conflation this slice exists to close. But it does change full-rip output in that
one case, so it needs an explicit ruling against the byte-identity constraint. **(b)** satisfies the
constraint literally and leaves a smaller version of the defect in place.

Under **(a)** the Relish gate is still byte-identical, because every track measures successfully -
which means **the gate cannot distinguish (a) from (b)**. Recording that here rather than letting a
green gate imply more than it proves.

### 3.5 CTDB - refuse the combination now, render it in CD-S3

In `start()`, after the plan is built and before the thread launches
([CDRipper.cpp:2498](../src/CDRipper.cpp#L2498)):

```cpp
// CTDB is one CRC over the whole disc's audio; no partial form exists, and a
// subset would produce a well-formed ID that is wrong twice over (the end-trim
// never fires, and the CRC covers the wrong audio). Refuse before anything is
// written rather than report a verdict that cannot mean anything.
if (mode == RipMode::CUETools && !ripsel::isWholeDisc(disc_total, plan)) return false;
```

Bare `false` follows the refusal idiom CD-S1 already established one line below for an empty plan,
which itself follows the format picker's zero-format refusal. Nothing is written because the worker
never starts - that is "refuse before commit" in the strongest form available.

**Division of labour, stated so neither half is assumed done by the other:** CD-S2 makes the
combination *unexecutable*; CD-S3 makes it *unselectable* by greying `[C]` with `whole disc only`.
Until S3, the only caller that can reach this is `riprobe`, which prints `start() FAIL` - adequate
for a gate, not a user-facing message, and not pretending to be one.

### 3.6 The log says which and why

On a partial rip the summary block gains lines naming each omitted artifact and its reason - the
cue, the album gain, and CTDB if it was refused. The existing `ReplayGain: album gain=...` line
([:2419](../src/CDRipper.cpp#L2419)) reports the omission instead of printing a collapsed number.
On a whole-disc rip the log is unchanged, character for character.

---

## 4. Raised, not decided

**Should `disc.json` record the selection explicitly?** It is kept partly because it records what
was *not* taken, but it conveys that only implicitly, via twelve `not_queried` entries. An explicit
`selection` block (`{"partial": true, "ripped": [3,7,9], "disc_total": 12}`) would state it
directly and would be the natural thing for a future re-rip to read. It is also a schema addition
beyond the defect this slice was called for, so I am not building it unasked. **Ruling wanted.**

---

## 5. Gate

**Machine, both toolchains.** Both builds clean, both suites green (Win 51/51, Linux 52/52).

**No new unit test, and that is a determination rather than an omission.** I said I would add the
absent-vs-zero case "at whatever level is genuinely testable" and then looked for that level. There
is not one worth taking: `isWholeDisc` already carries the predicate's three assertions
(`rip_selection_test` lines 50, 128, 129); everything else added here is `RGResult` field plumbing
and JSON construction inlined in `worker`, which takes a live device. The only way to reach it
would be to extract the ReplayGain JSON builder into a header purely so a test could assert a
ternary - a new seam, and a refactor of a path this slice must keep byte-identical, in exchange for
a test of `valid ? x : null`. Not worth the risk to the thing the stop condition guards.

**Honest limit, stated up front:** almost all of this slice is file I/O inside `worker`, which
takes a live device. It is not unit-testable without a fake device layer, and building one is not
in scope. **The artifact behaviour is hardware-gated, not test-gated** - the same split as CD-S1's
side products.

**Hardware, via `riprobe`, on *Relish*, identifying the disc by TOC against the baseline first.**

1. **Whole disc byte-identical.** 12/12 AR v2, confidence 200, CRCs identical to the retained
   baselines. Cue present and identical, album gain written, `disc.toc`/`disc.json` unchanged apart
   from `schema_version`, CTDB available in `[C]` mode. **Stop condition: if this is not
   byte-identical, stop and report.**
2. **Partial rip** (`sel=3,7,9`): no `.cue`; no `REPLAYGAIN_ALBUM_*` and no `R128_ALBUM_GAIN`, with
   track gain still written and correct; `disc.toc` and `disc.json` present and accurate; unripped
   tracks `null` not `0.0`; log names each omission.
3. **Subset CRCs unregressed** - `sel=1`, `sel=5`, `sel=12`, `sel=3,7,9` still match their CD-S1
   baselines.
4. **CTDB refusal**: `riprobe F <out> C f 0 0 0 sel=5` refuses and writes nothing;
   `riprobe F <out> C f` (whole disc) still reaches a CTDB verdict.

Verification split in the debrief as in the LIB campaign: what was run and green, versus what the
gate confirmed. Brace-balance and scoped-diff audit, `fetchARData` and `AR_PREGAP` grepped rather
than assumed.

---

## 6. Constraints honoured

`fetchARData` unmodified, still receives the full TOC. `AR_PREGAP` not read, reasoned about, or
approached. Track gain per-track and correct on every path. Audio thread untouched, nothing in
streaming, `Ctrl+T` untouched. No UI, no marking, no HTOA, no CUETools work beyond the refusal, no
new artifacts. Expected footprint: `include/CDRipper.h`, `src/CDRipper.cpp`, and this note. No
CHANGELOG line - still no user-reachable behaviour change, since a partial rip is unreachable from
the product until CD-S3; it earns its line there.
