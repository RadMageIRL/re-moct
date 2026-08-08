# Session handoff - 2026-08-07 13:26

Branch `experimental/win-pdcurses`. **1.6.1 landed, no ceremony.**
Supersedes `docs/session-handoff-2026-07-27-1030.md` (which was written before the 1.5.0 and
1.6.0 ceremonies and is stale on its headline item).

---

## NEXT SESSION STARTS HERE

**Everything in this handoff is DONE and CONFIRMED ON HARDWARE unless it says otherwise.** Tip
`e3fcbd2`, pushed, tracked tree clean. Nothing is mid-flight and nothing is broken.

1. **Nothing is waiting on you.** The session closed clean: 1.6.1 unreleased on `experimental`,
   four commits, all gated Windows 54/54 + Linux 55/55, all live-tested by Dos.
2. **The 1.6.1 release ceremony is the next substantive thing, and it is Dos's call on timing.**
   §13 has the readiness state and the three decisions that are still open. §15 lists the five
   commits; §16 the process lessons, three of which are mine to own.
3. **Read `docs/LOCKED-CODE.md` before proposing anything near the CD path**, and open such a
   proposal with the declaration line. It forbids the discussion, not just the change.
4. **On Windows, set `wingui_font` to a CJK-capable font before judging Japanese rendering.**
   Bundled JetBrains Mono has no CJK. Boxes are a font result, not a fold result.
5. **Do not re-investigate the half-second pause when you press the title bar and hold still.**
   It is Windows' double-click wait, it dispatches nothing at all, and it is closed - §11.

---

## 1. What shipped: the display fold, rewritten

**Every 3- and 4-byte UTF-8 sequence was being replaced with one `?` per character before it was
drawn.** 水田直志 drew as `????` in browser and playlist rows. 2-byte sequences passed through, so
every accented Latin, Cyrillic and Greek name survived and all CJK, kana and emoji did not - which
is why this lasted four months with a repo full of Latin-1 test fixtures.

Recon `docs/RECON-nonascii-render.md`; design of record `docs/DESIGN-nonascii-display.md`.

### The new contract - `foldForDisplay`, `include/StringUtils.h`

Per codepoint, in order:

1. **REJECT** - not well-formed UTF-8 → one `?`, resume at the next byte.
2. **NORMALIZE** - in the table → its ASCII equivalent, possibly empty.
3. **PASS** - everything else, verbatim.

**Byte length is never a criterion.** That sentence is the whole fix.

Nine groups still normalize (quotes, primes, dashes, ellipsis, bullet, space variants incl. U+3000,
zero-width, ligatures, the ASCII fast path). **Group J - seventeen accented Latin letters - is
gone**, on Dos's ruling: it was a hand-picked list that stripped `Müller` while leaving `Ångström`
untouched on the same screen, and consistent stripping stopped being available the moment CJK was in.

**Renamed from `sanitizeForDisplay`** for the compile-time sweep - 104 sites, 8 files, zero
stragglers, fully mechanical.

## 2. The malformed-UTF-8 enforcement — READ THIS, it was not in the design note

**The old decoder only rejected invalid LEAD bytes.** Bad continuations, truncated tails, overlong
forms and surrogate halves were never checked - they were caught only *as a side effect of their
byte count*, because anything 3 bytes or longer fell to `?` regardless.

**Once length stopped deciding anything, that accidental guard disappeared.** Without adding a real
check, malformed bytes would have passed straight through to the terminal - and, since this same
text now feeds the scrobblers, onto the wire.

So the decode now validates properly: sequence length, every continuation byte, truncation, overlong
encodings, surrogate halves, and > U+10FFFF. Any failure yields `?` and **advances exactly one
byte**, so a bad sequence can never swallow the good text after it. Seven assertions pin it
(`playlist_encoding_test.cpp`, group I).

This is the one thing in the slice that was not in the approved design. It is not scope creep - it
is what "reject malformed UTF-8" costs once the length rule is gone. Named here because a future
reader will otherwise see strictness that no brief asked for.

## 3. Outbound data — the part that mattered most

**The brief for the design round assumed this was display-only. It was not, and the tree already
knew.**

`UIManager.cpp` (CD scrobble path) has said for a long time:

> *the playlist's display_title is combined "Artist - Title" AND ASCII-sanitized for the terminal,
> so it's unsuitable for scrobbling*

...and routed around the fold by reading the raw `MBRelease`. **The other two sources never were.**
A Japanese-tagged local file scrobbled `???? - ????` to Last.fm and ListenBrainz - permanently.

Fixed by moving the fold to the draw sites and keeping the producers raw:

- **`LocalFileSource`** stores raw tag text in `TrackInfo`.
- **`StreamSource`** stores raw in `now_playing_`. `parseIcyMetadata` had *always* been raw; only
  the iHeart and HLS-ID3 paths folded, so one station scrobbled differently depending on which
  transport carried the title. **All three agree now.**
- Fold moved to `drawTitleBar`, `drawTrackInfo` (one place - the `add()` lambda), `drawLyrics`,
  `drawProgress`, `drawRecPanel`, and the podcast info subject.

**Same rule the library index has always used** (`LibraryIndex.h`): raw in, fold at the draw.

### The tag editor was corrupting files, and nobody knew

`tag_edit_values_[0] = ct.title` seeded from the **folded** string, and the save path writes it back
with `setTitle(..., UTF8)`. **Opening `Ctrl+E` on the playing track and saving wrote the flattened
text into the file** - an untouched field could turn `café` into `cafe` on disk. Raw `TrackInfo`
fixes it. Found on the path, not looked for.

### Three sites needed column-safety work

Because *this change* made their strings multi-byte: `drawLyrics`'s header (byte `resize()` + narrow
API), `drawRecPanel`'s title (byte `substr` + `mvwprintw`), and the marquee trigger in `tickFrame`
(byte length compared against columns). All three now fold, measure in columns, and draw wide.

**The browser and playlist pane headers were NOT touched.** They were already raw and already
byte-truncating (`UIManager.cpp` `bar.resize((size_t)cols, ' ')`, and `panelFrame`'s title
`substr`). Pre-existing, reported in the recon, out of this slice's blast radius.

## 4. Astral / surrogate pairs — deliberately unaccommodated

**Emoji and other codepoints above U+FFFF no longer fold to `?`.** They are 4-byte, and length is
never a criterion, so they pass through.

- **Linux:** correct. `wchar_t` is 32-bit, one codepoint per cell, the emoji draws.
- **Windows:** `utf8_to_wide` produces UTF-16, so an astral codepoint becomes a **surrogate pair** -
  two `wchar_t`. PDCursesMod receives two lone surrogates. **What it draws is unknown and was not
  measured.** Previously this was unreachable, because the fold turned them into `?` first.

Dos ruled astral out of scope ("documented, accepted"), so **nothing was added to accommodate it**.
But the *appearance* changed from `?` to whatever two lone surrogates produce, and **if that is
worse than `?`, it is a Windows appearance regression introduced by this slice.** It is on Dos's
live-test list for exactly that reason. `docs/lessons.md` now records the real state; that note had
been readable as blessing the old `?` catch-all for all non-ASCII, which is part of how this lasted
four months.

## 5. Named and OUT — `LastFm::urlEncode`

`src/LastFm.cpp` gates on `std::isalnum(c)` for `unsigned char`, and `UIManager.cpp` calls
`setlocale(LC_ALL, "")`. **`isalnum` is locale-dependent for bytes ≥ 0x80.** Under a UTF-8 locale
every such byte is non-alnum and gets percent-encoded correctly. Under a legacy single-byte locale,
high bytes could test alnum and be emitted **raw into a signed query string**.

This was unreachable for local files *because the fold guaranteed ASCII before it got there*. **This
slice removed that accidental guard.** Radio already reached this code with 2-byte text, so the
exposure is pre-existing and merely widened.

The hardening is one line - `c < 0x80 && std::isalnum(c)`. **It is correct independently of this
work and Dos ruled it explicitly out of this slice.** Recorded here so it is not discovered later as
fallout and mistaken for a new bug.

## 6. Dos's live test — four things

1. **水田直志 renders in browser and playlist rows.** *Set `wingui_font` to a CJK font first* - see
   §7. Boxes with the default font are a missing-glyph result, not a fold failure.
2. **Columns line up.** CJK is two cells wide; the layout already knew that and was verified against
   the real helpers (a composed row measured exactly 40 columns from 50 bytes / 30 `wchar_t`).
3. **A Japanese-tagged file scrobbles its real title** to Last.fm / ListenBrainz, and the Windows
   media card and Discord show it.
4. **An existing Latin-1 library shows its accents** - `café`, `Müller`. This changes the look of
   the library on first run and is the correct text.

Plus the two that may come back as regressions: **emoji on Windows** (§4) and **non-ASCII filenames
from stream recording** (raw `now_playing_` → `sanitizePathComponent`, which passes bytes ≥ 0x80 -
matches what CD ripping has always done).

## 7. Environment notes

- **`ctest` needs `export PATH="/c/msys64/ucrt64/bin:$PATH"` in the same shell**, or ~22 tests fail
  with `0xc0000139`. PATH problem, never a regression. (Unchanged, still true, still costs time.)
- **The Windows font is chosen by RE-MOCT, not the terminal.** `wingui_font` in
  `%APPDATA%\RE-MOCT\remoct.conf`; relaunch to apply. PDCursesMod wingui calls
  `CreateFontIndirect` with the exact face name and draws with `ExtTextOutW`, so glyph coverage is
  whatever that one font has. JetBrains Mono ships Latin, Greek and Cyrillic - **no CJK**.
- Linux build used this session: `cmake -S . -B /tmp/blin -G Ninja -DCMAKE_BUILD_TYPE=Release`
  under WSL2 Trixie. No `/dev/sr*`, so no CD work there.

## 8. Gates

| | result |
|---|---|
| Windows UCRT64 build | **EXIT=0** |
| Windows ctest | **54/54** |
| Linux build | **EXIT=0** |
| Linux ctest | **55/55** |
| New compiler warnings | **zero**, both toolchains |
| `playlist_encoding_test` | 323 checks, 0 failures |

Every warning still emitted is on the `docs/warn-sweep-plan.md` inventory (line numbers have moved;
the code has not). **No assertion changed** - the design note predicted that and it held. The
additions are the CJK regression guard, one case per surviving normalize group, the seven
malformed-UTF-8 cases, and an idempotence check.

## 9. Version state

- **`Version.h` and `CMakeLists.txt` = 1.6.1.** CHANGELOG `## [1.6.1] - 2026-08-07`, dated.
- **`docs/index.html` deliberately still says 1.6.0.** It reconciles at ceremony, as it did last
  cycle. Bumping it now would announce a release that has not happened.
- **README** gained one bullet under the Windows font section - the CJK font requirement. Nothing in
  either doc described the old `?` fold, so nothing else needed correcting there.
- CHANGELOG link definitions are still missing for **1.3.1, 1.4.0, 1.4.1, 1.5.0, 1.6.0 and now
  1.6.1** (six). Sections all present, history complete. Ceremony decision, still open.
- Tag naming is still split: every release tag is bare except **`v1.5.0`**. Cosmetic, still open.

---

## 10. THE RESIZE CRASH — found and fixed the same night (added after §1-9)

**1.6.1 shipped a crash and it was found within hours: on Windows, dragging the window border with
CJK on screen aborted the process.** Reliable in seconds, three independent reproductions by Dos.
1.6.0 does not crash; 1.6.1 does. **It was this slice**, and the reason is structural: until the
fold stopped replacing CJK with `?`, `PDC_wcwidth` never returned 2, no double-width cell ever
existed, and the failing code was unreachable by construction.

**THE BUG IS IN VENDORED PDCursesMod. `lib/pdcursesmod/pdcurses/refresh.c` now carries a local
patch - see `VENDOR.md`, entry 4, and RE-APPLY IT AFTER ANY RE-PIN.** Upstream report is written at
`docs/upstream-pdcursesmod-chunk-boundary.md`; **not filed** - it posts publicly under Dos's name
and is his call.

### The mechanism, since it is not obvious

`PDC_transform_line_sliced` cuts a redraw span into `MAX_PACKET_LEN - 1` = **89**-cell chunks. A
double-width glyph occupies two cells - the character, then `DUMMY_CHAR_NEXT_TO_FULLWIDTH`. When a
chunk boundary falls between the two, the pair is split, and **both** ends of the split abort:

| face | assert | why |
|---|---|---|
| chunk *begins* on the dummy | `i > 1 \|\| ch != MAX_UNICODE` (`refresh.c`) | the leading-dummy repair sat ABOVE the loop, so it ran once - for the first chunk only |
| chunk *ends* on the dummy | `(srcp[len-1] & A_CHARTEXT) != MAX_UNICODE` (`wingui/pdcdisp.c`) | the trim read `ch`, which is STALE when the loop exits on `i == MAX_PACKET_LEN - 1` - that exit never evaluates `ch`, so `srcp[i-1]` is never examined |

Both fixed in one function. The cursor-split recursion at the top needs nothing: it re-enters the
same loop, so both repairs cover it. **Needs a window wider than 89 columns AND a wide glyph on the
boundary** - which is why several hundred automated ASCII-only resizes never reproduced it.

### Also changed, correct on its own terms

- **`onWinguiLiveResize` no longer draws inside the `WM_SIZE` handler.** It raises
  `live_resize_pending_`; `servicePendingResize()` does the rebuild from the modal paint tick (so
  the drag still reflows live) and from the run loop (so the final size is always correct). This did
  NOT fix the crash on its own - said so at the time rather than claiming partial credit - but
  drawing from inside a resize handler was wrong regardless, and it is what made the fault so easy
  to hit.
- **`handle_sigsegv` (`main.cpp`) rewritten.** It used to set a flag and RETURN, which resumes at
  the faulting instruction and loops forever - no crash record, ever. Now: async-signal-safe message
  to stderr, restore `SIG_DFL`, re-raise. It would not have caught this one (a fail-fast is not
  SIGSEGV) but it makes the next real memory fault reportable.

### Lessons that cost real time

1. **`PDC_doupdate()` is a message pump** (`PeekMessage`/`DispatchMessage`). `doupdate()` is not a
   leaf on this build. Anything drawn from a Windows message handler can be re-entered.
2. **`-DNDEBUG` is not set** - `CMAKE_BUILD_TYPE` is empty, so no build-type flags apply at all.
   That is why this aborts cleanly instead of drawing a garbled line, and it is the only reason the
   bug was findable. **Do not silence it.** Whether release packaging should set a build type is a
   separate, open question.
3. **A GUI-subsystem binary's stderr does not reach a redirect**, so the `fprintf` upstream put
   above its own assert never landed in the log. `gdb -batch -ex run -ex "bt 60"` is the way to get
   a trace; `catch-crash.bat` at the repo root does it in one double-click.
4. **Verify a PID is one you started before driving a window.** A launch that silently failed with
   exit 127 left an automation script driving Dos's own running instance - moving it, resizing it,
   sending it keystrokes. Check `Win32_Process.CommandLine` first.
5. **Do not claim a repro without checking state.** Several "survived 400 resizes" results were
   void: playback had never started, which a screenshot showed immediately and an assumption did
   not.

---

## 11. THE MOVE FREEZE — and the app-side window subclass (added after §10)

**Dragging the window to a new position froze the display for the whole drag.** Separate defect from
the crash, found the same night, fixed and confirmed on hardware.

**THERE IS NOW AN APP-SIDE SUBCLASS ON THE WINGUI WINDOW.** `remoctWndProc` in `UIManager.cpp`
(anonymous namespace) is installed over `PDC_hWnd` with `SetWindowLongPtrW(GWLP_WNDPROC)` next to the
two `PDC_set_*_callback` registrations. **It observes `WM_MOVING` and always forwards to the previous
proc - it never intercepts.** Anyone debugging window behaviour needs to know it is there.

**Why a subclass and not a third vendored callback** (Dos's ruling): every vendored line is carried
through every future merge, and this one comes straight out if upstream grows a real hook. The
`pdcscrn.c` callback would have been marginally cleaner engineering and permanently ours.

### The mechanism

`WM_TIMER` is synthesised only when the message queue drains. A held drag never lets it drain.
Measured inside ONE modal loop: a 344 ms burst of motion produced **zero** `WM_TIMER` against **44**
in the idle window immediately before it, and **235 `WM_MOVING`** in that same starved interval. So
the modal paint timer cannot carry a move; `WM_MOVING` is the move's equivalent of the `WM_SIZE` that
already drives a resize.

Both are needed and neither alone is enough: `WM_MOVING` covers motion, the timer covers a held-but-
stationary drag (44 ticks, zero `WM_MOVING`, measured).

### What `servicePendingMove()` does differently from its resize twin

- **`resizeWindows()` is unreachable from it, structurally.** A move has no geometry change, and a
  synthetic drag in recon produced 73 stray `WM_SIZE` during a MOVE - whatever causes that must
  never be able to turn a move into a relayout.
- **Throttled to 33 ms.** `WM_MOVING` is not a frame clock: **~680/s** measured. Painting on every
  one would have cost several times the freeze it fixes. This was not in the approved proposal and
  would have shipped a regression without it.
- **Animated subset, not `drawAll()`.** Browser + playlist are ~56% of a frame under playback and
  cannot change mid-drag. **Accepted, documented trade:** their ROW marquees freeze for the drag and
  a mid-drag track change does not repaint the now-playing highlight. Both resolve on release.

### NOT FIXED, and not ours — do not re-investigate

**Press the title bar and hold still: ~500 ms freeze before anything happens.** Confirmed cause:

```
00A1 WM_NCLBUTTONDOWN (HTCAPTION)
0112 WM_SYSCOMMAND wp=F012 (SC_MOVE|HTCAPTION)
   <- 500 ms, ZERO messages dispatched - the blink timer stops dead
0215 WM_CAPTURECHANGED / 0231 WM_ENTERSIZEMOVE   <- only now
```

Seven freezes, every one after `WM_SYSCOMMAND wp=F012`; gaps of 500/500/500/500/516/516/328 ms
against **`GetDoubleClickTime()` = 500** on this machine. `DefWindowProc` holds the caption press for
the double-click interval, dispatching **nothing** - so no app-side timer or message hook can paint
there either. **Option 2 (own the move loop: Aero Snap, multi-monitor edges, Escape-to-cancel,
keyboard move, snapping) was proposed and refused as a bad trade.** Accepted as Windows behaviour and
recorded in the CHANGELOG so it is documented rather than rediscovered.

Measured worst single freeze is **516 ms**, not the 1-2 s originally perceived - worth knowing before
anyone spends effort on it.

---

## 12. THE RESIZE FLICKER — the deferral removed again (added after §11, session close)

**1.6.1 shipped a visual regression against 1.6.0: resizing flickered the whole way through a drag.**
Found by Dos on hardware, fixed, confirmed smooth. Tip is `e3fcbd2`.

**The cause was the deferral written in §10** — the one that was kept "because drawing from inside a
`WM_SIZE` handler is wrong in principle." The principle holds. **It was never what aborted**, that was
said at the time, and it turned out to cost something real.

`HandleSize` sets `PDC_n_rows/cols` to the new size before calling us, but `COLS`, `LINES`, `curscr`,
`stdscr` and every `win_*` only change when `resize_term()` runs - which was inside the rebuild we had
postponed. So the client area was the new size while the entire curses state was the old one, and
**every `WM_PAINT` in that gap drew the old grid into the new window**: bare margin growing, clipped
shrinking. Then the tick corrected it. Stale, correct, stale = flicker.

**And the servicer was on a starved clock.** Measured on the resize path directly (not carried across
from the move): **zero `WM_TIMER` during a flooded modal size loop**, against 35 in the idle window of
that same loop, longest gap **532 ms** — over 11x the 47 ms interval. The stale window was never
bounded by the interval.

**`resizeWindows()`'s own comment already recorded the standard**: an intermediate frame during a live
resize drag is visible flicker on wingui, and one full repaint per tick is what the build was tuned
to. The deferral broke both halves and added a second full frame on any serviced tick.

### What the code looks like now

- **`onWinguiLiveResize()` is synchronous again**, behind `in_live_resize`, exactly as 1.6.0.
  `resizeWindows()` was byte-identical between versions, so the call site was the only difference.
- **`live_resize_pending_` and `servicePendingResize()` are deleted**, not dormant.
- **The MOVE deferral stays** and shares none of that machinery. A move has no size message to ride,
  which is the whole reason it needed one. `servicePendingMove()` is now the only deferred paint
  path in the build, and that is correct.

### The lesson worth keeping

**A fix kept on principle, for a fault it did not fix, is still a change that has to earn its place.**
The deferral was reported as not-the-fix when it was written; it should have come out the moment the
real cause was patched at source, instead of waiting for Dos to see flicker.

---

## 13. RELEASE READINESS — 1.6.1, and the three open ceremony decisions

**1.6.1 is UNRELEASED on `experimental/win-pdcurses`.** Not merged, not tagged, deliberately.

- `Version.h` and `CMakeLists.txt` = **1.6.1**. CHANGELOG `## [1.6.1] - 2026-08-07`, dated, four
  Fixed entries and two Changed.
- **`docs/index.html` still says 1.6.0 on purpose** - it reconciles at ceremony, as it did last
  cycle. It also does not yet describe any 1.6.1 feature. **That is ceremony work.**
- README carries the Windows CJK-font bullet.

**Three things still open, all cosmetic, all ceremony decisions:**

1. **CHANGELOG link definitions missing for six releases** - 1.3.1, 1.4.0, 1.4.1, 1.5.0, 1.6.0,
   1.6.1. Twelve sections, seven link lines. Add them or drop the convention deliberately.
2. **Tag naming is split**: every release tag is bare except **`v1.5.0`**. Adding link definitions
   mechanically would produce a wrong URL for that one.
3. **The CHANGELOG has still never been read end to end** - now ~45 entries across five campaigns.

**The ceremony itself** (from the workflow memory, as run for 1.3.0/1.3.1/1.5.0/1.6.0): finalise the
CHANGELOG date on experimental -> ff `dev` -> `git merge --no-ff dev -m "Merge dev into main: RE-MOCT
1.6.1"` -> push main -> annotated bare tag `1.6.1` on the main merge commit -> push the tag. **`main`
carries direct GitHub web edits not in `dev`, so it is never ff-able - always `--no-ff`.** Stash
untracked WIP before the main checkout. **Dos does the clean-box build and the GitHub release himself.**

## 14. STILL UNTRACKED — a standing decision, not an oversight

`docs/LOCKED-CODE.md` and `docs/CD-ADDRESSING-LOCKED.md` were committed this session (`7767867`).
Still untracked, unchanged, and **Dos's call, not to be committed unasked**:

- **13 session handoffs**, 07-18 through 07-27 - including the two covering the 1.5.0 and 1.6.0
  ceremonies. That trail is local-only.
- **Four not-greenlit notes**: `warn-sweep-plan.md`, `SCOPE-podcasts.md`, `RECON-gap-handling.md`,
  `RECON-playing-predicate.md`. Correctly untracked by convention.
- **Scratch that must never ride along**: `tools/src/cap*/`, `np_*/`, `*_heartbeat.log`, `*_out.txt`.

## 15. Session commits, in order

| commit | what |
|---|---|
| `29618c3` | the display fold + outbound raw + the tag-editor fix |
| `6a02060` | the CJK resize crash - vendored `refresh.c`, both faces |
| `3967055` | the move-drag freeze - `WM_MOVING` subclass, throttled subset draw |
| `7767867` | the two locked-policy docs into the repo |
| `e3fcbd2` | resize deferral removed, flicker regression fixed |

## 16. Process lessons from this session — the expensive ones

1. **Verify a PID is one you started before driving a window.** A launch failing silently with exit
   127 left automation driving Dos's own instance - moving it, resizing it, sending it keystrokes.
   Check `Win32_Process.CommandLine`.
2. **Do not claim a reproduction without checking state.** Several "survived 400 resizes" results
   were void because playback had never started. A screenshot showed it in seconds; an assumption
   never would have.
3. **Measure before proposing a mechanism.** Three theories were wrong before the backtrace: it was
   not a stack overrun, not the geometry assert, and not the `x == 0` case. Each cost a build.
4. **Stopping at the scope boundary was right every time** - and each stop was approved. Widening
   the approval unilaterally would have hidden the second face of the vendored defect.
5. **A GUI-subsystem binary's stderr does not reach a redirect.** `gdb -batch -ex run -ex "bt 60"` is
   the way; `catch-crash.bat` at the repo root does it in one double-click.
