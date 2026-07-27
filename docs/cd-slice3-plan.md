# CD-S3 design note - the marking UI

**Status: PROPOSED, not greenlit. Untracked until sign-off.**
Follows CD-S2 (`471f96c`) and the recon closures (`eca05a0`). Branch `experimental/win-pdcurses`.
**This slice has a TUI, so the hardware gate is Dos's, not mine.**

---

## 0. What this slice owes

1. The marking UI itself, in the **playlist pane** (ruled, recon §9a).
2. **The `[C]` greying debt from CD-S2.** CD-S2 made CTDB-plus-partial *unexecutable*; this slice
   makes it *unselectable*. That split was recorded precisely so it could not fall between the two.

---

## 1. Findings from the probe

**P1 - `u` is genuinely free in the playlist pane.** [UIManager.cpp:8211](../src/UIManager.cpp#L8211)
gates the whole handler on `focus_ == Pane::DirBrowser`. Confirmed, so the ruling costs no new
binding.

**P2 - but `U` is NOT pane-gated, and that is a collision.**
[UIManager.cpp:8222](../src/UIManager.cpp#L8222) clears `marked_` from **any** pane. If `u` starts
marking CD rows in the playlist pane, `U` pressed there would clear the *browser's* convert marks
and leave the CD selection untouched - the opposite of what the user just did. **`U` has to become
pane-aware.** Not in the brief; raised here.

**P3 - `marked_` must not be reused, and cannot be.** It is the convert domain's set, and
`convertSupportedInput` denies `isCDTrackPath`, so CD rows can never enter it. CD-S3 needs its own
set. `MarkSet` ([include/MarkSet.h](../include/MarkSet.h)) is a clean reusable container
(`toggle/add/remove/clear/contains/empty/size/list`) - **reuse the type, not the instance.**

**P4 - track NUMBER is not TOC INDEX, and the engine wants the index.**
`cdTrackNumber` ([StringUtils.h:143](../include/StringUtils.h#L143)) recovers the number from a
synthetic path. `selected_toc` takes **0-based TOC indices**. `number == index + 1` holds on
Relish but is not guaranteed - data tracks and non-standard numbering break it. **Map by searching
`tracks` for the matching `number`, never by subtracting one.** This is exactly the class of
mistake CD-S1 existed to fix; doing it right here is the whole point of having found it there.

**P5 - a selection keyed on the path SURVIVES A DISC SWAP, which is a bug waiting to happen.**
The ruling to key on the synthetic path is right for surviving `activateDrive`'s
clear-and-repopulate. But the paths are `G:CD Track NN` - **disc-independent**. Eject Relish,
insert a different disc, and a selection of `G:CD Track 05` still matches, silently selecting
track 5 of a disc the user never marked. **The selection must be cleared on media change**, and
the CD poll already re-enters `activateDrive` on that event. Not in the brief; raised here.

---

## 2. Proposed design

### 2.1 Semantics: empty means all

`MarkSet cd_sel_`, keyed on the synthetic path. **Empty means every track**, which is exactly
`selected_toc`'s contract from CD-S1 and needs no populate-on-open. Marking one track means "rip
only that one". A user who never presses `u` gets today's rip, byte for byte, with no new state.

The alternative - populate everything as marked on open and have the user unmark - needs the set
rebuilt on every `activateDrive` and re-synchronised on media change, for no gain.

### 2.2 Keys

- **`u`** in the playlist pane on a CD row: toggle. Existing browser behaviour untouched.
- **`U`**: clear whichever set the focused pane owns (P2). In the playlist pane with CD rows, clear
  `cd_sel_`; otherwise clear `marked_` exactly as today.
- Both no-ops where they are no-ops today.

### 2.3 Rendering

A marker on marked CD rows in the playlist pane, following the browser's existing marked-row
treatment rather than inventing one. Row count and layout unchanged.

### 2.4 The modal

One summary line, **hidden when the selection is empty**, so the default modal is pixel-identical
to today. `BOX_H` ([UIManager.cpp:2132](../src/UIManager.cpp#L2132)) becomes
`17 + kRipFormatCount + (partial ? 1 : 0)`.

**`[C]` greyed while partial**, with `whole disc only` on the line - the CD-S2 debt. The mode row
lives at [UIManager.cpp:2254](../src/UIManager.cpp#L2254). Selecting it while partial must be
refused at the modal, so `start()`'s refusal stays a backstop rather than the user's experience.

### 2.5 Handing the selection to the engine

At the `start()` call site ([UIManager.cpp:6605](../src/UIManager.cpp#L6605)), map `cd_sel_` to
TOC indices per P4 - search `tracks` for each marked path's `cdTrackNumber`, drop anything that
does not resolve. Empty set gives `{}`, which is today's call unchanged.

### 2.6 Clearing

`cd_sel_.clear()` on media change and on drive re-entry (P5). Also after a successful rip, matching
how `marked_` is consumed and cleared at
[UIManager.cpp:6770](../src/UIManager.cpp#L6770).

---

## 2.7 Addition A - what unmarking everything means, stated rather than implied

**Marking track 5 and then unmarking it returns to ripping all twelve. It does not rip nothing.**

That is the only reading consistent with "empty means all", and the summary line disappearing at
the same moment makes it visually unambiguous - the modal returns to exactly its default shape. But
it is a decision, not a consequence to be discovered: the set has two states that both mean "no
marks" only because they are the same state, and a reader who assumed marking is subtractive would
expect the opposite. **There is no way to express "rip nothing", and there should not be** -
`start()` already refuses an empty plan, and the way to not rip is to not rip.

## 2.8 Addition B - the drop rule, and the HTOA slot it must not close

§2.5 drops marked paths that resolve to no TOC entry. **An HTOA row has no TOC entry, so under a
naive reading of that rule it would be silently dropped rather than extracted** - which would
quietly undo the reason the campaign was sequenced this way.

**Answer: it is a later restructure, and it is not foreclosed - but only if CD-S3 writes the drop
narrowly and visibly.** Three things, all cheap, none of them building HTOA:

1. **`selected_toc` structurally cannot carry HTOA and should not be stretched to.** It is a vector
   of TOC indices; HTOA has no index, and a sentinel like `-1` would be exactly the
   one-value-two-meanings defect this campaign has now closed twice. HTOA needs a **companion
   channel** on `start()` - a flag or a richer descriptor. That is the restructure, it belongs to
   the HTOA slice, and it is small because `ripsel::Item` is already a struct rather than a bare
   int.
2. **The mapping is a named pure function, not an inline loop** (§4), so the arrival of a second
   row kind is a compile-visible change at one site rather than a behaviour that silently differs.
3. **The drop is COUNTED, not silent.** The mapping returns the resolved indices *and* the number
   of paths that resolved to nothing. CD-S3 has no unresolvable rows in practice, so the count is
   always zero today - which is exactly why it must be returned rather than discarded: the day it
   is non-zero, something is being dropped that someone meant to rip.

So the distinction Dos asked about - "doesn't resolve, drop it" versus "resolves to no TOC index,
keep it" - **is real, and CD-S3 implements only the first**, deliberately. The second requires a
row kind that does not exist yet. What CD-S3 owes is to make the first one narrow, named and
observable so the second can be added beside it rather than carved out of it.

## 3. Decisions - all four RULED as proposed

1. **`U` is pane-aware.** `U` already means "clear marks"; clearing the marks of the pane you are
   looking at is the least surprising reading, and a separate key would spend a binding on a second
   name for one concept.
2. **Clear on media change, unconditionally.** The failure directions are not symmetric: clearing
   costs a redone selection, while not clearing silently rips tracks the user never marked on a
   disc they never saw.
3. **Row marker follows the browser's treatment.** A CD-specific glyph would invent a second visual
   language for one concept.
4. **The selection persists across closing and reopening the modal.** Mark eight tracks, open the
   modal, reconsider the format, close, reopen - losing the selection there would be infuriating.
   P5's media-change clear is the bound that matters. No persistence to disk; that is a fenced cut.

---

## 4. Gate

**Machine:** both toolchains, both suites. **Genuinely testable here, unlike CD-S2:** the
path-to-TOC-index mapping (P4) is pure and belongs in a test beside `rip_selection_test`, including
the case where `number != index + 1`. That is the one piece with real logic and no device.

**Hardware - Dos's, because this slice has a TUI.** Mark tracks in the playlist pane and rip only
those; rip with nothing marked and get today's whole-disc behaviour; `[C]` greyed while partial and
selectable when not; the summary line absent when nothing is marked; a disc swap clearing the
selection (P5); `U` clearing the right set from each pane (P2); browser convert marking unaffected.

---

## 5. Constraints

No persistence, no range syntax, no per-track formats, no reordering, no partial re-rip, **no
HTOA** - design for the row, do not build it. `fetchARData` unmodified. `AR_PREGAP` not approached.
Audio thread sacred, nothing in streaming. Whole-disc behaviour byte-identical for a user who never
presses `u`. **This slice earns the CHANGELOG line CD-S1 and CD-S2 both deferred**, because it is
the first user-reachable behaviour in the campaign.
