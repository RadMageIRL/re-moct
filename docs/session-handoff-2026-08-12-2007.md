# Session handoff - 2026-08-12 20:07

Branch `experimental/win-pdcurses`, tip **`bb3acbc`** - **no commit made this session.** Six tracked
files are modified in the working tree and nothing is staged.

Continues `docs/session-handoff-2026-08-11-2137.md`. That handoff's §9 listed three open C2 decisions
and a set of dead stores. **Dos ruled on all of them, then ruled a fifth item after it was flagged
mid-session. This session executes those rulings and nothing else.**

---

## NEXT SESSION STARTS HERE

1. **Nothing is running, nothing is half-done.** Both toolchains gated green after the final tree.
2. **THE C2 QUESTION IS CLOSED.** Recon'd and **declined**, with reasons, in `docs/roadmap.md`
   **Decisions log** - the top entry. **Do not re-open it without new information: a damaged disc to
   test against, or a reference implementation changing its mind.** Everything else about C2 in this
   tree is now either a true statement or deleted.
3. **1.6.2 is still UNRELEASED and the ceremony is still outstanding** - `docs/session-handoff-2026-08-11-2137.md`
   §6, items 1-4, unchanged. **This session's changes are unreleased 1.6.2 material too**, so they
   ride along with that merge; they add nothing to the ceremony list.
4. **READ THE CONFIGURE BANNER before trusting any build.** Still the cheapest mistake available.
5. `docs/LOCKED-CODE.md` before anything near the CD path, with the declaration line. **Most of this
   session's edits are in CD-path files**; the declarations are at §1.1 and §2, both "does not
   touch".
6. **The rip log now says "not queried" on Windows, and that is the only behavioural change here.**
   §2. Linux output did not move by a byte.

---

## 1. What was done - the ruling, item by item

### 1.1 The two false comments in tracked source: CORRECTED

**This change touches / does not touch AR_PREGAP, `ar_crc.*`, or the read addressing.**
**Does not touch.** Comment text only in both files; not one statement, expression or signature
changed. `readRaw`'s body and `RAW_READ_INFO` setup are byte-identical.

| file | what it said | what it says now |
|---|---|---|
| `src/platform/win/CdIoWin.cpp`, `readRaw` | *"the caller's buffer size, passed through untouched, IS the request"* | `want_c2` is DISCARDED and **cannot** be honoured through this IOCTL, with the measurement that proves it |
| `include/core/ICdIo.h`, the `readRaw` contract | repeated the same claim as *"advisory on Windows"* | honoured by SG_IO, **discarded** by Windows; `got` reports what arrived, never what was asked for |

Both now carry the measurement rather than the conclusion: `RAW_READ_INFO` has no C2 field,
`TrackMode=CDDA` delivers 2352 B/sector whatever `out_size` says, and on the GHD3N - one probe run,
same disc, same sector - READ CD (0xBE) flag byte `0x12` **via SPTI** returned 2646 bytes while the
IOCTL with a 2646-byte buffer returned 2352.

**The `CdIoWin.cpp` comment points at `docs/roadmap.md`, not at the RECON notes.** First draft
pointed at `docs/RECON-c2-capability.md` and that was wrong: **it is untracked.** A tracked source
comment must not cite a file that does not exist in the repository - the pointer has to survive a
clone. Caught before the gate; worth remembering the next time a comment cites a doc.

### 1.2 The dead code: DELETED

| symbol | where it was | how it died |
|---|---|---|
| `total_c2_errors` | `CDRipper::ripTrack` - decl + one `+=` | both lines gone |
| `RipProgress::using_c2` | `include/CDRipper.h` + two writes in `CDRipper.cpp` | field and both writes gone |

Neither was reachable-by-data on the only platform with a drive attached, and neither was read
anywhere in the tree. **Confirmed by measurement, not by reasoning:** Windows warnings went 20 -> 19,
exactly the one row, every other row identical in kind and count (§4).

**`c2_errs` and the `C2!` / `C2` indicator were KEPT, deliberately.** The ruling named two symbols
and those two are gone. The remaining chain - `c2_errs` -> `readSectors`' `c2_error_count` out-param
-> the 294-byte scan - is **live code fed by live locals**, not a dead store: permanently unreachable
on Windows for the reason above, but reachable in principle and correct on SG_IO. Deleting it was not
ruled and was not done. **Nothing became newly unread as a result of the two deletions** - that was
the open question from the 08-09 recon and the answer is no.

### 1.3 The decision record

**`docs/roadmap.md` Decisions log, top entry.** That location was chosen over `docs/CLAUDE.md` for a
reason worth keeping: **CLAUDE.md already points there** - *"Roadmap, phases, parked items, decisions
-> `docs/roadmap.md`"*. So the decision is reachable from the index **without spending any of the
201/200 line budget**, which is what item 3 of the ruling asked for.

The entry records all three reasons (no reference support, no test material, no complaint driving it)
**and** the measured capability facts, so re-opening it requires disagreeing with something specific
rather than simply not knowing.

### 1.4 `docs/CLAUDE.md`: NOT touched

Per the ruling. Fixing the comments removed the risk the entry would have guarded against - the
danger was someone reading `ICdIo.h` and believing it. **That line is now true, so it no longer needs
a correction stored somewhere else.** Still 201 lines, still no headroom.

---

## 2. The rip log: FIXED, on the CD-S4 shape

**This change touches / does not touch AR_PREGAP, `ar_crc.*`, or the read addressing.**
**Does not touch.** Two output strings and one file-scope constant in `CDRipper.cpp`. No read path,
no CDB, no addressing.

Ruled by Dos after the above was flagged: **say what is true - that C2 was not queried, not that the
drive doesn't support it** - and the named precedent is the 1.5.0 AccurateRip fix, *"a rip that never
asked stopped reporting a failure."* That precedent is `ARStatus::NotQueried`, CD-S4, still in the
tree at `CDRipper.cpp` - *""never asked", which is NOT the same fact as NotFound ("asked, no
match")"*. **Same distinction, applied to C2.**

**One constant carries the fact, gated once with the reason** (`CDRipper.cpp`, above `probeC2`):

```cpp
#ifdef _WIN32
static constexpr bool C2_QUERYABLE = false;   // IOCTL_CDROM_RAW_READ discards want_c2
#else
static constexpr bool C2_QUERYABLE = true;    // SG_IO sets CDB byte 9 = 0x12
#endif
```

**It is deliberately not a drive property and the comment says so.** It answers "can this build ask",
which is exactly the question the old code conflated with "can this drive answer".

| | before | after |
|---|---|---|
| Windows log | `C2 support  : no` | `C2 support  : not queried (no C2 request path on this platform)` |
| Windows status | *"C2 not supported by drive - using standard rip with retry"* | *"C2 not queried on this platform - standard rip with retry"* |
| **Linux, both** | | **character-identical - not one byte changed** |

**`probeC2`'s own comment was a THIRD copy of the false claim** and was corrected in the same pass:
it said *"C2 comes back iff the drive supports it and the buffer is sized for it"*, and its doc line
said *"Returns true if drive supports C2"*. The ruling named two comments; this is the same sentence
in a third place, and it is the function whose return value feeds the line being fixed. Leaving it
would have made the fix look arbitrary to the next reader.

**Verified in the binaries, not just the source** - `strings` on both:

| string | `remoct.exe` | `remoct` (Linux) |
|---|---|---|
| `not queried (no C2 request path on this platform)` | **present** | absent |
| `C2 not supported by drive` | **absent** | present |
| `using C2-assisted rip` | **absent** | present |

The dead branch is gone from each binary, so **neither build can emit the other's claim.** That is
the check worth repeating for any `constexpr`-gated message: the source proves intent, the binary
proves the fold.

---

## 2b. What the fix turned up - a false premise in a TRACKED phase-3 doc

Checking whether the old string was load-bearing found that it was: `docs/phase3-slice6-design.md`
§2 and its **THE gate** item 2 both name `"C2 support: no"` as one of the fields the Linux SG_IO rip
had to match **byte-identically against the Windows baseline log**. That gate is passed and closed
(Phase 3 complete, 2026-07-04), so nothing re-runs - but the doc states as fact:

> *"GHD3N is non-C2 (baseline prints "C2 support: no")"*

**That is false, and it is load-bearing for the item it justifies.** The GHD3N advertises C2 on both
MMC queries and returned 2646 bytes to a direct `READ CD`. The Windows half of that "two mechanisms
converge" argument was never a drive fact at all - `got=2352` was guaranteed by the discarded
parameter. **Which leaves the Linux half unverified:** it claims byte 9 = `0x12` produced CHECK
CONDITION *because the drive is non-C2*. If the real reason was something else, then on Linux
`use_c2` is **TRUE** on this drive and the rip takes a C2 de-interleave path **that has never run on
hardware**.

**Not testable here** - the WSL box has no `/dev/sr*`, which is the same wall the 08-09 recon hit.
**Nothing depends on it while C2 stays declined** - but it is *reachable* code reached by a real
configuration, not dead code, and **the reasoning that called it safe rested on something false.**

**Recorded as an OPEN HARDWARE QUESTION** - `docs/roadmap.md`, the entry immediately after the C2
decline, deliberately beside it because that is where a future session already has to look. Marked
open, not decided: **the decline does not answer this, and closing C2 did not close it.** The entry
names what would settle it and admits nothing less will: **RE-MOCT on a real Linux install with the
GHD3N attached** - not a hypervisor's virtual CD, which lessons.md already records as a *different
drive* with a different offset. One line of the rip log decides it: `C2 support  : yes` means the
premise was wrong and the de-interleave is live and untested; `: no` means it holds, and the
**reason** should then be captured from the sense data instead of assumed a second time.

A dated correction also sits above the item in `docs/phase3-slice6-design.md`, pointing at the
roadmap entry and saying plainly that the item is not established.

---

## 3. Docs changed

| file | tracked | what |
|---|---|---|
| `docs/roadmap.md` | **yes** | **Decisions log: the C2 decline**, with the measured facts and all three reasons - the durable record; **plus the OPEN HARDWARE QUESTION beside it** (§2b), marked open, with what would settle it |
| `docs/lessons.md` | **yes** | one bullet in **CD transport / SG_IO** (the IOCTL cannot request C2; buffer size is not a request); three in **Process / workflow** (see §5) |
| `docs/phase3-slice6-design.md` | **yes** | dated CORRECTION above §2: *"GHD3N is non-C2"* is false, and what that costs the Linux arm of that argument. **Original text left intact** - it is a closed acceptance record, so it gets a correction, not a rewrite |
| `docs/warn-sweep-plan.md` | no | §1 table and total corrected to **19**; new §11 closing out §10.2-§10.4 |
| `docs/RECON-c2-capability.md` | no | CLOSED banner at the top; body unedited |
| `docs/RECON-c2-integration.md` | no | CLOSED banner; **kept, because most of it is not about C2** |

**`warn-sweep-plan.md` was edited despite being not-greenlit, and the 08-11 handoff deliberately did
not edit it.** The difference is the reason: that session declined to record *line drift* there,
which is churn. This session deleted **the subject of one of its rows**, and that file is the
**baseline every future warning diff is read against** - leaving it saying 20 would have made the
next honest build look like it had lost a warning, or gained one. A stale gate baseline is worse than
a stale line number.

`RECON-c2-integration.md` is kept rather than deleted because its six collisions are **not about
C2**: they describe RE-MOCT's actual entry-point parsing (there is no argv parser), TUI screen model,
test-fake reach, and `disc.json` writer. Any future subcommand or read-only panel hits the same
three that make such work greenfield.

---

## 4. Gates

**Windows** - full clean rebuild (`--clean-first`) of the final tree. Banner read and quoted:
`STATIC PROBE: preferring .a archives over .dll.a import stubs`,
`curses: PDCursesMod wingui (vendored, static) - Option C`, `CMAKE_BUILD_TYPE:STRING=` (empty,
verified in `CMakeCache.txt`). Build `EXIT=0`, **ctest 56/56**. `ldd` shows exactly two UCRT64 DLLs:
`libebur128.dll`, `libfdk-aac-2.dll`. `remoct.exe --version` reports **`RE-MOCT v1.6.2-win`**.

**Linux** - WSL Debian, full clean rebuild, `ncursesw` in the banner (correct; `REMOCT_PDCURSES` is
`WIN32`-gated). Build `EXIT=0`, **ctest 57/57**.

**Warning diff - the only intended change, and it is a deletion:**

| | before | after |
|---|---|---|
| Windows | 20 | **19** |
| Linux | 8 | **8** (unchanged - GCC 14.2 never flagged `total_c2_errors`) |

The removed row is `CDRipper.cpp - total_c2_errors set but not used`. Full Windows inventory after:
8 `-Wmissing-field-initializers`, 2 `-Wunused-but-set-variable` (`ar_none`, `wrows`), 2
`-Wmisleading-indentation`, 2 `-Wformat=`, 2 `-Wdelete-non-virtual-dtor`, 1 `-Wcpp`, 2 untagged
`MOUSE_MOVED` redefinitions. **Same set, same counts, minus exactly one.**

**An incremental build cannot produce this diff and nearly hid it.** The first build after the edits
was incremental and reported 16 warnings - not because 4 were fixed, but because only the recompiled
TUs emit. **A warning count from an incremental build is meaningless; only `--clean-first` counts.**
Both numbers above are from full clean builds, and the Windows one was re-run after a late
comment-only edit so `build_win.log` matches the final tree.

**Line numbers, both toolchains agreeing:** `CDRipper.cpp:532, 707`; `UIManager.cpp:11850`,
`13014`, `13111`; `595, 598` unmoved. **These are 39 lower than the 08-11 handoff §7 recorded** -
that entry looks to have been taken from a pre-commit build. Identifiers unmoved, as ever.

Both toolchains were re-gated clean **after** the log-wording change, and the counts above are the
post-change numbers. The `#ifdef` constant added no warning on either.

**Hardware gate - one line, and only on Windows.** The ruling's *"nothing changes behaviour except
the deletions"* held until Dos ruled the log fix in; that change is now the one behavioural delta,
and it is confined to **two strings on the never-asked path**. The rip path, the read path, the CRCs
and every other log field are byte-identical, and **Linux output is unchanged to the byte**. Worth
one glance at the next Windows rip log to see `C2 support  : not queried (...)` where it used to say
`no` - nothing else in that file should differ.

---

## 5. Lessons recorded durably (not only here)

Added to `docs/lessons.md` - **Process / workflow**, the section CLAUDE.md points at:

1. **"Untested" and "untestable without a production change" are different findings.** The C2
   de-interleave is not an ordinary coverage gap: it is file-`static` in a TU nothing links against,
   and the one CD fake drives `CDSource`, which passes `want_c2=false` unconditionally. **No
   test-only change can reach it.** Check reachability before writing "untested" - the answer changes
   what anyone can do about it. *(The ruling singled this one out.)*
2. **Query the device, never infer from the model number - and take more than one statement.** The
   defect lived in the **disagreement** between the drive and the OS path, not in either alone.
   Keep "can the device" and "does our code ask" separate; collapsing them is what put a sentence
   blaming the hardware into the rip log for the life of the feature.
3. **A warning inventory built from compiler output under-reports its own subject.**
   `total_c2_errors` warned; `using_c2` - same dead store, same feature - did not, because GCC does
   not track struct fields. A sweep scoped to "what the compiler flags" is a sample, not a census.
4. **Before changing an output string, grep the docs for it - output strings are sometimes
   acceptance criteria.** That grep is what found §2b. **The grep that protects the change is also
   the one that audits the reasoning behind it.**
5. **A `constexpr`-gated message is proven by the binary, not the source.** `strings` each build and
   confirm the other platform's claim is absent.

And in **CD transport / SG_IO**: the `IOCTL_CDROM_RAW_READ` mechanism fact, so anyone touching that
transport meets it there rather than re-deriving it.

---

## 6. Still open, unchanged

- **1.6.2 ceremony** - merge to `dev`, merge to `main`, tag, flip the CLAUDE.md paragraph.
  `docs/session-handoff-2026-08-11-2137.md` §6.
- **OPEN HARDWARE QUESTION: does the GHD3N answer C2 over SG_IO?** - §2b above, recorded in
  `docs/roadmap.md` beside the C2 decision. **Settled only by RE-MOCT on a real Linux install with
  the drive attached.** Do not treat `phase3-slice6-design.md` §2 as established until then.
- **`ar_none`** (`CDRipper.cpp:2924`) - the last `-Wunused-but-set-variable` on Windows. The sweep is
  still not greenlit and this session did not touch it.
- **The EQ palette borrow** - `drawEq`. Ruled a non-goal; **logged in `docs/lessons.md`**, and the
  live hazard is the comment above it, which hard-codes default Classic colours into prose.
- **The unexplained pairing** - `The Sanctuary of Zi'Tah` + the FFXI artist. Leave open, do not chase.
- `selection.disc_total` means TRACKS while `disc.disc_total` means MEDIA - deliberately not renamed.
- **Cover art candidates** - may want re-scoping rather than resuming.

---

## 7. Working tree

Committed on `experimental/win-pdcurses` - seven modified files plus this handoff: `docs/roadmap.md`,
`docs/lessons.md`, `docs/phase3-slice6-design.md`, `include/CDRipper.h`, `include/core/ICdIo.h`,
`src/CDRipper.cpp`, `src/platform/win/CdIoWin.cpp`.

The source half is 4 files, +53/-22. **Of that, exactly six lines are executable**: the `#ifdef`
constant and the two rewritten ternaries. Everything else is comment or deletion. **The gates in §4
were run against this exact source tree** - every edit after them was documentation.

New untracked build logs from this session's gates: `cfg_win.log`, `cfg_lin.log`, `ctest_win.log`,
`ctest_lin.log` (joining the existing `build_*.log`). Not intended for tracking.

**Recent handoffs ARE tracked** (`08-07-1326`, `08-08-0044`, `08-08-0136`, `08-11-2137`), so this one
should be committed with the change. The older ones from July remain untracked by standing decision.
