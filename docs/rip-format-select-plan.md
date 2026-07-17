# Slice `rip-format-select` — format-selection modal + quality config (PLAN)

**Stage:** PLAN. No source written, nothing committed.
**Baseline:** `experimental/win-pdcurses` at `e071b0f` (post rip-encoder-seam).
**Position:** slice 2 of the rip overhaul — first user-visible slice. Builds
the LOCKED §3 modal design; no redesign proposed here.
**In scope per §0:** `rip_formats` + `flac_level` + `mp3` config keys; encoders
read config-fed values whose defaults equal today's literals exactly.

---

## 1. Divergence from the brief

Only one, minor: the brief's mockup reads "quality display-only (from
config)", and the current modal's readout is a **hardcoded string literal**
`"FLAC-5 + LAME V0 VBR"` at [UIManager.cpp:1674](src/UIManager.cpp#L1674) —
there is no config-driven text to replace, just the literal. Everything else
in the brief matches the tree. Notably:

- **Digits 1/2 are genuinely free in the modal** — the key handler's
  `default: return;` ([UIManager.cpp:4792](src/UIManager.cpp#L4792)) swallows
  them today. No collision, no rebinding.
- **The blocked-action idiom exists exactly as described** (§4.6 below).
- **Config is load-once** (one `config.load()` at `main.cpp:58`; the `~` key
  reloads theme.conf only) — so "seeded from config at start, session-only"
  is the natural grain of the existing system, not a new mechanism.

## 2. Resolved anchors (verified)

| what | where |
|---|---|
| Modal render | `UIManager::drawRipConfirm`, [src/UIManager.cpp:1625-1702](src/UIManager.cpp#L1625-L1702); `BOX_W 68 × BOX_H 15`, throwaway `newwin` per frame |
| Static readout to delete | [:1673-1676](src/UIManager.cpp#L1673-L1676) (`fmt_str` right-aligned on the Disc row) |
| Modal key handler | [:4780-4815](src/UIManager.cpp#L4780-L4815), gated on `UIOverlay::RipConfirm`; intercepts all input; `N`/Esc close at :4788-4791 |
| `CDRipper::start` call (the only one) | [:4808-4813](src/UIManager.cpp#L4808-L4813) |
| Modal open trigger | Ctrl+Y at [:5506-5528](src/UIManager.cpp#L5506-L5528) |
| Blocked-action hint idiom | predicate [:2924-2934](src/UIManager.cpp#L2924-L2934); inert+warn [:5157-5169](src/UIManager.cpp#L5157-L5169); render [:4180-4186](src/UIManager.cpp#L4180-L4186) (`warn_msg_`, `CP_STATUS_ERR` bold, ~5 s auto-expire at :1282-1286) |
| Session-transient state idiom | `mb_release_` (seed once, keep, reset on eject) — [include/UIManager.h:399-402](include/UIManager.h#L399-L402) |
| Encoder-list build (slice-1 seam) | [src/CDRipper.cpp:794-798](src/CDRipper.cpp#L794-L798) |
| Paired path sites to generalize | :1159-1160 (fail cleanup), :1454-1455 (path build), :1466 (ripTrack call), :1618-1619 (`.probe`), :1685-1686 + :1719-1722 (`.tmp` build/rename), :1755-1756 (`.det`), tagFile pass (:1898-1899 region) |
| FLAC literal to config-feed | [src/FlacEncoder.cpp:19](src/FlacEncoder.cpp#L19) (`FLAC_COMPRESSION = 5`) applied at [:26](src/FlacEncoder.cpp#L26) |
| LAME literals | [src/Mp3Encoder.cpp:22-24](src/Mp3Encoder.cpp#L22-L24) — `vbr_mtrh` + `VBR_q 0` + `quality 2`; only `VBR_q` becomes config-fed (§0: mtrh/q2 stay literals) |
| Config load/save patterns | string load `Config.cpp:200`, int load `:191` (`stoi` in try/catch), saves `:306`/`:315`; fields in `include/Config.h` |
| Seam oracle | `tests/rip_encoder_seam_test.cpp` — constructs `FlacEncoder()`/`Mp3Encoder()`; §5 keeps it compiling and pinning defaults |

## 3. Design

### 3.1 The shared row model — `include/RipFormats.h` (new)

One table drives the modal rows, the extensions, the master marker, and the
encoder factory, so slices 3-5 append a row + an encoder and touch no modal
logic:

```cpp
enum class RipFormat { Flac, Mp3 };

struct RipFormatRow {
    RipFormat   id;
    const char* label;      // "FLAC"
    const char* ext;        // ".flac"
    bool        lossless;   // drives the * master marker (§5 hazard: property, not row index)
};
inline constexpr RipFormatRow kRipFormats[] = {
    { RipFormat::Flac, "FLAC", ".flac", true  },
    { RipFormat::Mp3,  "MP3",  ".mp3",  false },
};
```

Digit key = table index + 1, so "(1-2 toggle)" scales to "(1-5 toggle)" by
appending rows. Quality text is composed in the UI from config (`"level %d"`,
`"LAME %s VBR"`); it is display-only and lives nowhere near the table.

### 3.2 Selection plumbing — recommendation: a `RipOptions` argument (option A)

Options considered:
- **(A) A `RipOptions` struct parameter on `CDRipper::start`** — explicit,
  copied into the worker like every other start argument, no mutable
  cross-thread state, and the single call site (:4808) makes the change
  trivial. **Recommended.**
- (B) A `setFormats()` member set before `start` — mutable state with a
  read-on-another-thread lifetime to reason about; worse for no gain.
- (C) Bare extra arguments (vector + two ints) — (A) with a worse signature;
  slices 4-5 would grow it again.

```cpp
struct RipOptions {
    std::vector<RipFormat> formats;      // ordered; default {Flac, Mp3}
    int flac_level = 5;                  // == today's literal
    int mp3_vbr_q  = 0;                  // V0 == today's literal
};
```

`start(..., RipMode, RipOptions, ProgressCb)` → `worker` → per track builds
an ordered `std::vector<std::pair<RipFormat, std::string>> outs`
(`prefix + ext` from the table — same strings as today), and `ripTrack`'s
`flac_path`/`mp3_path` parameters become that vector. Every paired site in §2
becomes a loop over `outs` (suffix append for `.tmp`/`.det`/`.probe`,
`fs::remove`/`rename` per entry, `tagFile` per entry — `tagFile` already
dispatches on path extension, zero change inside it). The seam build at
:794-798 becomes:

```cpp
for (auto& [fmt, path] : outs)
    encoders.push_back(makeEncoder(fmt, opt));   // Flac -> FlacEncoder(opt.flac_level), ...
```

A deselected format is **absent from `outs` entirely** — never instantiated,
no path built, no file touched, no `.tmp`/`.det` sibling, nothing for the
failure cleanup to remove (§5 subset hazard). Order is table order, so the
default set produces the identical FLAC-then-MP3 operation sequence.

### 3.3 Encoder config-feed (byte-identity per §0)

Constructor parameters with defaults equal to the literals they replace:

- `FlacEncoder(int level = 5)` — `set_compression_level(level_)` at the :26
  site; everything else untouched.
- `Mp3Encoder(int vbr_q = 0)` — `lame_set_VBR_q(lame_, vbr_q_)` at :23;
  `vbr_mtrh` and `quality 2` remain literals per §0.

**Default-identity argument:** the seam oracle constructs both encoders with
no arguments, so it compiles unchanged and now *pins the defaults* — if a
default ever drifts from the frozen inline reference arm, the oracle fails.
That converts §0's hard constraint into a machine-enforced invariant, the
same move as slice 1. Values are clamped at parse (level 0-8, V0-V9), never
in the encoder.

### 3.4 Config keys (patterns from `Config.cpp:191/200/306/315`)

`include/Config.h` gains:

```cpp
std::string rip_formats = "flac,mp3";  // default output set (seeds the session selection)
int         flac_level  = 5;           // FLAC compression 0-8
std::string mp3         = "V0";        // LAME VBR quality V0-V9
```

Load branches mirror the string/int idioms (stoi in try/catch; `flac_level`
clamped 0-8 on load; `mp3` validated `V0`-`V9` else default). Save writes all
three unconditionally (the `awesome_theme` pattern). Parsing `rip_formats`:
comma-split, case-insensitive token match against `kRipFormats[].label`,
unknown tokens ignored, empty result → full default set (absent config =
today's behavior, and a future config naming a slice-4 format degrades
gracefully on an older build).

**Session-only separation (§5 hazard):** `DigiConfig::rip_formats` is the
*default only*. `UIManager` copies it once into a new member
`std::vector<RipFormat> rip_sel_` at startup (constructor/init — config is
load-once, §1). The modal toggles `rip_sel_`; nothing ever writes it back to
`config_`, so the many existing `config_.save()` calls persist only the
unchanged default. Unlike `mb_release_`, `rip_sel_` is NOT reset on
eject/disc-change — it is a user preference for the session, not disc data.

### 3.5 Modal render (locked §3 layout)

`drawRipConfirm` changes: `BOX_H` 15 → **19**; the :1673-1676 static readout
is deleted. New layout (rows relative to the box):

```
 2  Drive  G\   offset +6 samples
 3  Disc   12 tracks
 4  <album, truncated>                    Out: FLAC + MP3     <- live summary, right-aligned
 5  (blank)
 6  Output formats                        (1-2 toggle)
 7    [x] 1  FLAC     level 5           *
 8    [x] 2  MP3      LAME V0 VBR
 9  (blank)
10  Select ripping mode
11-15  [A]/[C]/[Y]/[B]/[N] rows (unchanged strings)
16  divider
17  Out  <dir>
```

- Rows 7-8 loop `kRipFormats`: `[x]`/`[ ]` from `rip_sel_`, digit = index+1,
  label, quality text from config, `*` iff `row.lossless`. "(1-2 toggle)"
  renders `(1-%d toggle)` from the table size.
- The `Out:` summary joins selected labels in table order (`FLAC + MP3`).
- **Deselect-all state:** the summary slot shows
  `select at least one format to rip` in `CP_STATUS_ERR` bold (the
  `warn_msg_` colour), and the four mode rows render with `A_DIM`. Restored
  the instant a format is toggled on. The album text truncates to keep the
  right-aligned summary/hint intact (the existing `MAX_DIR`-style truncation
  at :1666-1669 is the idiom).

### 3.6 Modal keys (handler at :4780)

Rewritten switch, order making the disambiguation structural (§5 hazard):

1. `case 'n': case 'N': case 27:` — close, always allowed (unchanged).
2. `case '1' ... case '1'+N-1:` — toggle `rip_sel_` for that row,
   `redraw_needed_`, `return` (modal stays open). Digits can never reach the
   commit path because they `return` inside their own case.
3. `case 'a'/'c'/'y'/'b'` — **guarded**: `if (rip_sel_.empty()) { redraw;
   return; }` before any `RipMode` dispatch — commit is inert, not
   "commits with empty list"; the dimmed rows + hint (already rendered) tell
   the user why. Otherwise proceed exactly as today, now also building
   `RipOptions` from `rip_sel_` + `config_.flac_level` + parsed `config_.mp3`
   and passing it at :4808.
4. `default: return;` — unchanged.

A letter can never toggle (no letter case mutates `rip_sel_`); a digit can
never commit (digit cases return). Muscle memory: open-modal → `A` with the
default selection behaves byte-for-byte as today.

## 4. Hazard decisions (§5 of the brief)

| hazard | decision |
|---|---|
| Default-case byte-identity | Default `RipOptions` = table order {Flac,Mp3} + level 5 + V0 → identical operation sequence; encoder defaults pinned by the seam oracle (§3.3); confirmed on hardware by the trimmed gate (§5 below) |
| Subset correctness | Deselected format absent from `outs` — never instantiated, no path exists anywhere in ripTrack/worker (§3.2). The empty-`.flac`-on-LAME-failure quirk becomes "empty-*first-lossless*-on-*later-encoder*-failure" generically; in FLAC-only mode there is no later encoder, so it cannot misfire. Verified by the §5 subset checks |
| Digit/letter disambiguation | Structural: disjoint switch cases; digits return inside their case; letters guarded on non-empty (§3.6) |
| Selection lifetime | `rip_sel_` member, seeded once from `config_.rip_formats`, never written back (the only writers are the digit cases); not reset on eject by design (§3.4) |
| Master marker | `row.lossless` property in `kRipFormats` (§3.1) — no row index anywhere |

## 5. Trimmed verification (per the standing gate agreement)

1. **Default-case hardware gate:** [A] rip of **tracks 1-2 only** on the
   GHD3N with the default selection → both tracks AR v2 (the riprobe harness
   from slice 1 gains a `--tracks N` arg for this), outputs SHA256-identical
   to the slice-1 baseline files for those tracks (`preflight1/01-02.*` are
   retained on disk).
2. **Seam oracle** green both toolchains (now also pinning ctor defaults).
3. **Subset checks (no disc, or 1-track local rips):** FLAC-only → `.flac`
   present, no `.mp3`, no `.mp3.tmp/.det` ever created; MP3-only mirror;
   verified with a 1-track [Y] rip each (fast) + directory listing.
4. **Modal behavior:** toggle updates summary; deselect-all → dimmed modes +
   hint, `A` inert (no rip starts, no `start()` call), `N`/Esc still closes;
   re-select restores. Digit/letter no-collision exercised in the same pass.
5. **ctest** green both toolchains.
6. **CHANGELOG:** user-facing entry under `[1.3.0] - UNRELEASED`
   ("choose rip output formats in the rip dialog; FLAC level and MP3 VBR
   quality configurable"), hyphens only.
7. Byte-identity bar applies to the default case only; subsets are new
   behavior verified by presence/absence (gate note, per brief).

The modal behavior checks (4) are TUI-interactive — those are a Dos
eyes-on-screen pass unless you want a headless UI probe built, which I'd
argue is not worth it for one dialog.

## 6. Change inventory (on greenlight)

| file | change |
|---|---|
| `include/RipFormats.h` | **new** — enum + row table (§3.1) |
| `include/CDRipper.h` | `RipOptions`; `start`/`worker`/`ripTrack` signatures take it / the `outs` list |
| `src/CDRipper.cpp` | `outs` loops at the §2 paired sites; seam build via factory; worker path build from table |
| `include/FlacEncoder.h` / `src/FlacEncoder.cpp` | `int level = 5` ctor param |
| `include/Mp3Encoder.h` / `src/Mp3Encoder.cpp` | `int vbr_q = 0` ctor param |
| `include/Config.h` / `src/Config.cpp` | `rip_formats` / `flac_level` / `mp3` fields + load/save |
| `include/UIManager.h` | `rip_sel_` member |
| `src/UIManager.cpp` | `drawRipConfirm` (BOX_H 19, rows, summary/hint, dim), key handler (§3.6), `RipOptions` at the start call, seed `rip_sel_` |
| `tests/rip_encoder_seam_test.cpp` | unchanged (compiles as-is; now pins defaults) — possibly +2 asserts constructing with explicit 5/0 to document the pin |
| `CHANGELOG.md` | user-facing 1.3.0 entry |
| `docs/lessons.md` | one entry: session-selection vs config-default separation |

No `ar_crc.*`, no read/verify logic, no new deps, no CI changes.
