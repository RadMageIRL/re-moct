# Batch ReplayGain over a folder — PLAN (batch-r128)

**Stage:** PLAN deliverable. No source, no commit.
**Baseline probed:** `experimental/win-pdcurses` @ `c62ca85`.

## 1. Discovery findings (§2) — every piece exists; this slice is wiring

1. **The per-track libebur128 computation is already isolated** — twice.
   The rip's per-track piece (CDRipper: `ebur128_init(2, 44100,
   EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)` → `add_frames_float` →
   `loudness_global` → `gain = -18.0 - loudness`, peak = max true-peak
   L/R) was ALREADY extracted from the album pass once, by StreamRecorder's
   rollCut RG block — including the finite/>-70 LUFS guard. The batch path
   is that same ~20-line shape a third time; the math is identical by
   construction, which is what makes the parity proof (§5) meaningful.
2. **The dialect-correct tag writes have a precedent** — the recorder's
   tagCut (post mp3-rg-write): MP3 = proper `UserTextIdentificationFrame`
   REPLAYGAIN_TRACK_GAIN/PEAK; Opus = `R128_TRACK_GAIN` via R128Gain.h;
   FLAC = Xiph REPLAYGAIN_* (tagFile's shape); WavPack = APEv2 REPLAYGAIN_*
   (tagFile's shape). The batch pass is a small dedicated writer reusing
   those four shapes — NOT tagFile (MB/AR-entangled), the recorder
   precedent exactly. **WAV answered from the table**: `kRipFormats`'s WAV
   row is `taggable=false "(untagged)"` — batch skips `.wav`
   with-note, consistent with the rip.
3. **Decode-for-measurement has an in-tree twin**: BPM detection
   (`startBpmDetection`) already full-decodes an arbitrary file on a worker
   thread with a cancel flag — the exact pattern. The batch worker opens
   its own `LocalFileSource` per file and drains readFrames start-to-end.
   Note: LocalFileSource delivers the playback contract (44100/stereo
   f32), so measurement happens at playback rate — the same reference the
   player applies gain against, and rate-identical to the rip's
   measurements for CD-sourced files.
4. **The skip predicate** reads `tag->properties()["REPLAYGAIN_TRACK_GAIN"]`
   (FLAC/MP3/WavPack — PropertyMap spans Xiph/ID3v2/APEv2) and
   `R128_TRACK_GAIN` for Opus — both dialects, mirroring the player's own
   read path. **A healing side-effect, flagged:** legacy blob-shape MP3s
   (pre mp3-rg-write) are PropertyMap-invisible, so the default scan will
   treat them as untagged and re-tag them PROPERLY — the batch write
   removes any legacy REPLAYGAIN_* blob TXXX frames before adding the
   standard ones (no duplicate-frame cruft). One batch run heals the
   pre-fix library.
5. **Key**: `^O` / `^V` / `^W` / `^X` all still free (re-scanned against
   both binding patterns post-^E). **Propose `^O`** ("nOrmalize" is the
   least-bad mnemonic left in the map; re-verified at implement per the
   discipline). Force lives in the SAME key via a one-line prompt (§3.2).
6. **Extension gate**: `PlaylistManager::isSupportedAudio` + `AUDIO_EXTS`
   (src/PlaylistManager.cpp:32) — the scan enumerates the selected
   folder's entries through it, then narrows to the TAGGABLE set.
   **Non-recursive v1** (the selected folder only); recursion is a listed
   later option.
7. **Progress idiom**: the rip's status-line + toast pattern
   (`rip_status_` / `showTrackToast`) — a browser-context status line
   ("ReplayGain: 12/87  Artist - Title…") polled by the tick, and the
   summary toast at the end.

## 2. The shape (the recorder precedent, again)

New `include/GainScan.h` + `src/GainScan.cpp` — an engine class, NOT
UIManager logic (headless-testable, honoring the linkage norms):

    class GainScan {
        bool start(dir, bool force);   // enumerates, spawns the worker
        void cancel();                 // honored BETWEEN files
        // atomics: current index/total, tagged/skipped/errors, running
        // current-file string under a small mutex
    };

Per-file pipeline on the worker: extension+taggable gate → skip predicate
(unless force) → LocalFileSource full decode feeding ebur128 → the guarded
gain/peak → the dialect writer → counters. One bad file = one error count,
never an abort. UIManager: `^O` opens the one-line prompt ("ReplayGain scan
'<folder>': [Enter] tag untagged  [F] force re-tag  [Esc]"), wires
start/cancel, shows the status line, toasts the summary
("N tagged, M already had gain, K errors, W wav skipped").

**Formats v1**: FLAC, MP3, Opus, WavPack (the rip-taggable four). Proposed
IN as a fifth: `.m4a`/`.m4b` via TagLib's MP4 PropertyMap (the freeform
REPLAYGAIN atoms — the player's read path already consults properties();
the audiobook library benefits). Cheap; Dos strikes it if unwanted.

## 3. Design answers (§3)

1. **Parity by construction**: same init flags, same add-frames feed, same
   `-18.0 - loudness`, same guard, same formatters (`%.2f dB` / `%.6f`),
   same R128Gain.h conversion — the numbers cannot diverge from the rip's,
   and §5 proves it against the retained artifacts anyway.
2. **Skip/force surface**: one key + the prompt (Enter/F/Esc) — cleaner
   than two keys in an exhausted key map, and the prompt names the folder
   (no "which folder did that just chew?" surprises).
3. **Worker + atomicity, stated honestly**: cancel is checked BETWEEN
   files — a cancelled scan never leaves a mid-write file (each file is a
   complete unit), and everything already tagged stays tagged. Within a
   file, TagLib::save() writes in place (not temp+rename) — a hard crash
   mid-save risks that one file's tag region, which is EXACTLY the
   exposure every existing tag write in the app has (rip tagFile, recorder
   tagCut). Batch matches the app's established write-safety envelope; it
   does not degrade it, and cancel/interrupt-by-flag is always clean. (A
   copy-tag-rename scheme would be new surface and GB-scale I/O per file —
   disproportionate; rejected.)
4. **Error handling**: per-file try/skip + error counter; summary toast +
   status line; errors also logged (slog) with paths.
5. **Peak**: written wherever the rip writes it (REPLAYGAIN_TRACK_PEAK on
   FLAC/MP3/WavPack); Opus carries none (R128 defines none) — consistency
   with rip output, exactly.
6. **Cancellation**: worker checks the flag between files; `^O` while
   running offers cancel; every stop path leaves a valid library.
7. **Runtime expectation, stated**: decode-bound — roughly 2-5 s per
   4-minute track; thousands of files = hours on first run. The
   skip-default makes re-runs proportional to NEW files only; progress +
   cancel keep the long case honest.

## 4. Hazards (§5) — answers

Parity: §3.1 + §5. Atomicity: §3.3 (cancel-clean guaranteed; crash
exposure = the app's existing envelope). Skip reads both dialects: §1.4.
Error isolation: §3.4. Audio thread: never touched — worker decode via a
private LocalFileSource, no device, no callback. Large library: §3.7.

## 5. Verification (on greenlight)

- **Gain parity (machine, the correctness proof)**: new `gain_scan_test` —
  copy retained artifacts (subF/01.flac -6.92, subO/01.opus -6.92, a
  recorder MP3), STRIP their gain tags, run the engine → re-tagged values
  equal the originals within rounding, both dialects; read back through
  LocalFileSource (the player's path).
- **Skip/force/mixed**: tagged folder → all skipped (fast); force →
  re-tagged; mixed → only untagged touched. Legacy-blob MP3 → healed to
  the standard shape, blob frames removed.
- **Error isolation**: a truncated/garbage file in the folder → error
  count, scan completes.
- **Cancel**: flag mid-scan → completed files tagged, no partial file.
- **Audio invariance**: tag-strip SHA unchanged on a tagged file (the
  id3strip method for MP3; FLAC audio MD5 for FLAC).
- **WAV**: present in folder → skipped-with-note.
- Live (Dos): `^O` over a real folder — progress visible, TUI responsive,
  summary toast; RG audibly applies on playback of a batch-tagged file.
- ctest both toolchains; CHANGELOG (batch ReplayGain scan; hyphens; the
  legacy-MP3 healing note).

## 6. Divergences

1. `.m4a` proposed IN beyond the brief's rip-format list (§2 formats) —
   cheap via PropertyMap, strikeable.
2. The legacy-blob MP3 healing side-effect (§1.4) — a feature, but it
   means the first default run over an old library re-tags MP3s the user
   may believe are "already tagged"; the CHANGELOG says so.
3. Atomicity honesty (§3.3): cancel-clean is guaranteed; crash-during-save
   matches the app's existing envelope rather than exceeding it.
