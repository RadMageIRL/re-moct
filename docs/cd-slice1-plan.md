# CD-S1 - decouple "the disc" from "what to rip"

**Design note. Not greenlit. Untracked until it is.**

Branch `experimental/win-pdcurses`, tip `8ff0a85`. No UI. Default = all selected.

---

## 0. Disconfirmation - the recon found three couplings. There are FIVE, and the two new ones are worse.

CD-R1 reported three places where `fetchARData` assumes the vector it is handed IS the disc. Reading
the **worker** rather than only the AR fetch turns up two more, and the second changes what "wrong"
means.

### (4) The multi-disc pick reads the track count

```cpp
const int current_disc = pickDiscForTrackCount(rel, (int)tracks.size());   // :1444
```

A subset would match the wrong medium of a multi-disc release - or none - putting output in the
wrong `Disc N` folder and tagging from the wrong disc's metadata.

### (5) `is_first` / `is_last` are DISC facts derived from LOOP POSITION - and they drive the AR CRC

```cpp
ARTrackResult ar = ripTrack(*dev, trk, i, total, i==0, i==total-1, ...);   // :1713
```

and inside `ripTrack` ([:1032-1036](src/CDRipper.cpp#L1032)):

```cpp
const uint32_t ar_check_from = is_first ? AR_SKIP : 1u;
const uint32_t ar_check_to   = is_last ? (total_samples > AR_SKIP ? total_samples - AR_SKIP : 0u)
                                       : total_samples;
```

**These select the AccurateRip CRC skip window** - AR's flat-disc scheme excludes `AR_SKIP` samples
at the very start of track 1 and the very end of the final track.

**So a naive subset does not merely fail to look up. It computes the WRONG CRC.** Rip track 5 alone
and `i==0` makes `is_first` true, so the first `AR_SKIP` samples of track 5 are skipped from its
checksum. That checksum can never match AccurateRip, and it is wrong in a way no lookup failure
would explain.

### The shape underneath all five

**`total` is one variable carrying two meanings, and `i` is one index carrying two.**

| | means "the disc" | means "what we're ripping" |
|---|---|---|
| `total` | disc ID, chunk filter, `is_last`, disc pick, CTDB | loop bound, progress denominator, result-vector size |
| `i` | position in the TOC (AR verdict slot, `is_first`) | position in this rip |

**They have only ever been correct because they coincided.** That is the same failure this codebase
has now paid for four times - `next_track_info_` (XF C1), `return_to` (LIB-S17), the single-slot
`marked_` role, and `total` here. The fix is the same one: **make the two meanings two things.**

---

## 1. The design

### 1.1 A pure, testable classifier - `include/RipSelection.h`

The whole of the risk is "which index means what", and that is pure arithmetic over a TOC size and a
selection. So it goes in a header with no device, no network and no CD, and gets a test - the same
move as `BrowserPins.h`, `PaneScroll.h`, `BrowserSections.h` and `LibraryNav.h`.

```cpp
namespace ripsel {

// One track to extract, carrying its DISC identity alongside its selection identity.
struct Item {
    int  toc_index;   // position in the FULL TOC - the disc fact
    bool is_first;    // is this the disc's FIRST track (toc_index == 0)
    bool is_last;     // is this the disc's LAST track  (toc_index == disc_total - 1)
};

// Build the extraction plan. `selected` is TOC indices, in any order.
// - out of range indices are dropped
// - duplicates are dropped
// - the result is sorted into TOC order  <- the rip_sel_ contract: behaviour
//   must not depend on toggle order
std::vector<Item> plan(int disc_total, const std::vector<int>& selected);

// The default: every track, in TOC order. plan(n, all) must equal this.
std::vector<Item> planAll(int disc_total);

}
```

**`is_first`/`is_last` are computed from `toc_index`, never from position in the plan.** That single
line is finding (5)'s fix.

### 1.2 What the worker does with it

`worker` keeps receiving the **full TOC** and gains a selection. Then, mechanically:

| use of `total`/`i` today | becomes |
|---|---|
| `pickDiscForTrackCount(rel, tracks.size())` | `disc_total` - **unchanged behaviour** |
| `fetchARData(tracks, ...)` | **not modified at all** - still the full TOC |
| `ar_results` / `rg_results` / `album_states` sizing | `disc_total`, so slot == TOC index |
| `ripTrack(..., i, total, i==0, i==total-1, ...)` | `ripTrack(..., item.toc_index, disc_total, item.is_first, item.is_last, ...)` |
| verdict lookup | `ar_results[item.toc_index]` - lines up with `out_v1[toc_index]` by construction |
| extraction loop bound | the plan |
| `p.track` / `p.total` progress | position in the plan / plan size |
| tagging, m3u, output files | the plan |
| CTDB | disc-level; CD-S4 refuses partial, so it only runs on a full plan |

**`AR_PREGAP` is not read, not passed, not reasoned about.** `fetchARData` is untouched.

### 1.3 The results type - the HTOA-carrying requirement

The constraint: *"no AR entry" and "checked and failed" must be different states in the results type,
not the display.*

**`ARStatus` already has the state**: `NotQueried` is the default and an unselected track simply
keeps it, distinct from `NotFound`. So the type needs **no new enum value** - which is the good
outcome, and it means HTOA later is a row whose result stays `NotQueried` by construction rather
than a special case.

**But one reader already conflates them** ([:2386-2393](src/CDRipper.cpp#L2386)):

```cpp
for (auto& r : ar_results) {
    if (r.status==ARStatus::Matched_v2) ++ar_v2;
    else if (r.status==ARStatus::Matched_v1) ++ar_v1;
    else ++ar_none;                       // NotQueried counted as "not found"
}
fprintf(lf, "AR: %d v2 + %d v1 matched, %d not found / %d total\n", ..., (int)ar_results.size());
```

With `ar_results` sized to the disc, every unripped track would report as "not found" and `total`
would be the disc's count. **Fixed here, in CD-S1, because it is the results-type requirement and
not cosmetics:** count only planned items, and report the plan's size.

---

## 2. What must NOT change - and how that is proven

**A full selection must produce a byte-identical rip.** The proof is structural, not hopeful:

- `planAll(n)` yields `toc_index == i` for every `i`, `is_first == (i==0)`, `is_last == (i==n-1)`.
  Substituting those into the call sites reproduces today's arguments **exactly**.
- `fetchARData` is not modified, so disc ID, chunk filter and `out_v1` indexing are untouched.
- `AR_PREGAP`, `ripTrack`'s body, `ar_crc`, the encoders and the offset logic are untouched.

**Machine-checkable, and it will be:** a test asserting `plan(n, {0..n-1}) == planAll(n)` and that
`planAll` reproduces the old `i==0` / `i==total-1` flags for every `n` in a range. That is the
default path pinned by arithmetic before *Relish* ever spins.

---

## 3. Fence

**Files expected to change:** `include/RipSelection.h` (new), `include/CDRipper.h` (the `start` and
`worker` signatures), `src/CDRipper.cpp` (the worker's index discipline and the summary counter),
`tests/rip_selection_test.cpp` (new), `tests/CMakeLists.txt`.

**Not touched:** `fetchARData`, `ripTrack`'s body, `ar_crc.*`, `AR_PREGAP`, `computeCDDB`,
`computeCTDBId`, the encoders, `CDSource`, `AudioManager`, the audio thread, `UIManager` (no UI in
this slice), `Version.h`, anything in `plugins/`.

**No UI.** The call site keeps passing every track; it simply passes a full selection.

---

## 4. Tests - `tests/rip_selection_test.cpp`, pure, both jobs

1. **`planAll` reproduces today's flags** for `n` = 1, 2, 3, 12, 99: `toc_index == i`,
   `is_first == (i==0)`, `is_last == (i==n-1)`. **This is the byte-identity argument in test form.**
2. **`plan(n, all) == planAll(n)`** - the default path is the general path.
3. **Order independence** - `plan(12, {9,3,7})` equals `plan(12, {3,7,9})`, in TOC order. The
   `rip_sel_` contract.
4. **`is_first`/`is_last` follow the DISC, not the plan** - `plan(12, {5})` yields one item with
   `toc_index==5`, **`is_first==false`, `is_last==false`**. *This is finding (5); it fails against
   any implementation that uses plan position.*
5. **Disc edges when selected** - `plan(12, {0})` → `is_first==true`, `is_last==false`;
   `plan(12, {11})` → `is_first==false`, `is_last==true`; `plan(1, {0})` → both true.
6. **Defensive** - out-of-range and duplicate indices dropped; empty selection yields an empty plan
   (the caller refuses to start, as `rip_sel_.empty()` already does for formats).

**Mutation-tested, verify-it-landed:** make `is_first` read plan position instead of `toc_index` and
confirm block 4 fails. That is the forbidden change, and it is the one a future reader would make.

---

## 5. Gate

**Machine:** both toolchains build, both suites green, `rip_selection_test` passes, and blocks 1-2
pin the default path.

**Hardware - and this is the slice's real gate:** ***Relish*, whole disc, nothing deselected -
12/12 AR v2, confidence 200, byte-identical CRCs against the retained baselines.** With everything
selected the rip must be byte-for-byte what it is today. **If it is not, stop.**

A partial rip is *not* gated here - there is no UI to express one yet, and the side-product rulings
land in CD-S2. What can be shown in this slice is the default path unchanged, which is the whole
claim.

**Verification split as in the LIB campaign.** Brace-balance and scoped-diff audit.

---

## 6. Raised - WITHDRAWN

An earlier draft of this note raised "two discs of a set sharing a track count are ambiguous today"
as a pre-existing weakness. **That was wrong, and it is withdrawn.** It was inferred from the
function's name and its call site; the body handles exactly that case and says so
([MBLookup.h:32-47](include/MBLookup.h#L32)):

```cpp
// Unambiguous single match wins; otherwise (no match, or two discs sharing a count)
// fall back to disc 1.
...
return (nmatch == 1) ? found : 1;            // exact unique match, else disc 1
```

It counts matches and refuses to guess: a unique track-count match wins, anything else falls back to
disc 1 deliberately. Multi-disc sets were handled when this shipped. **Nothing to raise, nothing to
fix.**

Recorded rather than deleted because the error is the standing one - a claim about the CD path
assembled from a signature instead of a read, which is the exact failure mode this campaign's
disconfirmation clause exists to catch, arrived at by me this time.

**Finding (4) is unaffected and still stands**, for a different reason than the withdrawn claim: it
is not that the function is ambiguous, it is that **a subset changes `n_physical`**. Pass 3 of a
12-track disc and the count either matches no medium - falling back to disc 1, wrong for disc 2 - or
coincidentally matches another disc's count and picks that one. Passing `disc_total` keeps the
existing, correct behaviour intact.
