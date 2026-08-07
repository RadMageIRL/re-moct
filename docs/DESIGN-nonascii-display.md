# DESIGN: non-ASCII display — the `sanitizeForDisplay` contract

> This proposal does not touch AR_PREGAP, `ar_crc.*`, or the read addressing.

**Status:** design proposal. Not greenlit. No code. UNTRACKED until a slice is greenlit.
**Tree:** `experimental/win-pdcurses` at `457e12d` (1.6.0).
**Recon of record:** `docs/RECON-nonascii-render.md`.

**Goal:** 水田直志 renders as itself everywhere the fold is on the path.

---

# 0. ONE THING THE BRIEF ASSUMES THAT IS NOT TRUE

**The brief says "Identity paths already take the raw string, so this should be display-only —
confirm it." I cannot confirm it. It is not display-only.**

For **local files** and **radio**, the folded string is what RE-MOCT sends to Last.fm,
ListenBrainz, the OS media card and Discord Rich Presence:

```
LocalFileSource.cpp:44-45   info.title/artist = sanitizeForDisplay(tag->...to8Bit(true))
  → AudioManager.cpp:232    current_track_ = file_src_->info()
  → UIManager.cpp:6057-6058 artist = t.artist; track = t.title;   ← the scrobble tick
  → scrobbler (Last.fm + ListenBrainz) AND publishMedia (SMTC + Discord RP)

StreamSource.cpp:1133, 1314  now_playing_ = sanitizeForDisplay(combined)
  → UIManager.cpp:5988-5990  artist/track split out of `np`   ← same tick, same consumers
```

**A Japanese-tagged local file scrobbles `???? - ????` to Last.fm today.** That is a permanent
record on an external service, not a pixel.

**The CD path is already clean, and says why in the code** — `UIManager.cpp:5991-5993`:

> *CD: pull raw UTF-8 artist/title from the cached MusicBrainz release. (The playlist's
> display_title is combined "Artist - Title" AND ASCII-sanitized for the terminal, so it's
> unsuitable for scrobbling.)*

So the tree already contains the judgement that the fold is unsuitable for outbound data, and
already routes one of three sources around it. The other two were never revisited.

**This strengthens the case rather than complicating it** — fixing the fold fixes the outbound data
for free, and the CD special-case stays correct either way. But it must be stated plainly: this
change corrects data that leaves the machine. It is not confined to the terminal.

Verified clean and unaffected: rip filenames, cue sheets, `.m3u`, tags written by `CDRipper` (all
read the raw `MBRelease` — `CDRipper.cpp:173-176, 753, 1763, 2428-2430`), podcast cache filenames
(`pathSafeAscii(sanitizePathComponent(ep.title))` — raw title, `UIManager.cpp:11384-11385`),
`LibraryIndex` records (raw by contract), and `mb_album_` (display-only, `UIManager.cpp:2853-2854`).

---

# 1. THE NEW CONTRACT

**Rule.** Given a UTF-8 string, `sanitizeForDisplay` emits, for each codepoint in order:

1. **Reject** — if the bytes are not a well-formed UTF-8 sequence: one `'?'`, and advance one byte.
2. **Normalize** — else if the codepoint is in the NORMALIZE table (§2): its ASCII replacement,
   which may be the empty string.
3. **Pass through** — else: the codepoint's own bytes, verbatim.

**Nothing else is rejected. A codepoint's byte length is never a criterion.**

That last sentence is the whole change. The current rule's step 3 reads *"else if the sequence is
two bytes, verbatim; otherwise `'?'`"* — length decides fate, which is why 水 (3 bytes) dies and ö
(2 bytes) lives.

**Checking a string against the rule:** every character either appears in the §2 table, is an
ASCII byte, or comes out exactly as it went in. If a character that is none of those is missing
from the output, the implementation is wrong.

**Name.** The name stops describing the function once it mostly passes text through. Suggest
`foldForDisplay` — "fold" is the vocabulary the codebase already uses for this
(`StringUtils.h:222`, `LibraryIndex.h:101`), and a rename makes every one of the ~95 call sites
fail to compile, so none is missed. **This is a suggestion, not part of the contract** — the
behaviour change stands or falls on its own, and a rename is a diff-size decision that is Dos's.

---

# 2. Constraint 1 — every current normalization, enumerated

From `include/StringUtils.h:169-211`. Eight groups. **Seven stay unchanged. One is proposed for
removal and is severable from the goal.**

| # | group | codepoints | → | verdict |
|---|---|---|---|---|
| A | single quotes / prime | U+2018 U+2019 U+201A U+201B U+2032 | `'` | **KEEP** |
| B | double quotes / double prime | U+201C U+201D U+201E U+201F U+2033 | `"` | **KEEP** |
| C | dashes & hyphens | U+2010 U+2011 U+2012 U+2013 U+2014 U+2015 U+2212 U+00AD | `-` | **KEEP** |
| D | ellipsis / bullet | U+2026 → `.` ; U+2022 → `*` | | **KEEP** |
| E | space variants | U+00A0 U+2002 U+2003 U+2009 U+200A U+202F U+205F U+3000 | ` ` | **KEEP** |
| F | zero-width / BOM | U+200B U+200C U+200D U+FEFF | *(dropped)* | **KEEP** |
| G | ligatures | U+0153→`oe` U+0152→`OE` U+00E6→`ae` U+00C6→`AE` | | **KEEP** |
| H | ASCII fast path | U+0000–U+007F | verbatim | **KEEP** (incl. control bytes — pinned) |
| I | malformed UTF-8 | invalid lead / bad continuation | `?` | **KEEP** |
| **J** | **accented Latin letters** | **17 codepoints, below** | **stripped to bare ASCII** | **PROPOSE REMOVE** |

**Why A–I stay:** each is a *typographic* equivalence — a character that exists to look right in
print, mapped to the ASCII the terminal can show, with no loss of linguistic content. `Don’t` →
`Don't` is the same word. `a—b` → `a-b` is the same words. That is a coherent rule and it is the
one worth keeping: **fold typography, keep language.** Group E folding U+3000 (ideographic space)
to a space is consistent with that and stays — it is punctuation, not a letter.

## Group J — the one I propose removing

`U+00E9 U+00E8 U+00EA U+00EB → e` · `U+00E0 U+00E2 → a` · `U+00E7 → c` · `U+00EE U+00EF → i` ·
`U+00F4 U+00F8 → o` · `U+00FC U+00FB U+00F9 → u` · `U+00C9 U+00C8 → E` · `U+00C0 → A`

**These are not typography. They are letters, and the list is incomplete, so the result is already
inconsistent on screen today.** Measured, current build:

| input | current output | |
|---|---|---|
| `café` | `cafe` | é is in the list |
| `Björk` | `Björk` | ö is not |
| `Müller` | `Muller` | ü is in the list |
| `España` | `España` | ñ is not |
| `Ångström` | `Ångström` | å and ö are not |
| `Dvořák` | `Dvořák` | ř and á are not |

Two German names in the same pane, one keeps its umlaut and one loses it. That is not a design
decision anyone made; it is a list that stopped. Group J is also the only group whose members would
survive the default anyway — they are 2-byte, so the explicit cases exist purely to strip them,
which is what the pre-wide-API draw path needed and no longer does.

**Group J is severable.** Removing it is not required for 水田直志 — the goal is met with A–I
unchanged. Keeping J costs only the inconsistency it already has. **This is the one item that
needs a ruling** (§6, Q1), because removing it changes visible output for Latin-1 libraries, which
is most of the existing user base, in a direction they did not ask for.

**Recommendation: remove.** The whole point of the change is that characters render as themselves,
and a rule that keeps *some* diacritics is harder to explain than one that keeps all of them or
none. But the goal does not depend on it and the risk is entirely Dos's to price.

---

# 3. Constraint 3 — why it was written this way

**Nothing in the code, the comments or the history defends the `'?'` catch-all as a deliberate
permanent limit. The only place in the tree that judges it, judges it wrong.**

## History

The `'?'` default is present in **`b17bc0f` (2026-06-18)** — the earliest of the bulk
`Add files via upload` commits, i.e. it predates the repo's real per-change history. `git log -S`
finds no later commit that introduced or revised it.

| | commit | date |
|---|---|---|
| the `'?'` fold | `b17bc0f` | **2026-06-18** |
| `utf8_to_wide` (wide draw path) | `6cb4544` | 2026-06-19 |
| `cpWidth`/`dispWidth` (column pipeline) | `341719f` | 2026-06-28 |

**The fold predates the wide draw path by one day and the column-aware pipeline by ten.** When it
was written the narrow draw functions could not decode UTF-8 at all, so a non-ASCII glyph rendered
as mojibake and `'?'` was the *correct*, more honest output. **The reason was real. It stopped
holding on 2026-06-19 and nothing has revisited the function since.**

## What the tree says about it

Three written statements, none a defence:

- **`StringUtils.h:222-223`** — *"They do NOT fold to ASCII — sanitizeForDisplay is the lossy
  fallback; these preserve the codepoints."* Describes a split between two helper families. It
  records that the fold is lossy; it does not argue the loss is wanted.
- **`lessons.md:13-14`** — *"Astral/emoji (surrogate pairs, Windows 16-bit wchar_t) may not render;
  folded to '?' - acceptable, no regression."* This is about **astral-plane** codepoints and the
  UTF-16 surrogate-pair limit, which the brief explicitly rules out of scope and which this
  proposal does not change. **It is not a statement about BMP text**, and 水 is BMP. It has been
  read as broader than it is.
- **`UIManager.cpp:5991-5993`** — the CD scrobble path, quoted in §0, calls the fold
  *"unsuitable"* and routes around it.

**Verdict: stopgap, not a deliberate limit.** The one judgement in the tree is against it, and the
condition that justified it was gone within a day of it being written.

---

# 4. Constraint 2 — the test list

**Exhaustive search:** `tests/playlist_encoding_test.cpp` is the *only* test file that references
`sanitizeForDisplay`, `displayTitleFor` or `cleanChapterTitle`. Every other non-ASCII fixture in
`tests/` (`art_embed_test.cpp:205-207` incl. 東京, `library_index_test.cpp:79, 588, 651, 1085-1094`,
`library_scanner_test.cpp:121, 140`, `md5_test.cpp:73-74`) exercises **raw** paths and tag text that
never touch the fold, so none is affected.

## Assertions that CHANGE

**None.** Under the proposed contract every existing assertion passes unmodified.

## Assertions that STAY, and why they still pass

| line | assertion | why it survives |
|---|---|---|
| `:81` | `displayTitleFor(…, "Don\xE2\x80\x99t") == "Don't"` | U+2019 is Group A — still normalized. **This is the assertion the brief named; it is unaffected.** |
| `:91` | ASCII control byte passes through verbatim | Group H, ASCII fast path — untouched by this change |
| `:66-75` | the six `displayTitleFor` shape cases (artist/title/stem) | ASCII only |
| `:96-108` | `isValidUtf8` — 13 cases | different function |
| `:113-127` | `ensure_utf8` / `cp1252_to_utf8` — 8 cases | different functions |
| `:146-198` | legacy `.m3u`/`.pls`/`.xspf` loader cases | different path |

If Group J is removed (§2), still no assertion changes — no test covers an accented Latin letter
through `displayTitleFor`.

## Comments that MUST change (not assertions, but they will be wrong)

1. **`tests/playlist_encoding_test.cpp:76-80`** — *"a smart quote comes back as ASCII whichever
   caller supplied it"* stays true, but the framing *"the folding has to happen here"* should say
   what is folded now.
2. **`tests/playlist_encoding_test.cpp:84-89`** — *"sanitizeForDisplay folds NON-ASCII and passes
   every byte below 0x80 through verbatim"* becomes **false** on the first clause. This is the
   "honest limit" text the brief flagged. It must be rewritten to state the new limit: the control
   byte survives because it is ASCII, not because non-ASCII is folded.
3. **`include/LibraryIndex.h:101-103`** — *"Tag text is stored RAW. sanitizeForDisplay runs at draw
   time… folding on the way in would be lossy, and it would also be insufficient (passes ASCII
   control bytes straight through)."* **The contract this describes is unchanged and still correct**
   — the fold still runs at draw time, it just folds less. Worth a clause noting it no longer folds
   language, so nobody re-derives that the index should fold on the way in.
4. **`include/PodcastChapters.h:71-83`** — *"the Unicode punctuation sanitizeForDisplay exists to
   fold"* becomes exactly right rather than an understatement; the composition
   (controls→spaces, then fold, then collapse, then UTF-8-safe byte truncate) is **unchanged and
   still correct**. See §5.3 for the one behavioural consequence.

## Tests I propose ADDING

Not required by the contract, but this is the failure mode that went four months unnoticed:

- `sanitizeForDisplay("水田直志") == "水田直志"` — the headline case, and the regression guard.
- One assertion per surviving group (A–G) pinning the replacement, so the table in §2 is executable
  rather than prose.
- `sanitizeForDisplay("caf\xE9") == "caf?"` — malformed UTF-8 still rejects (Group I), which is the
  case most at risk of being lost when the catch-all is narrowed.

---

# 5. Call-site impact

**~95 sites, one function, one behaviour. No site wants the old behaviour.** The change is at the
definition; no call site is edited (unless the rename in §1 is taken, which edits all of them
mechanically).

## 5.1 Sites that get better and want nothing else

All browser rows (`UIManager.cpp:10028` and the ~30 section populators), all playlist rows
(`PlaylistManager.cpp:81-82`), CD rows (`UIManager.cpp:2527, 2535`), the info pane
(`:5114-5119, 5195, 5206`), status/toast text, the library levels, podcasts, radio. These are the
goal.

## 5.2 Sites where the improvement leaves the terminal — the §0 set

Last.fm, ListenBrainz, SMTC and Discord RP begin receiving real text for local files and radio.
Verified byte-safe end to end:

- **Discord** — `DiscordRP::jsonEscape` (`src/DiscordRP.cpp:81-95`) escapes the five JSON
  metacharacters and `\u`-escapes bytes `< 0x20`, passing every byte `≥ 0x20` through. Raw UTF-8 is
  legal in a JSON string. **Correct as written.**
- **Last.fm signing** — MD5 over UTF-8 is already gated: `tests/md5_test.cpp:73-74` signs
  `Björk`/`Jóga`/`ß`/`ç`. **Proven, not assumed.**
- **SMTC** — `MediaControlSmtc.cpp:43-45` widens with `MultiByteToWideChar(CP_UTF8, …)`. Correct.
- **notify-send** — argv is bytes, `NotifyArgv.h` is byte-transparent. Correct.
- **Last.fm URL encoding** — see §5.4, the one genuine finding.

## 5.3 Sites where the composition changes slightly

- **`PodcastChapters::cleanChapterTitle`** (`include/PodcastChapters.h:83`) — `kMaxTitleBytes` is a
  **byte** cap. Text that used to fold to one byte per character now costs up to three, so a
  Japanese chapter title fits roughly a third as many characters. **No corruption**: the truncation
  already backs off to a codepoint boundary (`:86-88`). Shorter titles, correctly formed. Whether
  the cap should become columns is a separate question and is **not proposed here**.
- **Browser/playlist search** (`UIManager.cpp:9042-9045`, `:7279`) matches against `dir_display_`
  and `display_title`, so search starts matching real text instead of `????`. Improvement. The
  validate-on-use comparison at `:7279` compares a display string to a display snapshot and is
  consistent either way.
- **Empty-row disambiguation** (`UIManager.cpp:10781-10782`, `:9440`) compares `dir_display_`
  against the ASCII literals `"(no artist)"` / `"(no album)"`. Unaffected.

## 5.4 The one thing that becomes reachable

**`LastFm::urlEncode` (`src/LastFm.cpp:61-70`) gates on `std::isalnum(c)` for `unsigned char`, and
`UIManager.cpp:308` calls `setlocale(LC_ALL, "")`.** `isalnum` is locale-dependent for bytes
`≥ 0x80`. Under a UTF-8 locale every such byte is non-alnum and gets percent-encoded correctly.
Under a legacy single-byte locale, high bytes could test alnum and be emitted **raw** into a signed
query string.

**Unreachable today for local files** — the fold guarantees the string is ASCII before it gets
there. Removing the fold removes that accidental guard. The hardening is one line (`c < 0x80 &&
std::isalnum(c)`), it is correct independently of this proposal, and **it is not proposed here** —
it is named so it is not discovered later as fallout. Radio already reaches this code with 2-byte
text today, so the exposure is pre-existing and merely widened.

---

# 6. Decisions for Dos

**Q1 — Group J (the 17 accented Latin letters): remove, or keep?**
Removing makes the rule "fold typography, keep language" true without exception and ends the
`Müller`/`Ångström` inconsistency; it also changes visible output for existing Latin-1 libraries.
Keeping meets the 水田直志 goal just as fully and changes nothing anyone has already seen.
**Severable either way. My recommendation is remove; the goal does not depend on it.**

**Q2 — rename `sanitizeForDisplay` → `foldForDisplay`?**
Buys a compile-time sweep of all ~95 sites (nothing missed) and a name that describes the new
behaviour. Costs a large mechanical diff. **Behaviour change is independent of this.**

**Q3 — is correcting the outbound data (§0) in scope for this slice, or does it want to be named
separately?** It happens automatically when the fold is fixed; it cannot be opted out of without
adding a second fold at the scrobble tick, which would be worse. Flagging it as a scope question
rather than assuming.

---

# Noticed in passing

- The two header paths still measure columns in bytes — `UIManager.cpp:3121`
  (`bar.resize((size_t)cols, ' ')`) and `:974` (`t.substr(0, cols - tx - 2)`) — and a CJK folder
  name is exactly what would reach them; pre-existing, reported in the recon, unchanged by this.
- `UIManager.cpp:961-962`'s comment ("the narrow path on this build doesn't decode UTF-8") is stale
  under `PDC_FORCE_UTF8`; the wide-API code it justifies is correct regardless.
