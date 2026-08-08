# Session handoff - 2026-08-08 01:36

Branch `experimental/win-pdcurses`, tip **`3203747`**, pushed, tracked tree clean.
Continues `docs/session-handoff-2026-08-08-0044.md`, which is still correct about everything up to
the picker. **Read that one first** - this covers only what came after it, and the earlier one holds
the wrong-product build lesson, the GCC 16 upgrade and the measurements behind the picker's design.

**No ceremony, no merge, no tag.** Dos is casual-testing and will circle back.

---

## NEXT SESSION STARTS HERE

**Everything below is DONE, GATED ON BOTH TOOLCHAINS, AND CONFIRMED ON HARDWARE.**

1. **Nothing is waiting on you.** Four commits after the last handoff, all pushed and confirmed.
2. **READ THE CONFIGURE BANNER before trusting any build.** `docs/CLAUDE.md` has the flags. This is
   still the cheapest mistake available.
3. **The outbound rule is now a standing rule, in `docs/CLAUDE.md`: THE DISPLAY MAY GUESS, THE
   RECORD MAY NOT.** Anything that leaves the machine goes through `cd_identity_`; nothing outbound
   re-derives which disc it is. §2 is why.
4. **1.6.1 ceremony is still the next substantive thing and still Dos's call.** The three cosmetic
   decisions from two handoffs ago are unchanged and still open.
5. **`docs/LOCKED-CODE.md` before anything near the CD path**, with the declaration line. Dos ruled
   this session that *calling* a locked function unchanged is a read, not a touch - but declare the
   adjacency and hand him the decision anyway. That is what he asked for explicitly.

---

## 1. What happened after the picker shipped

A regression report, and it turned out to be two defects plus a third found while tracing.

| commit | what |
|---|---|
| `49d94ed` | Q1: the scrobbler stops deriving its own disc - the fifth site REMOVED, not fixed |
| `f074840` | Q2: a release is stamped with the disc ID it was adopted for |
| `3203747` | Q3: an assumed medium never scrobbles |
| `d8bec68` | (before these) docs + the previous handoff |

### The symptom

Playing **Mellon Collie disc 2 track 5 (*1979*)** with the header, title bar and every playlist row
correctly saying `Disc 2/2`, **ListenBrainz recorded *Here Is No Why*** - disc 1, track 5. Same index,
wrong medium. And earlier the same evening, FFXI tracks scrobbled under an artist (伊賀あゆみ) who is
credited on no disc of that set that was ever in the drive.

### The mechanism, which is the part worth keeping

**A stale release paired with a live track count does not fail - it SUCCEEDS WRONGLY.** `n_phys` was
recounted from the current playlist on every call while `rel` only changed on a lookup, so when they
described different discs `pickDiscForTrackCount` found whichever medium of the OLD release happened
to have the NEW disc's track count, and returned it with full confidence. That is why the *artist*
changed and not merely the title: a wrong medium of a multi-composer release credits different people.

The diagnosis ran backwards from the artist. `Tu'Lia` credited to 伊賀あゆみ exists in exactly one
place across all 4 releases and 15 media of that disc ID: **PREMIUM BOX medium 7, track 8**. That
pins `n_phys == 10` at scrobble time, which no other reading explains.

---

## 2. Q1 - the fifth site is gone, not fixed

Four consumers were converged on `resolvedPick()` when the picker shipped. **`updateScrobbler()` was
missed**, and it is the one whose output is permanent. It could not see `mb_disc_override_` (symptom
1, my own regression) and it re-resolved a stale release (symptom 2, pre-existing since multi-disc
support).

The fix was not to route it through the same resolver. **`cd_identity_` is written by
`applyReleaseTitles` at the moment it titles the rows** - raw artist/title/album by CD track - and
the scrobbler reads that. Same release and same medium as the screen *by construction*, because
there is no second derivation left to drift from.

`pickDiscForTrackCount` now survives only in comments.

**Lifetime is the other half of the fix.** The identity is cleared with the rows it describes:
`purgeCDRows`, every `openCD`, and beside all three `mb_release_` clears. The CD open path already
cleared `cd_sel_` for exactly this reason - CD-S3's comment says the synthetic paths are
*disc-independent*, so anything keyed by track number survives a swap unless explicitly dropped. **The
tree had already written down the failure, next to the field that got it right.**

---

## 3. Q2 - a release remembers which disc it was taken for

Nothing checked that a cached release described the disc actually in the drive, and the three
clearing sites fired on **eject only**. A disc swapped while stopped goes through
`reopenCDForAction`, which re-reads the TOC and **cleared nothing at all** - almost certainly how
this happened.

**The reframe is the useful part.** The check is not "does this release describe this disc" - that is
unanswerable for a `Ctrl+F` pick, where choosing outside the disc-ID candidate list is the entire
point and a mismatch is the expected state. It is **"was this release adopted FOR this disc"**, which
is always answerable and needs no special case.

`adoptReleaseLocked` - the one door every release comes through - stamps `mb_release_disc_id_`; every
successful `openCD` calls `dropReleaseIfDiscChanged()`. A different disc drops release + stamp +
override + candidates + identities together and says so. An empty stamp (adopted with no disc loaded,
which `Ctrl+F` allows) is claimed by no disc.

**No debounce, and none needed:** the drop empties the release and only fires when there IS one, so
at most once per adoption.

### The stability probe - do not re-derive this

A spurious drop would cost a choice the user made, so this was **measured, not argued**. A standalone
probe against the real `CDSource` + `CdIoWin` (source in the scratchpad, `discid_stability_probe.cpp`):

```
drive F, 4 opens:  tracks 1..14  lead-out 288024  id=fycwEKoS8tQPZ0v307GDz3bEytA-   identical x4
drive G, 2 opens:  tracks 1..12  lead-out 275940  id=L2QyrSF.KymhdN4yLYjQwrF29aw-   identical x2
```

Stable within a disc, **different between discs** - so it discriminates rather than merely being
constant, which a naive constancy check would have missed. `fycwEKoS8tQ...` is also byte-identical to
the ID derived hours earlier from that disc's stored `disc.json` TOC by a completely different route:
two independent derivations agreeing.

---

## 4. Q3 - a guess may be shown but must not be recorded

Of the five uncertain states, **four already sent nothing** after Q1 and Q2 (no lookup yet, track
index outside the medium, empty map after a swap - all reach `cd_unmatched`). Building guards for
them would have been building for cases that no longer exist.

**Only the assumed-medium case still leaked**, and only it was built. The predicate is
`pick.ambiguous()` from the very `DiscPick` that titled the rows, recorded in the same function at
the same instant, so the suppression and the red on-screen warning cannot disagree. It is already
false once a medium is chosen, so choosing resumes scrobbling with no extra bookkeeping.

**Scrobblers only.** `publishMedia()` (SMTC/MPRIS) and Discord still publish, below the same line
`cd_unmatched` already draws: they show what the screen shows and replace themselves next track, so
blanking them would be the worse lie. One is a record, the others are a view.

**`ambiguous()` is gated on `total > 1`**, so a single-disc album and a unique track-count match both
scrobble exactly as before. The common case cannot be caught by this.

Suppression announces itself once per **track** (not per tick), as a toast rather than the status
line - the status line already holds the *cause*, and overwriting an explanation with its consequence
loses the more useful half.

### The documented exception

**A `Ctrl+F` release is trusted and its tracklist is NOT re-checked against the disc.** That is what
makes searching by name a usable repair for a bad automatic match, and it is now in the CHANGELOG as
policy rather than left to be rediscovered as a bug. It is also **the one remaining way a
confidently-wrong scrobble can leave the machine**, by the user's own choice.

---

## 5. Testing reality (unchanged from the last handoff)

| | |
|---|---|
| Windows | **56/56** |
| Linux | **57/57** |
| new warnings | zero, both toolchains, every commit |

Nothing new is unit-testable here: all three fixes are UI-thread state and lifetime, and the parts
that *are* pure (`DiscPick`, the reporting helpers, the row formatter, the wrap) already have
coverage in `disc_pick_test`. **The disc-ID stamp was verified by probe on real hardware; the rest
was verified by Dos.** Named so nobody reads 56/56 as covering this work.

---

## 6. STILL OPEN

- **The unexplained pairing.** `The Sanctuary of Zi'Tah` + 伊賀あゆみ exists nowhere in the data, and
  the code takes title and artist from the same `MBTrack`, so it cannot be manufactured. Most likely
  Dos read the artist off the line below. **Ruled: leave it open, do not explain it away, do not
  chase it.** If it recurs it becomes real.
- The three 1.6.1 ceremony decisions: six missing CHANGELOG link definitions, the split `v1.5.0` tag
  naming, the CHANGELOG never read end to end.
- `selection.disc_total` in the sidecar means TRACKS while `disc.disc_total` means MEDIA - flagged,
  deliberately not renamed (published in 1.5.0).
- The two GCC 16 dead stores (`total_c2_errors`, `ar_none`), on `docs/warn-sweep-plan.md`.
- Cover art candidates - deferred, never designed.
- O4 follow-ups: nothing outstanding; the re-openable picker shipped as `F5`.

---

## 7. Untracked, unchanged, by standing decision

17 older session handoffs, the four not-greenlit notes (`warn-sweep-plan.md` **which was updated
tonight and left untracked**, `SCOPE-podcasts.md`, `RECON-gap-handling.md`,
`RECON-playing-predicate.md`), and the `tools/src/cap*/`, `np_*/` scratch. The fence was checked
before each of tonight's commits: staged sets contained only code, tests, CHANGELOG and docs.

---

## 8. Process lessons from this stretch

1. **Work backwards from the anomalous field.** The artist, not the title, is what identified medium
   7 and proved the mechanism instead of merely fitting it. A title alone would have supported three
   theories.
2. **Trace, do not patch.** Fixing the drain order would have shipped the `mb_disc_override_` twin
   sitting two lines away. The third defect (a repeat `Ctrl+R` discarding a chosen disc) turned up
   the same way.
3. **Remove the site rather than fix it.** Five places deriving one decision is the shape that caused
   this; converging the fifth would have left the shape intact.
4. **Check which cases still exist before guarding them.** Four of Q3's five states had already been
   closed by Q1 and Q2. Guarding them would have been dead code that looked like diligence.
5. **Measure the property the design cannot tolerate losing.** A disc ID that was constant but not
   discriminating would have passed a naive check and been useless.
6. **Declare locked-code adjacency and hand over the decision.** Dos ruled that a read is not a
   touch, and said to do it that way every time.
