# Session handoff - 2026-08-07 13:26

Branch `experimental/win-pdcurses`. **1.6.1 landed, no ceremony.**
Supersedes `docs/session-handoff-2026-07-27-1030.md` (which was written before the 1.5.0 and
1.6.0 ceremonies and is stale on its headline item).

---

## NEXT SESSION STARTS HERE

1. **Dos's live test of 1.6.1 is outstanding.** Four things to look at - §6. Two of them may come
   back as regressions and that is expected, not a surprise.
2. **Let it run a day or two before any release is considered.** Dos's call. No tag, no merge.
3. **On Windows, set `wingui_font` to a CJK-capable font before judging the Japanese rendering.**
   The bundled JetBrains Mono has no CJK glyphs. Boxes are a font result, not a fold result.

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
