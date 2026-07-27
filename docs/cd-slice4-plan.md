# CD-S4 design note - CUETools, and what actually causes `status=4`

**Status: PROPOSED, not greenlit. Untracked until sign-off.**
Last slice of the per-track rip campaign. Follows CD-S1/S2/S3, all shipped.
**No TUI - see §5, so the gate is mine, as with CD-S1 and CD-S2.**

---

## 0. A correction I owe first

After CD-S2 I reported this as *"the same NotQueried-versus-NotFound conflation"*. **That was wrong,
and the brief inherited it from me.** Read out of the live tree:

```
enum class ARStatus { NotQueried, Matched_v2, Matched_v1, NotFound, NetworkError, ReadError };
//                        0            1            2          3          4           5
```

**`status=4` is `NetworkError`. It is not `NotFound`, and it is not `NotQueried`.** So this is not a
reader conflating two adjacent meanings. It is a **writer asserting a failure that never happened**,
and six readers faithfully repeating it.

## 1. The actual defect, and it reaches the user's files

`fetchARData` is called **only in AccurateRip mode**
([CDRipper.cpp:1605](../src/CDRipper.cpp#L1605)). In CUETools mode it is never called, so
`ar_db_loaded` stays false and `checkAR` does this
([:1780](../src/CDRipper.cpp#L1780)):

```cpp
result.status = ARStatus::NotFound;
if (!ar_db_loaded) { result.status = ARStatus::NetworkError; return result; }
```

**In CUETools mode `!ar_db_loaded` does not mean the network failed. It means nobody asked.**

Measured on the CD-S2 gate's own CUETools rip, `smoke/cds2-ctdb-full/01.flac`:

```
ACCURATERIP=AR: network error
ACCURATERIPCOUNT=0
ACCURATERIPCRC=a377572a
```

**Every file from a CUETools rip carries a permanent, false claim that AccurateRip had a network
error.** It is in the user's library, in their tags, forever. That is materially worse than a log
line, and it is the single strongest reason this slice exists.

### Every reader, swept as the brief asked

| # | reader | site | today (NetworkError) | after the writer fix |
|---|---|---|---|---|
| 1 | **file tags** `arStatusStr` | [:668](../src/CDRipper.cpp#L668) | **`AR: network error`** | `""` -> **tag omitted** |
| 2 | Pass 1 log line | [:1838](../src/CDRipper.cpp#L1838) | **`no match`** | still wrong, needs a label |
| 3 | Pass 2 log line | [:1984](../src/CDRipper.cpp#L1984) | ~~`no match`~~ **see 1b** | **not defective - untouched** |
| 4 | progress callback | [:2076](../src/CDRipper.cpp#L2076) | `ripped (AR skipped)` | correct |
| 5 | `disc.json` `arStr` | [:2410](../src/CDRipper.cpp#L2410) | **`network_error`** | `not_queried`, correct |
| 6 | summary counter | [:2473](../src/CDRipper.cpp#L2473) | **counts as not-found** | still wrong, needs a bucket |
| 7 | summary detail line | [:2501](../src/CDRipper.cpp#L2501) | **`[AR net err]`** | still wrong, needs a label |

**Six of seven are wrong today, all downstream of one assignment.** Two are already right, and both
are right for the same reason: their fall-through happens to land on the honest answer
(`default: return ""` for tags, `default: "not_queried"` for `disc.json`, the latter written under
CD-S2).

**This is the fourth instance of the class, not the third** - the brief counted three. And the
answer to "writer or shared helper" is **the writer first**: fixing the assignment makes readers 1,
4 and 5 correct with no further change, including the one that touches the user's files.

### 1b. Reader 3 is NOT defective - a correction found while building

I listed the Pass 2 line as having the same `no match` fall-through. **It does not.** Opened:

```cpp
ar2.status==ARStatus::Matched_v2 ? "AR v2 OK" :
ar2.status==ARStatus::Matched_v1 ? "AR v1 OK" :
ar2.crc_v1==ar.crc_v1 && ar2.crc_v2==ar.crc_v2
    ? "same as pass1 (disc deterministic)" : "different from pass1 (read error)";
```

Its fall-through is not an AR-status label at all - it is a **determinism comparison between the
two passes**, which is exactly what `[B]` Local 2-pass exists to report, and it is honest in every
mode. **Left untouched.**

I had read that line off a grep window without opening the body, which is the failure this
project's disconfirmation clause exists to catch and which I have now made twice in this campaign
(the other was `pickDiscForTrackCount` in CD-S1). **So it is six readers with three defective, not
seven with four.**

### 1a. Which MODES are affected - MEASURED, after a challenge

The first draft of this note framed the defect as CUETools-only, and that framing was an inference
about scope I had not measured. PM asked whether `[Y]` Local and `[B]` Local 2-pass carry it too,
since `ar_db_loaded` is false in those as well. **Measured on the retained `modeY/`, `modeB/` and
`modeC/` gate rips (2026-07-16, so this predates the whole campaign):**

| surface | `[A]` AccurateRip | `[C]` CUETools | `[Y]` Local | `[B]` Local 2-pass |
|---|---|---|---|---|
| **file tags** | real verdict | **`AR: network error`** | **absent** | **absent** |
| Pass 1/2 log line | real | **`no match`** | **`no match`** | **`no match`** |
| summary counter | real | **`12 not found`** | **`12 not found`** | **`12 not found`** |
| summary detail | real | **`[AR net err]`** | **`[AR net err]`** | **`[AR net err]`** |
| `disc.json` | real | **`network_error`** | **`network_error`** | **`network_error`** |
| progress line | real | `ripped (AR skipped)` | correct | correct |

**The inference was half right, and both halves matter.**

**WRONG about the tags.** Local and Local 2-pass write **no** `ACCURATERIP` tag, because the tag
write is separately guarded at [:707](../src/CDRipper.cpp#L707) -
`ar_str = isLocal(mode) ? "" : arStatusStr(...)` - and `isLocal` is `Local || LocalVerify`. CUETools
is **not** in that guard, so it alone falls through to `arStatusStr(NetworkError)`. So the
tag severity is exactly what §1 said: **CUETools only.** It does not reach the more-used quick-rip
modes.

**RIGHT about everything else, and my note understated it.** The false **log** and the false
**`disc.json`** are written in **all three** non-AR modes, not one. That is three times the breadth
I claimed.

**And in the Local modes it is starker still**: the AR CRC is never computed there
(`ar::TrackCrc arc(..., !isLocal(mode))`, [:1058](../src/CDRipper.cpp#L1058)), so the log reports
`no match` beside `crc_v1=00000000 crc_v2=00000000`. It claims a comparison against a checksum that
was never calculated.

**The one-line writer fix already covers all three**, because it gates on
`mode == RipMode::AccurateRip` rather than naming CUETools. Worth stating as measured rather than
left as a happy accident: `[C]`, `[Y]` and `[B]` all become `NotQueried` together.

---

## 2. Part 1 - is there CUETools work left?

**The partial-CTDB ruling is FULLY DELIVERED. Nothing remains of it.** CD-S2 made the combination
unexecutable in `start()`; CD-S3 made it unselectable at the modal and demoted the refusal to a
backstop. Verified in the tree, not assumed. I would have said "Part 2 only" and closed it.

**But Part 1 is not empty**, because the probe found real CUETools work that was not what the plan
reserved the slice for: **CUETools mode lies about AccurateRip, in the user's files.** That is
squarely CUETools work, it is user-facing, and it is worth the slice on its own.

So: the ruling the plan anticipated is done; the slice earns its place for a different, measured
reason.

---

## 3. Proposed design

### 3.1 The writer - one line, and the root

```cpp
// AR is fetched ONLY in AccurateRip mode. Anywhere else, !ar_db_loaded means
// nobody asked - and NetworkError asserts a failure that never happened, which
// ends up stamped into the user's files as "AR: network error", permanently.
if (!ar_db_loaded) {
    result.status = (mode == RipMode::AccurateRip) ? ARStatus::NetworkError
                                                   : ARStatus::NotQueried;
    return result;
}
```

`checkAR` captures `[&]`, so `mode` is already in scope at all three call sites (1809, 1911, 1977).
**In AccurateRip mode the value is unchanged**, so every AR-mode output stays byte-identical - which
the stop condition requires.

### 3.2 One shared label, so a fall-through cannot swallow a value again

Six readers hand-roll a ternary or switch over one enum, and the wrong ones are wrong *because* a
fall-through absorbed a value nobody listed. Two labels now live in `include/ArStatus.h` -
`arStatusLabel` (the short bracketed form, reader 7) and `arStatusPassLabel` (the longer Pass-line
form, reader 2). **Two rather than one because the two sites genuinely word things differently**,
and forcing a single vocabulary would change `[A]` output in paths the gate does not exercise, for
no gain.

**What actually protects against a seventh instance, stated exactly** - because I first wrote this
section overstating it and the build disproved it:

- `-Wswitch` **warns** on a missing case, but **only in TUs built with `-Wall`**. That is the app
  (`CDRipper.cpp` includes this via `CDRipper.h`). **The test targets build with plain
  `-std=c++20`**, so nothing is emitted there. And there is **no `-Werror` anywhere in the
  project**, so it is a warning even where it fires. Measured: the mutation below compiled silently.
- **`ar_status_label_test` FAILS.** That is the hard gate, and it is why the fall-through returns a
  deliberately absurd `"AR ???"` rather than a plausible string - a missing case has to be loud
  somewhere that blocks, and a plausible fallback would have made the mutation pass.

### 3.3 The counter - a fourth bucket, added only when it is non-zero

`else ++ar_none` counts "never asked" as "asked and not found". Proposed:

```
AR: %d v2 + %d v1 matched, %d not found / %d total          <- unchanged when nothing is unqueried
AR: %d v2 + %d v1 matched, %d not found, %d not queried / %d total
```

The fourth term appears **only when the count is non-zero**, which in AccurateRip mode it never is -
`checkAR` always assigns a real verdict there. So **AR-mode logging is character-identical**, and the
CUETools log stops claiming twelve failed lookups that were never attempted.

### 3.4 Wording (the brief asked, it is user-facing in the log)

- Pass 1 / Pass 2 lines: `status=0 (not queried)`
- Summary detail: `[AR not queried]`
- Summary total: `, N not queried`
- File tags: **nothing**. A rip that never asked AccurateRip should say nothing about AccurateRip,
  and `arStatusStr`'s existing `default: return ""` plus the `if (!ar_str.empty())` guard already
  produce exactly that once the status is right.

---

## 4. Raised, not fixed - the possible next one

`fetchCTDBData` uses `"Unknown"` both as its **initial value** and as an explicit **response case**
([:605, :646](../src/CDRipper.cpp#L605)), so "we never got an answer" and "the answer was
unrecognised" are the same string. It is the same family, but it is **not the same severity**: it
asserts nothing false, it says "unknown", which is at worst imprecise rather than a lie. It is also
CTDB's own status vocabulary rather than ours.

**Raised per the brief, not fixed.** If it is wanted, it is post-campaign work and wants its own
number.

---

## 5. TUI? No - so the gate is mine

The changed surfaces are the status assignment, three log readers, the counter, and a tag that stops
being written. The one reader that renders into the TUI is the progress callback, and it is
**already correct** (`default: "ripped (AR skipped)"`) and is not touched. **No TUI, so I gate this,
as with CD-S1 and CD-S2.**

---

## 6. Gate

**Machine:** both toolchains, both suites. `arStatusLabel` is pure and testable; a test asserting
every enum value maps to a distinct non-empty label - including `NotQueried` - is the thing that
stops a seventh reader repeating this. Mutation: delete the `NotQueried` case and confirm it fails.

**Hardware, on *Relish*, disc identified by TOC first.**

1. **Whole disc, AccurateRip mode: byte-identical.** 12/12 AR v2, confidence 200, CRCs identical to
   the retained baselines, **and the AR-path log character-identical**, summary line included.
   **Stop condition.**
2. **Whole disc, CUETools mode:** no `ACCURATERIP` tag in the files at all; log reads `not queried`
   rather than `no match`; the summary stops reporting twelve not-found; `disc.json` reads
   `not_queried`; **the CTDB verdict itself unchanged - same ID `8a950d34`, same `tracks=12` lookup
   URL.**
3. **Whole disc, `[Y]` Local mode - the proof the fix is MODE-GENERAL, per §1a.** Log and
   `disc.json` read `not queried` rather than `no match` / `network_error`, and **no `ACCURATERIP`
   tag appears** - i.e. the `isLocal` guard at :707 still holds and the fix did not start writing
   one. `[B]` Local 2-pass shares that exact code path (`isLocal` covers both) and is not spent a
   separate rip; that is stated rather than silently skipped.
4. **Partial rip:** CD-S3 unregressed, `[C]` still greyed and inert.

**Expected diffs, named in advance** (the CD-S2 `schema_version` discipline), corrected by §1a:

- **CUETools `[C]` only:** the `ACCURATERIP*` tags disappear from the files.
- **All three non-AR modes `[C]`, `[Y]`, `[B]`:** the per-track log label changes `no match` ->
  `not queried`, the summary gains a `not queried` term and stops counting those as not-found, the
  detail line reads `[AR not queried]`, and `disc.json` reads `not_queried` instead of
  `network_error`.
- **AccurateRip `[A]`: nothing changes at all**, tags, log, counters, `disc.json` or otherwise.
  That is the stop condition.

---

## 7. Constraints

`fetchARData` unmodified, still receiving the full TOC. `AR_PREGAP` not read, reasoned about or
approached. CD-S3's selection behaviour unchanged. Audio thread sacred, nothing in streaming. No
HTOA, no partial CTDB, no new artifacts. **CHANGELOG: yes** - a CUETools rip stops mislabelling
files, which is user-facing.
