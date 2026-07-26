# DESIGN NOTE - Library slice 14: tag editing on browser rows

**Scope ID:** LIB-S14. **Status:** BUILT. Greenlit as proposed. **§9 holds the results and the §6
disposition.**
**Probed against tip `3b40f50`** on 2026-07-26 - a fresh read of all three named pieces, per the
disconfirmation clause.

**The premise this slice was split out on does not survive the tree. §0 first, because it makes the
slice much smaller than briefed.**

---

## 0. STOP AND REPORT - NONE OF THE THREE ASSUME A PLAYLIST ENTRY

The brief says `TagEditability`, `PlayingLocked` and `saveTagEdits()`'s playlist-sync loop "all
assume a playlist entry exists". **All three were read. None of them does.**

### `tagEditability` gates on the PATH only (`src/UIManager.cpp:4034`)

```cpp
UIManager::TagEditability UIManager::tagEditability(const std::string& path) const {
    if (path.empty())        return TagEditability::Empty;
    if (isCDTrackPath(path)) return TagEditability::NotAFile;
    if (isStreamPath(path))  return TagEditability::NotAFile;
    if (!audio_.streamMode() && !audio_.cdMode() &&
        path == audio_.currentTrack().path &&
        audio_.state() != PlaybackState::Stopped)
        return TagEditability::PlayingLocked;
    return TagEditability::Editable;
}
```

**It never touches the playlist.** It takes a path and answers about that path. `PlayingLocked` is
likewise a comparison against `audio_.currentTrack().path` - which is what is *sounding*, not what
is in a list. Both already mean exactly the right thing for a file that is not in the playlist.

### `saveTagEdits()`'s sync loop is a find-or-nothing (`:4079`)

```cpp
for (std::size_t i = 0; i < playlist_.size(); ++i) {
    if (playlist_.at(i).path == tag_edit_path_) { ...setDisplayTitle...; break; }
}
```

**A file that is not in the playlist simply matches nothing and the loop does nothing.** There is no
assumption to fix. The write itself - `TagLib::FileRef` + `ref.save()` - takes a path and knows
nothing about lists.

### So what actually blocks it is TWO LINES in the `e` handler

```cpp
const std::string& path = playlist_.at(idx).path;      // <- subj.path already holds this
...
const auto& e = playlist_.at(idx);
tag_edit_values_[0] = ct.title.empty() ? e.display_title : ct.title;   // <- the only real one
```

plus the S10 refusal above them and an `idx < playlist_.size()` guard that is meaningless for a
browser subject.

**This is not a new write capability across eight contexts. It is pointing an existing, working
writer at the subject `infoPaneSubject()` already resolves.** The split from LIB-S10 was still
right - a display-only fix would have edited the wrong file - but the reason it looked big was that
the debrief inferred those three dependencies rather than reading them, and I wrote that debrief.

---

## 1. THE TITLE SEED - the one genuinely new decision

The line that cannot be mechanically translated is the title fallback. Today, when the file is not
playing, `ct` is a default `TrackInfo{}` so `ct.title` is empty and the seed is
`playlist_.at(idx).display_title` - which for a playlist row is a real "Artist - Title".

**For a browser row the equivalent is `subj.display_title`, and it must NOT be used.** That value is
`dir_display_[cursor]`: a formatted ROW LABEL. At library level 3 it is
`"01  Apocalypse Please        3:21"`; in a `|` result it is
`"Muse - Apocalypse Please  [Absolution]  (flac)"`. Seeding the Title field with either would put a
track number, a duration and an album into a tag the user is about to save.

**The seed is not needed at all, and that is the point.** Immediately below, the existing code reads
the file with TagLib whenever `ct.title` is empty and overwrites all five fields from the real tags.
The `display_title` seed only survives for a file whose tags TagLib cannot read - exactly the case
where a formatted row label is the worst possible thing to write back.

**Proposal: seed the five fields EMPTY for a browser subject and let the TagLib read fill them.** A
file with no readable title then opens with an empty Title field, which is honest - and the user is
editing tags precisely because there are none.

## 2. WHICH CONTEXTS - enabled or excluded, each with a reason

`infoPaneSubject()` returns `Browser` for any row `browserRowIsFile()` accepts, which is already the
complete list. Taking each:

| context | decision |
|---|---|
| plain folder file | **enabled** - it is a file on disk, same as a playlist row |
| `[FAVs]` | **enabled** - absolute paths to real files |
| `[Recent]` | **enabled** - same |
| `[Books]` | **enabled** - same |
| library level 3 (tracks) | **enabled** - the slice's reason for existing |
| library `\|` search results | **enabled** - a result row IS a track (`rowIsPath`) |
| library stat views | **enabled** - falls out of `rowIsPath`, same as results |
| library levels 1-2, genres | **excluded by construction** - `browserEntryPath` returns `{}` for tag-text levels, so they never reach `Browser` |
| `[Drives]`, `[Radio]` | **excluded by construction** - same |
| directories | **excluded** - `browserRowIsFile`'s `is_directory` test |
| **`[Podcasts]` episodes** | **EXCLUDED DELIBERATELY, and this is the only judgement call** |

**Why podcast episodes are excluded.** They resolve as `InfoSource::Podcast`, not `Browser`, so they
are excluded by the same one-line gate today. Keeping that is deliberate rather than incidental: a
cached episode is a file the DOWNLOAD MANAGER owns - `d`/`Del` deletes it, a re-download replaces it,
and its identity in `podcast_progress` is the episode id, not the path. Tags edited into it are lost
the next time any of that happens, silently. **The refusal message stays for podcasts** and says so.

## 3. WHAT HAPPENS TO THE LIBRARY INDEX - update in place

The index stores `title`, `artist`, `album`, `album_artist`, `genre`, `year` per path. Editing a
file's tags makes its record wrong, and the library would keep browsing and searching the OLD values
until a rescan. That is silently wrong, which the brief rules out.

**Proposal: update the record in place after a successful write**, keyed on the path. It is cheap and
it self-heals:

- the five edited fields are written into the matching `LibraryTrack`
- **`mtime`/`size` are deliberately NOT refreshed.** The write changed them on disk, so the record
  now looks stale to the scanner - and the next rescan re-reads that one file and confirms what we
  already wrote. Leaving the revalidation key stale is what makes the in-place update a fast path
  rather than a second source of truth.
- **`rebuildCompilations()` re-runs**, because `artist` and `album_artist` feed `groupingArtist` and
  an edit can change whether an album is a compilation. Measured at 0.55 ms on the real index in
  LIB-S8, so this is not a cost worth avoiding.
- the index file is **not** rewritten - the in-memory index is what the pane reads, and a rescan
  will persist it. Writing the whole index after a single tag edit is disproportionate.

Matching the record uses **`foldPathKey`**, not a byte compare: the same file reached from the folder
browser and from the library can differ in spelling, which is precisely what LIB-S13 measured.

## 4. THE PLAYLIST SYNC - fold it, for the same reason

The existing loop compares `playlist_.at(i).path == tag_edit_path_` byte-exactly. LIB-S13 established
that path spellings vary by how a file was added, so the same file can be in the playlist under a
different case and its display title would not update. **One-line fix: compare through
`foldPathKey`.** Fourth use of the one helper, no new rule.

## 5. WRITE SAFETY - what already exists, and what it covers

The brief asks what protects against a partial or failed write. Most of it is already there and this
slice does not weaken it:

- `ref.save()` returns `bool` and **its result is captured** (`ok`), not assumed. On `false` the
  function returns early and **touches nothing in the UI** - the pane and the playlist keep showing
  the unchanged disk rather than the attempted edit.
- the whole write is inside `try`/`catch(...)`, and a throw returns `false`.
- a read-only or locked file makes `save()` return `false`, and the caller already surfaces
  **"Tag save failed - file locked or read-only"** on the warning line and leaves edit mode, so the
  user is not trapped.
- a file on an **offline LIB-S11 root** fails at `FileRef` construction - `isNull()` - and returns
  `false` down the same path.
- **the audio thread is untouched.** `PlayingLocked` refuses before any of this for the file that is
  sounding, so the editor never writes a file the decoder has open. That refusal applies to browser
  rows exactly as it does to playlist rows - the check is on the path, and it does not care where the
  path came from.
- **TagLib is the writer**, via the existing `utf8_to_wide` on Windows and the narrow path on POSIX -
  the `TL_PATH` idiom, unchanged.

**Not proposed: a backup of the music file.** LIB-S13 backed up `remoct.conf` because that migration
rewrote a file the user never asked us to touch. This is a user pressing `e`, typing, and pressing
Enter on one file they chose. Copying every edited music file would be a surprising amount of disk
for an explicit single-file action, and TagLib's in-place tag write is not the operation that eats
albums. **Stated so it is a decision rather than an omission** - if Dos wants it, it is a small
addition and its own ruling.

## 6. TESTS

Almost none of this is unit-testable: it is a curses handler plus a TagLib write. What IS testable is
the part that decides which file gets written.

**`tests/tag_edit_target_test.cpp`, new** - a small pure test over the decision, not the write:

- `tagEditability` returns `NotAFile` for a CD path and a stream URL, `Empty` for "", and `Editable`
  for an ordinary path - **including one that is in no playlist**, which is the property this slice
  depends on
- the fold-matched playlist sync finds a differently-cased entry on Windows and does not on Linux
- the index in-place update writes the five fields and **leaves `mtime`/`size` stale**, so a rescan
  still re-reads the file

`tagEditability` is a `UIManager` method and cannot be instantiated headless, so the testable part is
the path predicate it is built from; if extracting that turns out to cost more than it proves, **I
will say so and the whole of §6 becomes gate** rather than inventing a scaffold.

**Round-trip of a non-ASCII tag through TagLib is a real risk** and is machine-testable against a
fixture file: write `Björk - Jóga` and read it back byte-exact. That one is worth having whatever
happens to the rest.

**Mutation-tested**, with the verify-it-landed step.

## 7. GATE - EYES-ON, BOTH PLATFORMS

**On COPIES of music files first, never the real collection, until copies pass.** I will prepare a
copy directory.

1. `i` then `e` on a **library level-3 row**: edit the title, save, and it persists on re-read.
2. Same on a **`\|` search result**, a **plain folder file**, a **`[FAVs]` row**, and a
   **`[Recent]` row**.
3. **A `[Podcasts]` episode still refuses**, with a message saying why.
4. **A non-ASCII tag value round-trips byte-exact** - accented, and a CJK string.
5. **Editing the currently-playing file refuses** with "Stop playback first", from a browser row
   exactly as from a playlist row.
6. **A read-only file fails with the message**, not silently, and edit mode exits rather than
   trapping.
7. **The library browse list shows the new tags immediately** - no rescan needed - and a subsequent
   `F12` does not undo them.
8. **A compilation edit re-groups**: change an album-artist so an album stops being various-artists,
   and the artist list reflects it.
9. **Playlist sync with a different-cased path**: the same file in the playlist under another
   spelling gets its display title updated.
10. `e` on a **playlist row is unchanged**.
11. Everything LIB-S3 to LIB-S13 still works; every other section enters, draws, exits, plays.

Machine: ctest both toolchains, `--no-tests=error`, currently **48/48 Windows, 49/49 Linux**.
**Verification split as in LIB-S4 through LIB-S13.** Brace-balance and scoped-diff audit.

## 8. FILES

`src/UIManager.cpp` (the `e` handler's browser branch, the folded playlist sync, the index in-place
update), `include/UIManager.h` (one helper declaration), `tests/tag_edit_target_test.cpp` **new** and
`tests/CMakeLists.txt` if §6 survives contact, `CHANGELOG.md`, and this note as design-of-record.

**Not touched:** `infoPaneSubject()` itself - it already resolves the subject and this slice consumes
it rather than changing it. `LibraryIndex.h`, `LibraryScanner`, `PaneScroll.h`, `Config`,
`PlaylistManager`'s storage, `Version.h`, the audio thread, `ar_crc`, the CD path, the rip path.

**Numbered, so nothing floats:** silent dupe denial **LIB-S15**, library view navigation **LIB-S16**.

---

## 9. RESULTS, AND WHAT §6 BECAME

### §6 disposition - the path predicate was NOT extracted, and here is why

I offered to say so if extracting the path predicate from `tagEditability` cost more than it proved.
**It does, and it is worse than "not worth it": there is nothing left to extract.**

`tagEditability` is three checks on the path plus one on the audio state. The path-only part is
`path.empty()`, `isCDTrackPath(path)` and `isStreamPath(path)` - and those two are already pure
functions in `StringUtils.h` with their own coverage. Pulling them into a new predicate would create
a wrapper whose test asserts that `isCDTrackPath` is called, which is a test of the extraction rather
than of anything that could be wrong. The remaining check needs live `AudioManager` state and a
curses `UIManager`, which is not headless-testable at all.

**So §6's first two bullets become gate**, as offered - items 5 and 9 cover them.

### What DID get a test, and it earned its place

The non-ASCII round trip, which Dos said was worth having regardless. **Added to
`art_embed_test`**, which already synthesises real FLAC and MP3 through the encoder stack and already
links TagLib - so it cost no new link and no new fixture machinery.

It exercises exactly what `saveTagEdits` does: `TagLib::String(std::string, UTF8)` in, `ref.save()`,
reopen, `to8Bit(true)` out, through `utf8_to_wide` on Windows. **Both tag backends, because they are
different problems** - FLAC carries Vorbis comments which are UTF-8 by definition, while MP3 uses
ID3v2 frames with a per-frame encoding byte and a **Latin-1 default**, which is the one that silently
mangles anything outside it.

Three shapes that break differently: accented Latin (`Björk`, `Jóga`), CJK (`東京`), and a 4-byte
emoji. Asserted **byte-exact**, not "looks right" - a lossy round trip produces something readable
and wrong, which is the failure worth catching.

```
non-ASCII tags written to FLAC/Vorbis                      OK
non-ASCII tags round-trip BYTE-EXACT through FLAC/Vorbis   OK
non-ASCII tags written to MP3/ID3v2                        OK
non-ASCII tags round-trip BYTE-EXACT through MP3/ID3v2     OK
```

**Mutation-tested, verified LANDED:** writing the title as `TagLib::String::Latin1` instead of
`UTF8` fails **both** backends. So the test detects the encoding mistake it exists for.

### Gate copies prepared

**Nine real files copied out of the collection** to a scratch directory, never the originals -
6 FLAC and 3 Opus, including one with a non-ASCII filename (`03 4 Non Blondes - What's Up?.flac`,
whose title carries a character outside ASCII). Path is in the handoff; the collection itself is
untouched.

### Gates

**Windows 48/48, Linux 49/49**, `--no-tests=error`, clean builds both.

### Scope note

`tagEditability` and `saveTagEdits`'s write path are **unchanged** - not one line of the writer
moved. What changed is which subject reaches them, plus the two additions the design called for: the
folded playlist sync and the in-place index update. That is the whole slice, and it is the shape §0
predicted once the three pieces were actually read.
