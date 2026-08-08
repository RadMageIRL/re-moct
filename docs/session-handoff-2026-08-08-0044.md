# Session handoff - 2026-08-08 00:44

Branch `experimental/win-pdcurses`, tip **`50e7793`**, pushed, tracked tree clean.
Supersedes `docs/session-handoff-2026-08-07-1326.md`, which is still correct about 1.6.1's
display fold, the CJK crash and the move/resize work - this one adds everything after it.

**No ceremony, no merge, no tag.** Dos is casual-testing and will circle back.

---

## NEXT SESSION STARTS HERE

**Everything below is DONE, GATED ON BOTH TOOLCHAINS, AND CONFIRMED ON HARDWARE unless it says
otherwise.** Nothing is mid-flight and nothing is broken.

1. **Nothing is waiting on you.** Four commits tonight, all pushed. Dos is testing casually.
2. **You can build. Do build.** The old "Claude cannot compile → Dos builds → reports" loop is
   gone; `docs/CLAUDE.md` now says so. Both toolchains, every slice.
3. **READ THE CONFIGURE BANNER BEFORE TRUSTING A BUILD.** See §2 - this cost a full rebuild of the
   wrong product tonight and Dos caught it from the binary size, not from anything I checked.
4. **The 1.6.1 release ceremony is still the next substantive thing, and still Dos's call.** §9
   has the readiness state and the three decisions that remain open from last session, unchanged.
5. **Read `docs/LOCKED-CODE.md` before proposing anything near the CD path**, and open such a
   proposal with the declaration line. It forbids the discussion, not just the change.

---

## 1. What shipped tonight: the disc-number campaign

Four slices plus a follow-up and O4, in the order they landed. The through-line: **the program knew
which disc it had and never said, and when it did not know it guessed in silence.**

| commit | what |
|---|---|
| `0384696` | slices 1-4: the disc number in tags, the silent tie made loud, the disc surfaced before the rip, the release/disc picker |
| `9d21960` | the `Ctrl+F` medium-stage gap, and the ambiguity note running off the modal |
| `50e7793` | O4: `F5` re-opens the picker, the repeat-`Ctrl+R` message, the one adoption rule |

Recon and design live in this session's conversation only; the durable rules are now in
`docs/CLAUDE.md` under "1.6.1: which disc this is".

### Slice 1 - the disc number is written at all

`include/DiscTag.h` is the one definition per container: ID3v2 `TPOS`, Xiph `DISCNUMBER` +
`TOTALDISCS`, APEv2 `DISC`, MP4 `disk`. WAV has no container and never reaches it.

**`1/1` is written too** - Dos's ruling. Absence used to mean both "one disc" and "nobody wrote
one"; it now means only "not ripped by this version".

**The header exists so the test can call the shipping writer.** The risk was never `tagFile`'s
control flow - it was whether TagLib bridges each native field onto `DISCNUMBER`, which is what
`LibraryScanner::readTags` asks for. `disc_tag_test` writes real encoded files with
`disctag::writeDiscTag` and reads them back with `scanCollection()`. **APEv2 `DISC` and MP4 `disk`
both bridge** - the two I flagged as least certain - **on TagLib 2.2.1 AND 2.0.2**, which is a
stronger result than one platform would have given.

### Slice 2 - the silent tie

`DiscPick` carries the `matches` count `pickDiscForTrackCount` used to compute and discard. Rip log
gains a `Disc :` line always; `disc.json` goes to **schema 4**; `applyReleaseTitles` says so on the
cmdline at the moment wrong titles actually appear.

### Slice 3 - surfacing, and a rename

`buildOutputDir` now returns the complete path including `Disc N`; the worker stops appending and
receives it. That split was the whole reason the confirm modal and the pre-rip line were
*structurally incapable* of naming the folder the rip would use. Modal gains the disc row and the
ambiguity line; `Disc N tracks` became `Tracks N`; the modes indicator is composed once at the draw
by `releaseLabel()`, retiring `mb_album_` - a folded string held in state, which is the shape the
1.6.1 fold rule exists to prevent.

**The rename:** `disc_total` meant the MEDIA count in `tagFile` and the TRACK count in `worker`, in
one file, one letter-order from `total_discs`. The track reading is `toc_track_count` now, in
`CDRipper.cpp`, `RipSelection.h` and `CdTrackSelect.h`.

### Slice 4 - the picker

Parser keeps every release (cap 12, truncation *reported*). `MBDiscIdCallback` on the disc-ID entry
only; `MBRelease` gained only `disambiguation`. `UIOverlay::MBPick`, two stages, shared row
formatter with the `Ctrl+F` list. 0 candidates unchanged, 1 applies with no modal, 2+ opens the
picker with the cursor on candidate 1 so Enter reproduces the old behaviour in one press. **Escape
cancels** and says so. `Ctrl+F` was ungated for Linux in the same slice.

### O4 - `F5`

Re-opens without touching the network, asking the same question the forward flow would ask now
(skip rules in reverse), cursor on the release **or disc already in force**, matched by `mb_id`.
`Ctrl+R` is untouched and still re-queries, but a repeat now says
`MB: same release as before (4 candidates). F5 to choose.`

---

## 2. THE WRONG-PRODUCT BUILD - the lesson that generalises

`rm -rf build` discarded cache options I had not inspected. I reconfigured with bare defaults and
built an **ncursesw** binary instead of the vendored wingui one, then read a clean `EXIT=0` and 55/55
off it. Dos caught it from the binary size and from it not running.

**A green build of the wrong product is indistinguishable from a green build.** The banner is the
only thing that distinguishes them, and I had not read it.

Required Windows configure, now in `docs/CLAUDE.md`:

```
cmake -S . -B build -G Ninja -DREMOCT_PDCURSES=ON -DREMOCT_STATIC_PROBE=ON
```

`CMAKE_BUILD_TYPE` stays **empty** - BUILD.md's `-DCMAKE_BUILD_TYPE=Release` is wrong for this
machine, because empty is what keeps asserts live. Banner must read
`curses: PDCursesMod wingui (vendored, static) - Option C` + `STATIC PROBE: preferring .a archives`,
and `ldd build/bin/remoct.exe | grep ucrt64` must show exactly **`libebur128.dll` and
`libfdk-aac-2.dll`**. The wrong build shows 19.

---

## 3. TOOLCHAIN CHANGED UNDER 1.6.1 - GCC 15.2 → 16.1

Dos ran `pacman -Syu` mid-session: **104 packages**. Of ours only `gcc` (15.2.0-13 → 16.1.0-6),
`cmake` (4.2.3 → 4.4.2), `curl` and `ncurses` moved.

**TagLib did NOT move.** It is 2.2.1-1 and that is what MSYS2 ships; upstream 2.3.1 (2026-07-20) is
not packaged. Getting it would mean vendoring or building from source - not currently worth it, and
`disc_tag_test` is now the canary that would catch a bridge regression across such a bump.

**`libfdk-aac-2.dll` kept its soname**, so packaging and the `LOCKED-CODE.md` reference are
unaffected - that was the single most likely breakage.

**Dos re-ran the 1.6.1 live tests on GCC 16 and they behave as before** (CJK render, resize crash,
move-drag freeze, flicker fix). 1.6.1 stands as gated. Those four have no automated coverage; his
hands were the only available evidence, which is why raising it beat assuring it.

**Two new warnings**, both `-Wunused-but-set-variable` on pre-existing dead stores
(`CDRipper.cpp` `total_c2_errors`, `ar_none`), verified verbatim in the pre-slice tree. Added to
`docs/warn-sweep-plan.md` (still untracked). **Windows total 18 → 20; Linux unchanged.**

---

## 4. THE TWO GAPS FOUND AFTER SLICE 4 SHIPPED

Both were mine, both found by Dos on hardware, both fixed in `9d21960`.

**The medium stage never fired after a `Ctrl+F` pick.** The design put that decision in one place in
the run loop so every route got it - but the check was "open it *now*, if the screen is free", and
after a `Ctrl+F` pick it is not. That callback raises `mb_titles_pending_` and
`mb_search_close_pending_` together; the titles block runs first, the search modal is still up, the
guard fails, and the flag has already been consumed by the time the modal closes. **A `Ctrl+F` pick
landing on an ambiguous release silently kept disc 1** - the exact defect the feature exists to
remove, reintroduced by a drain order.

It is `mb_medium_pending_` now: a flag that survives ticks and opens the stage when the screen frees
up, whatever frees it. Not armed while the picker is showing, or escaping stage 1 re-arms forever.

**The same trace found the twin:** both `Ctrl+F` callbacks set `mb_release_` directly and never
cleared `mb_disc_override_`, so a medium chosen for one release carried into the next.

**Tracing beat patching here.** Fixing the drain order would have hidden the second bug entirely.

**And a third, from O4:** every path that adopted a release cleared the override *unconditionally*,
so a repeat `Ctrl+R` handing back the SAME release discarded a disc the user had chosen. There is
one rule now - `adoptReleaseLocked()` - and it clears only when the release actually changes. An
empty `mb_id` never counts as a match, or every Discogs release would look like every other.

---

## 5. Measurements that decided design - do not re-derive these

Raw JSON, fetched with `curl` and dumped structurally. **The MusicBrainz web service answers
questions about this correctly; a summarizing read of a large JSON document does not** - one gave me
5 FFXI releases when there are 4, and a self-contradictory media count on the next call.

- **Every medium of every release in a disc-ID response carries a full `tracks[]`.** 15/15 for the
  FFXI disc ID, 16/16 for Mellon Collie's. So **picking is instant and offline** - no
  `lookupByMbid` follow-up, and the disc column is computable for every row before the pick.
- **Mellon Collie disc 2 resolves to 4 releases**, all titled identically, every one ambiguous for a
  14-track disc: media shapes `[14,14,21,20,23,15]`, `[14,14]`, `[14,14,8,6]`, `[14,14,8,6]`.
  `releases[0]` is the 6-disc box - which is exactly the `Disc 1/6` Dos saw. Two of the four differ
  only in **`disambiguation`** ("30th anniversary edition, remastered") and a track-title spelling.
  **That measurement is why `disambiguation` is in the row at all**, and why the formatter reserves
  its space *before* truncating the title rather than appending it.
- The disc ID for that disc was computed from Dos's existing rip's `disc.json` TOC using RE-MOCT's
  own algorithm; it resolved, which incidentally validates the transcription.

---

## 6. Testing reality

| | |
|---|---|
| Windows | **56/56** (was 54/54: `disc_tag_test`, `disc_pick_test` added) |
| Linux | **57/57** |
| new warnings | zero, both toolchains, every slice |

`disc_pick_test` is header-only and links nothing. `disc_tag_test` links the real encoders and
`LibraryScanner.cpp` so assertion (C) is `scanCollection()` itself.

**The `Ctrl+F` modal and `F5` were driven on Linux in a real pty via tmux** - `Ctrl+F` had never
executed on that platform, and never-executed is not known-good. Script:
`<scratchpad>/linux_modal.sh`; re-run it after touching either modal's draw path.

**Still unexercised on Linux: the picker's own screens.** They need a disc and WSL has no
`/dev/sr*`. Named rather than implied.

---

## 7. What the tests caught that review did not

- **The disambiguation was being truncated away** on first run - the exact failure it was added to
  prevent, because appending it made it the last thing in the string and so the first thing cut.
- **`releaseLabel` inside `#ifdef PDCURSES`** compiled clean on Windows and failed on Linux with
  three "not declared in this scope". A Windows-only green build would have hidden it completely.
  **The second toolchain earns its place.**
- **Greedy wrapping** put the ambiguity note into 71 columns + a second row reading only `wrong`,
  which is correct and looks like a rendering fault. Two-line notes balance at the word boundary
  nearest the middle now.

---

## 8. Untracked, still, by standing decision

Unchanged from last session and **not to be committed unasked**: 16 session handoffs (this makes
17), the four not-greenlit notes (`warn-sweep-plan.md` - **which I updated tonight and left
untracked**, `SCOPE-podcasts.md`, `RECON-gap-handling.md`, `RECON-playing-predicate.md`), and the
`tools/src/cap*/`, `np_*/` scratch with its logs.

The fence was checked before every commit tonight: staged sets verified to contain only code, tests
and CHANGELOG.

---

## 9. RELEASE READINESS - unchanged, all three still open

1.6.1 is UNRELEASED, not merged, not tagged, deliberately. `Version.h` + `CMakeLists.txt` = 1.6.1;
CHANGELOG `## [1.6.1] - 2026-08-07` now carries the campaign under Added / Fixed / Changed;
**`docs/index.html` still says 1.6.0 on purpose** and does not describe any 1.6.1 feature - that is
ceremony work.

Still open, all cosmetic, all Dos's:

1. **CHANGELOG link definitions missing for six releases** - 1.3.1, 1.4.0, 1.4.1, 1.5.0, 1.6.0, 1.6.1.
2. **Tag naming is split**: every release tag is bare except **`v1.5.0`**, so mechanical link
   generation would produce one wrong URL.
3. **The CHANGELOG has never been read end to end.**

Ceremony recipe (from the workflow memory, as run for 1.3.0/1.3.1/1.5.0/1.6.0): finalise the
CHANGELOG date on experimental → ff `dev` → `git merge --no-ff dev -m "Merge dev into main: RE-MOCT
1.6.1"` → push main → annotated **bare** tag `1.6.1` on the main merge commit → push the tag. **`main`
carries direct GitHub web edits not in `dev`, so it is never ff-able - always `--no-ff`.** Stash
untracked WIP before the main checkout. **Dos does the clean-box build and the GitHub release.**

---

## 10. Open, flagged, NOT ruled

- **`selection.disc_total` in `disc.json` means the TRACK count** while the new `disc.disc_total`
  means MEDIA. Distinct paths, so a reader indexing by block is fine; a human skimming is not.
  Renaming it would break anything reading the 1.5.0-published field. Flagged, left alone.
- **The two GCC 16 dead stores** - real, small, each fixable in isolation. Explicitly not this work.
- **Cover art candidates** - deferred, never designed. CAA holds **38 images** for the FFXI PREMIUM
  BOX including one typed `Medium` commented `"3"` (the actual disc-3 artwork); the ripper asks a
  single-image redirect endpoint and never fetches the index. Needs a request that is not currently
  made, and because the disc↔image link is a human-written `comment` with no machine relation behind
  it, per-disc art can only ever be *offered*, never resolved.
- **Mixed line endings**: `include/CDRipper.h` and `include/MBLookup.h` are CRLF while the `.cpp`
  files are LF; `core.autocrlf=true` hides it from diffs. A `sed -i` converted `RipSelection.h` and
  `CdTrackSelect.h` to LF tonight. Harmless, noted so it is not mistaken for damage.

---

## 11. Process lessons worth carrying

1. **Read the configure banner.** §2. The most expensive mistake of the night and the easiest to
   have avoided.
2. **Do not `rm -rf` a build tree without inspecting its cache options first.** Sibling trees
   (`build-160`, `build-htoa`) carried the answer and I did not look until after.
3. **Trace, do not patch.** The `Ctrl+F` drain-order gap had a twin two lines away; reordering the
   drains would have fixed the symptom and shipped the other one.
4. **A summarizer over a large JSON document is not a measurement.** Refusing to report O1 when the
   read went bad was right; `curl` + a structural dump answered it in ten minutes.
5. **Force a recompile before claiming a warning count.** An incremental build reported zero
   warnings because it was a no-op, and I nearly reported that.
6. **Never-executed is not known-good.** `Ctrl+F` on Linux and `F5` anywhere were both compiled and
   both unexercised until driven in a pty.
