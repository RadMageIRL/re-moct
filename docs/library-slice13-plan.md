# DESIGN NOTE - Library slice 13: `Config` stat-key normalisation

**Scope ID:** LIB-S13. **Status:** BUILT. Greenlit with no changes requested.
**§8 holds the results, the two confirmations Dos asked for, and a false pass that nearly slipped
through.**
**Probed against tip `f127f78`** on 2026-07-26, and **re-measured against Dos's LIVE
`remoct.conf` as written at 14:04 today**, not against LIB-S10's numbers.

**This slice rewrites persisted user data. §4 is the rollback story and it is part of the design,
not an appendix.**

---

## 0. RE-MEASURED - the numbers hold, and LIB-S11 changed the picture

Every figure the brief carries is confirmed against the live file:

| | LIB-S10 said | measured now |
|---|---|---|
| stat entries | 295 | **295** |
| lowercase drive letter | 241 | **241** |
| uppercase drive letter | 54 | **54** |
| files with two case-variants | 17 | **17** |
| `One of Us` | 73 + 22 = 95 | **22 + 73 = 95** |

**And the arithmetic that matters for a merge is verified rather than assumed:**

```
SUM of counts before merge : 3901
SUM of counts after  merge : 3901
distinct files after folding: 278   (295 - 17)
```

**A summing merge loses nothing.** That is the whole correctness claim of this slice and it is now a
measured fact about Dos's actual data, not a property of the algorithm in the abstract.

All 17 pairs, by name, so the gate can check any of them:

```
beck - devils haircut          18+30=48    vampire weekend - a-punk        56+31=87
vampire weekend - oxford comma  1+36=37    4 non blondes - what's up       27+37=64
billy bragg - california stars 30+12=42    .38 special - hold on loosely   37+ 1=38
jackson 5 - rockin' robin      34+ 2=36    cyndi lauper - girls just want  34+21=55
joan osborne - one of us       22+73=95    vampire weekend - prep-school   18+76=94
beck - loser                    3+29=32    jackson 5 - abc                  2+49=51
arrested development - wendal  30+ 9=39    cornershop - brimful of asha    32+17=49
beck - where it's at           28+15=43    crystals - da doo ron ron       51+ 7=58
arrested development - people  23+27=50
```

`.38 Special - Hold on Loosely` is on that list, which is a small pleasure: it is the track that was
stuck in the info pane in the LIB-S10 bug report.

### LIB-S11 has landed on Dos's machine, and it changed what "orphaned" means

His live config now carries **two `library_root=` lines** - `C:\Users\david\Music` and `D:\Music`.
So `@` works and persisted, and the 59 keys LIB-S10 measured as matching nothing are now:

| where the 295 keys point | count |
|---|---|
| under `C:\Users\david\Music` | 244 |
| under `D:\Music` (in scope only since LIB-S11) | **46** |
| under no configured root | **5** |

**The 5 are all in `C:\Users\david\smoke\files\` - the test harness directory - and ALL FIVE STILL
EXIST ON DISK.** I checked rather than assumed.

**So the brief's question answers itself: nothing is dropped, for any reason.** Not for being outside
a library root, not for being absent from disk, not for anything. **Normalisation is a rename of the
key, not a garbage collection.** A stats file is a record of what the user played; deciding on their
behalf that some of it no longer counts is not this slice's business and would not be recoverable.

---

## 1. WHERE NORMALISATION HAPPENS - both, and they are different jobs

**`recordPlay` normalises on write.** Without it the next play re-creates a case-variant key and the
split pairs come back. This is the actual fix.

**`load()` merges what is already stored.** Without it the 17 existing pairs stay split for ever,
because nothing else ever revisits them.

Neither alone is sufficient and they are not the same operation - one prevents, one repairs.

**The stored key becomes the normalised form**, so `save()` writes normalised keys with no extra
step: the migration reaches disk on the next ordinary save, through the atomic temp-and-rename path
`Config::save()` already uses (`.tmp`, then `MoveFileEx REPLACE_EXISTING | WRITE_THROUGH`). **This
slice adds no new write mechanism to a file that holds credentials.**

**Visible consequence, stated rather than discovered:** on Windows the `stat=` paths in `remoct.conf`
become lowercase, because that is what the normalised form is. Nothing displays those keys - the stat
views iterate the index and look up stats by path - so this is only visible to someone reading the
file. On Linux nothing changes at all (§2).

## 2. THE NORMALISED FORM - `foldPathKey`, reused

`libidx::detail::foldPathKey` already means exactly what is needed: case- and separator-folded on
Windows, **the identity on Linux**. LIB-S10 built it for the read-side join and LIB-S11 reused it for
root comparison. A third path-equality rule is not acceptable and is not proposed.

**On Linux this makes the whole slice a no-op by construction**, which is the right answer rather
than a limitation: `foldPathKey` is the identity there, so grouping by it groups only byte-identical
keys - and those cannot be duplicates, because `track_stats` is already a map. Two paths differing in
case on Linux are two different files and both entries survive untouched. **No `#ifdef` in the
migration; the platform rule lives in the one helper that already owns it.**

`Config.cpp` gains an include of `LibraryIndex.h`, which is pure and header-inline. **Flagged
because this is the exact shape of a hazard this project has paid for**: the secret-at-rest slice
broke every test linking `Config.cpp` by giving it a new dependency. `LibraryIndex.h` has no `.cpp`
and no library, so it cannot repeat that - but the test targets get a build check in the gate rather
than an assurance.

## 3. THE MERGE

At the end of `load()`, once, over the parsed map:

- group by `foldPathKey(key)`
- **`play_count` SUMS** - measured above to preserve 3901 exactly
- **`last_played` takes the MAXIMUM**, since it is not additive: the file was last played at the
  later of the two times, and the earlier one is not information about anything else
- the surviving key is the folded form

**Idempotent by construction.** Folding an already-folded key returns it unchanged, and a group of
one sums to itself, so a second run is a no-op. It runs at startup, and `config.load()` has exactly
one call site (`main.cpp:213`), so "once per session" is also literally once.

**Nothing else in the file is touched.** `save()` rebuilds the whole config from memory as it always
has; this changes the content of one map and nothing else in `DigiConfig`.

## 4. THE ROLLBACK STORY

**A migration that cannot be undone is not gateable.** The brief is right and this is the part I
would want in place before running it on anything of Dos's.

**The backup.** At the end of `load()`, **only when a merge actually changed something**, copy the
existing on-disk `remoct.conf` to **`remoct.conf.statbak`** beside it. At that moment the file on
disk is still entirely pre-migration - `load()` has read it and nothing has written yet - so the copy
is an exact pre-migration snapshot, taken with no window in which a partial write could exist.

**Written once, never clobbered.** If `remoct.conf.statbak` already exists it is left alone. That
matters more than it looks: the failure mode to protect against is a user running the new build,
finding something wrong, running it again, and having the good backup overwritten with the bad state.
A backup that can be overwritten by the thing it protects against is not a backup.

**No backup is written when there is nothing to migrate**, so a user whose stats are already clean
does not accumulate a stray file.

**Restoring is a file rename**, doable by a user with no tooling: quit RE-MOCT, rename
`remoct.conf.statbak` over `remoct.conf`, start it again. It is stated in the CHANGELOG entry rather
than left for a support conversation, and the one-time status line names the file.

**Telling the user.** A yellow bottom-left `status_msg_` on the first run after a merge: how many
duplicate entries were merged and that a backup was written. The mechanism is the one LIB-S11 settled
on for saying something without taking a pane. Not a toast, not a popup - a migration the user did
not ask for should say so once and get out of the way.

**Why this is safer than it sounds.** `save()` is already atomic, so the live config can never be
half-written. The merge is pure in-memory arithmetic over one map. And the numbers say the operation
is lossless on the real data before it is run once.

## 5. TESTS

**`config_stats_test`, new** - `Config.cpp` links into tests already (`encoder_quality_test` does),
so this follows that pattern with the same `SECRET_IMPL`/`SECRET_LIBS` wiring.

- **the real shape**: two keys differing only in case, counts 22 and 73, merge to one key with **95**
  and the later `last_played`
- **sum preservation** over a generated map with many pairs - total before equals total after, which
  is the property, rather than checking individual rows
- **idempotence**: merge, then merge again - byte-identical result
- **`recordPlay` normalises**: recording against `C:\...` and `c:\...` increments ONE entry to 2,
  not two entries of 1
- **nothing is dropped**: keys outside any root, and keys naming files that do not exist, both
  survive - the property §0 established
- **platform split**: on Linux two case-differing keys stay two entries with their counts intact;
  on Windows they merge. The same shape as `foldPathKey`'s own test.
- **round-trip through save/load** leaves the merged map unchanged
- **the backup**: written when a merge happens, NOT written when nothing merges, and **not
  overwritten when it already exists** - the last one is the property that makes it a backup
- everything else in a loaded config survives a migrating save: credentials, podcast subscriptions,
  playlist position, `library_root=` lines. Asserted by round-tripping a config containing all of
  them and diffing every other field.

**Mutation-tested**, with the verify-it-landed step. The mutations that matter: make the merge pick
one entry instead of summing (must fail the 95 assertion), make it non-idempotent, and let the backup
overwrite an existing file.

## 6. GATE - EYES-ON, BOTH PLATFORMS

**Against a COPY of Dos's real config, never the live one, until the copy passes.** I will prepare
the copy and the before/after totals; the comparison is mechanical and I can do it, but the decision
to point the real build at the real file is Dos's.

1. **`One of Us` reads 95.** Not 73, not 22.
2. **Total plays before == total plays after == 3901**, summed across all entries.
3. **278 stat lines after migration**, down from 295, with 17 merged and none lost.
4. **Everything else byte-intact**: Last.fm and ListenBrainz credentials still authenticate, podcast
   subscriptions all present, playlist position preserved, **both `library_root=` lines still there**.
5. **Play a track: the count goes up by ONE**, and no second entry appears for it.
6. **Run it twice**: the second run changes nothing and does not overwrite the backup.
7. **`remoct.conf.statbak` exists after the first run, and restoring it works** - tested by actually
   restoring it and confirming the split pairs come back.
8. The stat views and "Times Played" still read correctly post-migration - they should, since
   LIB-S10's read-side fold is a no-op on already-folded keys.
9. **Linux: a config with two genuinely different case-differing paths keeps both**, counts intact.
10. Stats for files under `D:\Music` (46 of them) and under no root at all (5) are still present.
11. Everything LIB-S3 to LIB-S12 still works; every other section enters, draws, exits, plays.

Machine: ctest both toolchains, `--no-tests=error`, currently **47/47 Windows, 48/48 Linux**, plus
the new test. **Verification split as in LIB-S4 through LIB-S12.** Brace-balance and scoped-diff audit.

## 7. FILES

`include/Config.h` (the merge declaration and a migrated-count field), `src/Config.cpp`
(`recordPlay` normalises, `load()` merges and backs up, the `LibraryIndex.h` include),
`src/UIManager.cpp` (the one-time status line), `tests/config_stats_test.cpp` **new**,
`tests/CMakeLists.txt`, `CHANGELOG.md`, and this note as design-of-record.

**Not touched:** `LibraryIndex.h` itself (`foldPathKey` is reused, not modified), `LibraryScanner`,
`LibraryNav.h`, `PaneScroll.h`, `PlaylistManager`, `Version.h`, the audio thread, `ar_crc`, the CD
path, the rip path. **LIB-S10's read-side fold stays** - still needed for configs that have not
migrated, and a no-op on those that have.

**Numbered, so nothing floats:** tag editing on browser rows **LIB-S14**, silent dupe denial
**LIB-S15**, library view navigation **LIB-S16**.

---

## 8. RESULTS

### Run against a COPY of Dos's real config, never the live one

The shipped `Config::load()` and `save()`, driven by a small harness pointed at a copied config dir:

| | |
|---|---|
| entries before / after | **295 -> 278** |
| **total plays before / after** | **3901 -> 3901** |
| entries merged | **17** |
| **`One of Us`** | **95** |
| roots / podcasts / credentials / volume / playlist position | **2 / 5 / `RadMageIRL` / 0.10 / 2** - all intact |
| written file | 502 lines, 278 `stat=`, 2 `library_root=`, 5 `podcast=` |
| **sum of counts in the WRITTEN file** | **3901** |

**The backup is byte-identical to the live original** (`cmp` clean, 519 lines, 295 entries, sum
3901). **Run twice**: the second run merged nothing, left 278 entries, and **did not touch the
backup** (md5 unchanged). **Restore tested rather than assumed**: copying `.statbak` back gives 295
entries, sum 3901, the split pairs present again, and the 5 launch-smoke keys and 46 `D:\Music` keys
all still there.

**Pointing the real build at the real file is Dos's call. I stopped here.**

### Confirmation 1 - never-clobbered, and it turned out to be enforced TWICE

**Implemented as an explicit file-level `if (!fs::exists(bak, ec))`**, not as a consequence of the
merge being a no-op the second time. That is what was asked for and it is what is there.

**But the mutation test found something better and worse.** Deleting that check and re-running left
the test PASSING - because `fs::copy_file` **defaults to failing when the destination exists**. So
the guarantee has two independent enforcers, which is real defence in depth, and neither the
explicit check nor the default is load-bearing alone.

That also means the first mutation could not prove the check does anything. Defeating BOTH - removing
the test and adding `copy_options::overwrite_existing` - **fails the assertion**, so the test does
catch a genuinely clobbering implementation. Recorded because "the test passed under mutation" would
otherwise have read as a weak test rather than a doubly-guarded one.

### Confirmation 2 - the `Config.cpp` -> `LibraryIndex.h` include is clean

**Every target builds on both toolchains**, including all three that link `Config.cpp`
(`encoder_quality_test`, `podcast_state_test`, and the new `config_stats_test`). No undefined
references. `LibraryIndex.h` is header-only with no library behind it, so it cannot repeat what
secret-at-rest did - and the build proves it rather than the reasoning asserting it.

### A FALSE PASS, caught by the verify-it-landed step

The first attempt at mutation 2 reported **0 failures**, and the mutation had not applied - a `perl`
multi-line regex that silently matched nothing. Without checking that the mutant text was actually in
the file, that would have been recorded as "the test survives removing the guard", which is the exact
LIB-S8 false-pass trap in a new costume. **The step that catches this is cheap and it earned its
place again.**

### Mutations, all three verified LANDED before either outcome was trusted

| mutation | caught by |
|---|---|
| merge PICKS instead of summing | `22 + 73 must be 95, got 22`, plus 4 more incl. the 200-pair sum |
| backup overwrites (both guards defeated) | `THE EXISTING BACKUP MUST NOT BE OVERWRITTEN` |
| `recordPlay` stops normalising | `two spellings, ONE entry, got 2` |

### Gates

**Windows 48/48, Linux 49/49**, `--no-tests=error`. `config_stats_test` **32 checks**.

### Noted, not actioned

The 5 keys under `C:\Users\david\smoke\files\` are LIB-S6 launch-smoke artifacts sitting in the real
stats. They survive the migration, which is correct under the no-drop policy, and they are **not**
special-cased. Recorded so nobody later reads them as plays Dos made.
