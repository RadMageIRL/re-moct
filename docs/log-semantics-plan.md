# Log semantics: master vs derived marking — PLAN (log-semantics)

**Stage:** PLAN deliverable. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `8a36b4c`.

## 1. Discovery findings

1. **What the log records today, and where** (src/CDRipper.cpp): header
   block at :1420-1437 (Album/Artist/Year/Drive/Model/Tracks/Mode/Output),
   C2 block :1441-1448, per-track AR/CTDB diagnostics through the rip, and
   the `=== Summary ===` block at :2282-2295 (AR totals, album RG, one line
   per track). **The output FORMATS are never mentioned anywhere in the
   log** — no line records what was written or which outputs are lossless.
   So this slice ADDS a formats block + a summary verdict line; there is no
   existing marker to amend. `opt.formats` and the `kRipFormats[].lossless`
   property are both in scope at the two write sites — surfacing only, no
   new computation (the brief's premise confirmed).
2. **Recordings write no log** — confirmed; an R1 design decision
   ("recordings write no rip log in slice 1"). **Proposal: stay rip-only.**
   A recordings log is a new artifact class (naming, rotation, lifecycle)
   disproportionate to "surface what's known"; the recordings' derived-ness
   is already expressed where it matters — no AR/album fields in tags
   (absent because false) and the CHANGELOG's honest-limits text. If Dos
   wants one later it's its own small slice.
3. **The exact semantic** (factually precise wording — AR/CTDB verify the
   READ; lossless outputs RETAIN the verified bits, lossy ones transcode
   them away):
   - lossless (FLAC/WAV/WavPack): "lossless - verifiable master (retains
     the verified audio bit-for-bit)"
   - lossy (MP3/Opus): "lossy - derived copy (transcoded; verify against a
     lossless master, not this file)"
   - lossy-only rips state plainly in the Summary: "Master : NONE - lossy
     outputs only. The AR/CTDB verdicts above attest the disc READ, but no
     output retains the verified bits."
4. **No parser exists** — tree-wide grep finds no consumer of the rip log;
   it is human-only (the brief's discovery-4 gate: format is free). The
   cue/m3u8 master-target rule reads the `lossless` PROPERTY, not the log.

## 2. The marking (two write sites, log text only)

Header block (after `Output :`), one line per selected format in table
order:

    Formats: FLAC     lossless - verifiable master
             MP3      lossy - derived copy
    Master : FLAC (AR/CTDB verdicts apply to the lossless master;
             lossy copies derive from the same verified read)

…with the `Master :` line computed from the selection:
- >=1 lossless: names the lossless formats.
- lossy-only: "NONE - lossy outputs only" + the §1.3 sentence.
- (WAV counts as a master — untagged, but bit-preserving; the modal already
  annotates "(untagged)", the log need not repeat it.)

Summary block: one closing line restating the master verdict beside the AR
totals, so the two facts a future reader wants (did the read verify? which
file retains it?) sit together.

## 3. Non-goals honored / hazards

- Log-string emission ONLY: two fprintf sites in the existing worker, both
  with `opt` + `kRipFormats` already in scope. No AR/CTDB/tag/audio change;
  no Joan Osborne gate. If implementation needs anything beyond the two
  sites — stop and report (the brief's tripwire).
- No parser to break (§1.4). Factual for every shape (§1.3): lossless-only,
  lossy-only, and mixed rips each get a true statement.
- Recordings: inherit "derived, never verifiable" by NOT having a log to
  overclaim in — and the tag surface already says it (§1.2).

## 4. Verification (on greenlight)

- Three quick 1-track mode-[Y] rips on the GHD3N (local mode — the marking
  is mode-agnostic and [Y] skips the AR network wait): formats `f`
  (lossless-only), `m` (lossy-only), `fm` (mixed) — the log block + Master
  line correct in each shape; the lossy-only log plainly states no
  verifiable master.
- ctest both toolchains (should be untouched — no test compiles CDRipper's
  TU; green = no collateral).
- CHANGELOG: yes, user-facing (the log ships in every rip's `logs/` dir) —
  one short entry, hyphens only.

## 5. Divergence

None found — the brief's premises all held (properties exist, nothing new
computed, no parser). The one decision made: recordings stay log-less
(§1.2), recorded for Dos to override.
