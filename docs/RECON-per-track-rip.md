# RECON CD-R1 - per-track rip selection

**Recon report and proposed plan. Nothing implemented, no product file modified.**

Branch `experimental/win-pdcurses`, tip `8ff0a85`. Everything below was read on the live tree.

---

## 0. Headline

**One premise in the brief is refuted, and it is the load-bearing one.** Passing a subset of tracks
to the existing rip entry point does **not** give partial verification - it gives **zero**
verification, silently. The feature is still very much buildable, and the brief's *conclusion*
(partial rip, partial verification) is reachable - but not by the route the interface invites.

Two other findings change shape:

- **CD tracks are PLAYLIST rows, not browser rows.** The mark mechanism cannot reach them, for three
  independent reasons. Selection is new work, not wiring.
- **The rip modal already contains a working selection mechanism** (`rip_sel_`, the format picker).
  This feature is a second selection in a modal that already has one.

---

## 1. Where do CD tracks live in the UI?

**The playlist pane. The browser never lists them.**

`activateDrive()` ([UIManager.cpp:9615-9660](src/UIManager.cpp#L9615)) on a CD drive:

```cpp
playlist_.clear();
for (const auto& t : audio_.cdTracks()) {
    std::string title = "CD Track " + (t.number < 10 ? "0" : "") + std::to_string(t.number);
    playlist_.addCDTrack(letter + ":" + title, title, t.duration_sec);   // Windows
}
pl_cursor_ = 0; pl_scroll_ = 0;
right_pane_ = RightPane::Playlist;
```

- **Row identity** is a **synthetic path**: `"G:CD Track 03"` on Windows, `"sr0:CD Track 03"` on
  Linux. Not a file. `parseCDPath` / `isCDTrackPath` read it back.
- **What draws them**: the ordinary playlist pane. There is no CD-specific draw.
- **Section flag**: none. `in_*` section flags are browser state and are all false here.
  `cd_drive_letter_` is what drives the `[CD]` header mode tag - **the brief is right that this is a
  different surface from the `[Drives]` browser pin, and they should not be conflated.**
- The Linux branch additionally `removeIf`s any stale `"<spec>:CD Track "` rows before clearing.

**Consequence for this feature: the selection surface is the playlist pane or a modal, not the
browser.**

---

## 2. Is the mark mechanism reachable there? **NO - three independent reasons**

`u` ([UIManager.cpp:8211-8221](src/UIManager.cpp#L8211)):

```cpp
case 'u':
    if (focus_ == Pane::DirBrowser) {                    // (1)
        std::string p = browserEntryPath(dir_cursor_);   // (2)
        if (!p.empty() && convertSupportedInput(p)) {    // (3)
            bool now = marked_.toggle(p);
```

1. **`focus_ == Pane::DirBrowser`** - CD tracks are in the Playlist pane, so `u` is inert on them.
2. **`browserEntryPath(dir_cursor_)`** resolves a *browser* row. A CD track has no browser row.
3. **`convertSupportedInput(p)`** gates what may be marked - and the convert paths **explicitly
   exclude CD**: `if (isCDTrackPath(s) || isStreamPath(s)) continue;`
   ([:6786](src/UIManager.cpp#L6786)).

`MarkSet` itself is a path-keyed set and *could* hold a synthetic CD path, but nothing routes one
into it and the eligibility test rejects it by design.

**So this is not "mostly wiring".** The brief asked to be told if that changed the shape: it does.
There is no existing selection over CD tracks to extend.

**And reusing `marked_` would be wrong even if it were reachable**, because `marked_` is consumed by
the converter (`x` scope 3 clears it after a run) and is a set of real files. Overloading it with
synthetic CD rows would put two meanings in one set - the defect class this codebase has closed five
times.

---

## 3. How does the rip entry point take its track set?

```cpp
bool start(AudioManager&, const std::vector<CDTrack>& tracks, const std::string& out_dir,
           const MBRelease&, RipMode, RipOptions, ProgressCb);
```

**A subset is syntactically expressible today.** The call site passes everything:
`auto tracks = cd.tracks();` ([:6573](src/UIManager.cpp#L6573)).

**But expressible is not the same as supported.** See §4 - that vector is doing double duty as *"the
disc"* and *"what to rip"*, and the AR path depends on the first meaning. **The interface does have
to change**, just not in the way it looks: it needs to carry the full TOC **and** a selection, not a
shorter vector.

---

## 4. AccurateRip on a partial rip - **THE PM'S READING IS REFUTED**

The brief's reading: *"the disc ID is computed from the whole TOC ... so it stays valid;
verification is per-track."* **The first half is not what the code does.**

`fetchARData` ([CDRipper.cpp:400-566](src/CDRipper.cpp#L400)) takes
`const std::vector<CDTrack>& tracks` and derives **everything** from it. Three couplings:

### (1) The disc ID is computed FROM THE PASSED VECTOR

```cpp
int ntracks = (int)tracks.size();
...
for (int i = 0; i < ntracks; ++i) {
    uint32_t rel_lba = tracks[i].start_lba - AR_PREGAP;
    disc_id1 += rel_lba;
    disc_id2 += std::max(rel_lba, 1u) * (uint32_t)(i + 1);   // POSITIONAL weight
}
disc_id1 += rel_leadout;
disc_id2 += rel_leadout * (uint32_t)(ntracks + 1);
```

A subset changes `ntracks`, changes which LBAs are summed, **and changes the `(i+1)` weighting of
every remaining track**. The result is a different disc ID for a disc that has not changed → the
lookup 404s.

### (2) Every response chunk is rejected unless its track count matches

```cpp
if (tc != (uint8_t)ntracks) { pos += (size_t)tc * 9; continue; }
```

Even if an ID somehow collided, every chunk from a 12-track disc would be skipped when `ntracks` is
3.

### (3) Results are indexed BY POSITION IN THE PASSED VECTOR

```cpp
out_v1.assign(ntracks, {});
...
if (t < ntracks) { out_v1[t].push_back({main_crc, conf}); }
```

AR chunk position maps to vector position. With a subset, track 7's verdict would land in slot 1.

### What that means in practice

**Ripping tracks 3, 7 and 9 by passing a 3-element vector would produce a rip that completes, writes
files, and reports every track as unverified.** It fails *quietly* - no crash, no error - which is
the worst shape a defect can have in a verification feature. Anyone who took the brief's premise at
face value would have shipped exactly that.

### The conclusion IS reachable - by a different route

**Keep passing the full TOC to `fetchARData` and give the worker a separate selection.** Then:

| coupling | under the full-TOC design |
|---|---|
| disc ID | correct - computed over all 12 tracks, unchanged |
| chunk filter | correct - `ntracks` is the disc's count |
| `out_v1[t]` | correct - positional over the full TOC |
| a selected track's verdict | `out_v1[its index in the full TOC]` |

`fetchARData` needs **no change at all**. The change is in the worker: extract only selected tracks,
and look their verdicts up by TOC position rather than by loop counter.

**`AR_PREGAP = 150` is not touched, read, or reasoned about by any of this.** Nothing in the
proposal approaches the LBA origin.

### CTDB is a different answer: **genuinely incompatible**

`computeCTDBId` is a CRC32 over the **entire disc audio** with the first and last 10 sectors trimmed
([:570](src/CDRipper.cpp#L570)), accumulated streaming as the rip runs (`ctdb_state.ctdb_bytes`),
and the rip log states the scope itself: *"one verdict for the whole disc, not per track"*
([:2080](src/CDRipper.cpp#L2080)).

**A partial rip cannot produce a valid CTDB ID.** There is no partial form of it. So
`RipMode::CUETools` has to be handled explicitly under selection - see §7's slice list.

---

## 5. What else does a whole-disc rip produce?

Read from the worker's tail ([:2150-2260](src/CDRipper.cpp#L2150)):

| product | subset still sensible? | notes |
|---|---|---|
| **audio files** | yes | the point of the feature |
| **`.cue` sheet** | **NO** | describes a *disc*, and embeds `REM DISCID` from the full TOC. A cue listing 3 of 12 tracks describes something the user does not have. **Recommend: skip on a partial rip**, with the log saying so. |
| **`.m3u8`** | yes | it is a list of the files written; a shorter list is honest |
| **tags** | yes, with one care | `tagFile` writes track number and CTDB fields. **TOTALTRACKS must stay the disc's total**, not the selection size, or every tagged file misdescribes the album |
| **AR results** | yes, per §4 | one verdict per selected track |
| **CTDB status** | **no** | §4; the tags carry `CTDBDISCSTATUS`/`CTDBDISCID`, which must be omitted rather than written wrong |
| **ReplayGain** | **needs a ruling** | `std::vector<RGResult> rg_results(total)`. Track gain is fine. **Album gain computed across 3 of 12 tracks is not the album's gain.** Options: compute track gain only on a partial rip, or write album gain and accept it is the selection's, or omit. Recommend: **track gain only, album gain omitted on a partial rip**, stated in the log |
| **cover art** | yes | per-file embed, unaffected |
| **rip log** | yes | should state the selection explicitly |

---

## 6. What is already selection-shaped? **The rip modal itself**

This is the most useful finding for the design, and it is better than the convert precedent.

**`rip_sel_` is already a selection inside the rip confirm modal.** It is a
`std::vector<RipFormat>` ([UIManager.h:589](include/UIManager.h#L589)) toggled by number keys in
`UIOverlay::RipConfirm`, drawn with per-row on/off state ([:2218](src/UIManager.cpp#L2218)), with:

- **commit keys inert at zero selected** - `if (rip_sel_.empty()) return;` ([:6566](src/UIManager.cpp#L6566))
- **the selection normalised to table order at commit**, so behaviour does not depend on toggle
  order ([:6590](src/UIManager.cpp#L6590))

That is precisely the contract a track selection needs, one level over. **The feature is a second
selection in a modal that already has one**, sharing its idioms: a vector, a draw that shows state,
a commit gate, and order normalisation.

`drawConvertScope`/`drawConvertConfirm` remain the precedent for *scope-then-confirm*, and
`MarkSet` for a keyed set - but neither is as close a fit as the mechanism already living in the
very modal this feature extends.

---

## 7. HTOA - how the proposed shape carries it

**Nothing HTOA ships here.** The requirement is that adding it later is an addition.

**The design decision that achieves that:** a selection entry must **carry its own AR-verdict
source**, rather than the worker assuming a selected item has one.

Concretely - a selection row is `{ what to extract, optional TOC index }`:

- a **track** carries its TOC index; its verdict is `out_v1[index]`
- **HTOA**, later, is a row with **no TOC index**; its verdict is *absent by construction*

If instead the worker looks verdicts up by loop counter or assumes every selected row has an AR
entry, HTOA later forces a restructuring of the results plumbing. **Making the verdict source
optional from the start is the whole cost of carrying HTOA, and it is one field.**

The brief's stated property - *a selected row that produces a file with no verification result must
not read as a failure* - falls out of that: "no AR entry" and "AR checked and failed" become
different states rather than the same empty result. **That distinction has to exist in the results
type, not just in the display**, which is the one thing worth insisting on now.

**No placeholder row, no HTOA extraction path, no hidden-track detection** is proposed or needed.

---

## 8. Proposed slices - riskiest and most-provable first

Four slices, of which the first is where the risk actually lives.

| # | slice | why here | gate |
|---|---|---|---|
| **1** | **Decouple "the disc" from "what to rip" in the rip path.** `start()` gains a selection (indices into the TOC); `fetchARData` keeps receiving the FULL TOC unchanged; the worker extracts only selected tracks and resolves verdicts by TOC index. **No UI.** Default = all selected → byte-identical to today. | This is the entire correctness risk (§4), it is provable without any UI, and everything else sits on it | *Relish* whole-disc rip: 12/12 AR v2, confidence 200, byte-identical CRCs - proving the default path did not move. Then a 2-track selection with real AR verdicts |
| **2** | **The side-products under a partial rip** - cue skipped, TOTALTRACKS held to the disc total, CTDB omitted, album gain omitted, log states the selection | Correctness of what lands on disk, still no UI. Separated from 1 so a failure is attributable | inspect the output of a partial rip; whole-disc output unchanged |
| **3** | **Selection UI in the RipConfirm modal**, following `rip_sel_`'s contract: toggle, show state, order-normalise, and a commit gate at zero selected | Wiring over a proven mechanism; first user-visible slice | eyes-on: select, deselect, rip, cancel; select-none is inert |
| **4** | **`RipMode::CUETools` under selection** - refuse, or offer whole-disc-only, per Dos's ruling | Isolated, and the only mode with no partial form | eyes-on |

**Rough count: 4.** Slice 1 is the one that can go wrong; 2-4 are bounded.

### Machine-testable vs hardware-only

- **Machine-testable**: that the disc ID is computed over the full TOC regardless of selection
  (this is the §4 defect, and it is pure arithmetic over a fixture TOC - it would fail today if the
  naive subset route were taken); that verdicts resolve by TOC index; that an empty selection is
  rejected; that a selection normalises to table order. `ar_crc_test` and `cd_toc_test` already
  exist as homes.
- **Hardware-only**: every actual rip, all AR/CTDB network verdicts, and all eyes-on. The *Relish*
  gate cannot be replaced.

---

## 9. What to cut, and non-goals worth fixing now

**Nothing here says "not worth it"** - the feature is real and the foundation argument for HTOA
holds. But three things should be cut before they grow, given how the library campaign ran:

- **No selection persistence.** A selection lives for one rip. Saving it invites "rip profiles",
  which is a different feature.
- **No range syntax, no "select all but N", no invert.** Toggle rows. The library campaign's
  `\`-search scope creep is the precedent for fencing this early.
- **No per-track format selection.** Formats stay disc-wide (`rip_sel_`). Crossing the two
  selections multiplies the modal's state for no stated need.
- **No reordering.** Selection is a set; output order is TOC order, as today.
- **No partial re-rip / resume.** "Rip the three that failed" is a different feature with its own
  state.

**And one non-goal that is really a constraint:** whole-disc rip behaviour must be byte-identical
when nothing is deselected. Slice 1 is built to make that testable rather than hoped for - the
default selection is "all", and the *Relish* gate is what proves it.

---

## 9a. RULING (Dos) - selection lives in the PLAYLIST PANE, not the modal

**Q4 is decided: marks toggle on CD track rows in the playlist pane while `[CD]` is active, using a
selection set separate from `marked_`. The modal gains one summary line, hidden when all are
selected.** Both load-bearing claims in the ruling were checked against the tree, and one is
understated.

### The modal genuinely cannot hold a track list - measured

`BOX_H = 17 + kRipFormatCount` ([UIManager.cpp:2132](src/UIManager.cpp#L2132)), and
`kRipFormatCount` is **6** (FLAC, MP3, WAV, Opus, WavPack, M4A -
[RipFormats.h:28-36](include/RipFormats.h#L28)). **So the modal is already 23 rows tall**, and it
already **grows with the format table** - the comment at :2131 says so explicitly ("2 rows -> 19,
3 -> 20, ...").

The app itself runs down to a 9-row terminal ([:577](src/UIManager.cpp#L577)), and the modal already
clamps its origin to 0 when it does not fit ([:2136](src/UIManager.cpp#L2136)). **A 23-row box on a
24-row terminal is the whole screen.** The ruling says "near its height limit"; the measurement says
there is no room for even a handful of extra rows, let alone a variable list that Red Book allows to
reach 99. **Confirmed, and not marginal.**

*(Observation, not a proposal: on a terminal shorter than 23 rows the modal is clamped to the top
and simply overruns. Pre-existing, unrelated to this feature, and out of scope - recorded only
because it was read while checking the height.)*

### The mechanism needs NO new binding - `u` is already free there

`case 'u'` is guarded `if (focus_ == Pane::DirBrowser)`
([UIManager.cpp:8211](src/UIManager.cpp#L8211)) and there is no pre-switch `if` for it. **In the
playlist pane `u` is a no-op today.**

So the ruling's mechanism is `u` gaining a *second context* rather than a new key: it already means
"mark this row" in the browser, and it would mean the same thing on a CD row. **No key sweep is
required and no free-ASCII budget is spent** - which is the best possible outcome given that the
free list has been wrong twice when handed over.

`U` (clear all marks) is guarded only by `!marked_.empty()` and would need a parallel branch for the
CD selection; that is one term, not a new binding either.

### What this changes in the plan

- **Slice 3 moves from the modal to the playlist pane.** It becomes: a CD-track selection set, `u`/`U`
  on CD rows while `[CD]` is active, a drawn marker on selected rows, and **one summary line in the
  modal that is absent when all are selected** - so the default path draws exactly as it does today,
  which is the same "additive only" property slice 1 proves for the rip itself.
- **Slices 1, 2 and 4 are unchanged.** The ruling is about where the selection is expressed, not
  about how the rip consumes it, and §4's full-TOC design is untouched.
- **The `rip_sel_` precedent (§6) still applies**, but as a *contract* rather than a location: a
  vector, drawn state, a commit gate at zero selected, and order normalised at commit. The playlist
  selection should honour the same four properties.

### It does improve the HTOA slot, and for a concrete reason

Under §7 the requirement was that a selection entry carry an **optional** TOC index. In the modal
that would have been a special-cased row in a fixed table. In the playlist pane it is **a synthetic
row beside the existing `G:CD Track NN` rows** - and the playlist already holds synthetic,
non-file rows by design (`addCDTrack` exists precisely to store a path that is not a file, and
`isCDTrackPath`/`parseCDPath` read them back).

So HTOA later is "one more synthetic row whose selection entry has no TOC index", in a pane that
already tolerates exactly that shape. **That is an addition, not a restructuring** - which was the
§7 requirement. Still not built, still not designed here.

**One thing the ruling does NOT resolve**, and it should be stated rather than assumed: the
selection set must survive the playlist being repopulated. `activateDrive` calls
`playlist_.clear()` and rebuilds the CD rows on every open, and the CD poll re-enters that path on
media change. A selection keyed on the synthetic path survives a rebuild that produces the same
paths; one keyed on row index does not. **Key it on the path**, the same conclusion `libnav`
reached about identity strings versus subscripts.

## 10. Open questions for Dos

1. **CTDB under a partial rip** - refuse the mode, or allow it whole-disc-only? (§4, slice 4)
2. **Album ReplayGain on a partial rip** - omit, or write the selection's? Recommend omit. (§5)
3. **The `.cue`** - skip on partial (recommended), or write one describing only what was ripped?
4. ~~**Where the selection is toggled**~~ - **RULED: the playlist pane. See §9a.**
