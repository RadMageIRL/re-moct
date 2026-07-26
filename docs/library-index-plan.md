# Library slice 1 - metadata index and on-disk cache format

Design of record for **LIB-S1**, greenlit 2026-07-25. Built against tip `6d40021`.

Campaign context: `docs/ROADMAP-library-view.md`. The capability verdict (core module,
plugin route closed) and the fork (path b - seventh flag plus a library-only
level-descriptor) are settled there and are not re-derived here.

This slice is the metadata index and its format: the riskiest new piece, and the one
provable standalone before any UI wiring exists. Same discipline as the podcast RSS
parser - build the pure thing, prove it against real and hostile input, integrate only
once it holds.

---

## 1. Module

`include/LibraryIndex.h`, namespace `libidx`. Header-inline and pure, mirroring
`PodcastFeed.h` and `PodcastChapters.h`: no I/O, no filesystem, no globals, no throw. It
has **no dependency beyond the standard library**, so the test compiles the header and
links nothing at all.

Split trigger, inherited from `PodcastFeed.h`: it moves to a `.cpp` when a second product
TU needs it. Slice 3 will be the first. Flagged, not pre-empted.

Defensive contract, inherited verbatim: never crash, never throw, never hang. A malformed
file degrades to fewer records, ultimately to an empty index the caller reports as an
honest "no library" rather than an error. One linear pass over the text, one sort per
query, nothing quadratic.

## 2. Record

```
struct LibraryTrack {
    std::string path;              // absolute; IDENTITY - round-trips byte-exact
    std::string artist, album, album_artist, title, genre;
    int32_t     track_no, disc_no, year, duration_sec;
    int64_t     mtime;             // slice-2 revalidation key
    uint64_t    size;              // slice-2 revalidation key
};
```

`mtime` and `size` exist solely so slice 2 can decide what to re-read: a file whose path,
mtime and size all match its record is skipped, everything else is re-tagged. They
round-trip exactly for that reason.

Tag text is stored **raw**. `sanitizeForDisplay` runs at draw time in slices 3-4. Folding
on the way in would be lossy, and it would also be insufficient - `sanitizeForDisplay`
passes ASCII control bytes straight through, as the chapters slice discovered.

**Play count is deliberately absent.** `Config` already owns `TrackStats {play_count,
last_played}` keyed by path. Duplicating it here would create two sources of truth that
drift; slice 7's most-played and recently-played views join on `path` at query time.

## 3. Format

```
remoct-library-index<TAB>1
root<TAB><escaped root>
<12 tab-separated fields, one line per track>
```

A version mismatch **discards and rescans**. There is no migration path and deliberately
no plan for one: the index is a cache, fully rebuildable from the files it describes, so
migration code would be pure cost.

**Escaping is a correctness requirement, not tidiness.** `Config.cpp` persists tab-delimited
records too, but lossily on purpose - it folds tabs to spaces and strips CR/LF so a
`podcast=<url>\t<title>\t<art>` line can never mis-split. That is right for a feed title,
which is a display string. It is wrong here, because the first field is a **path**:
identity, not decoration. A folded path names a file that does not exist, and it fails on
exactly the tracks nobody checks. Tabs and newlines are both legal in POSIX filenames.

So every string field is backslash-escaped - `\\`, `\t`, `\n`, `\r`, and `\0` so a field is
always safe to hand to C-string code later - and round-trips byte-exact.

Parsing splits on **raw** tabs before unescaping, and only in that order: at split time an
escaped tab is still the two characters `\` `t`, so a separator is always a real separator.
Doing it the other way round would re-split fields open.

Other format decisions:

- **Strict field count.** A record without exactly 12 fields is skipped and counted in
  `skipped_records`, never guessed at. Same for a non-numeric numeric field or an empty
  path - a record with no path names nothing and can never be played, so it is corruption
  rather than a track.
- **A trailing `\r` is tolerated** on every line. Slice 2 writes this file on Windows; if
  any path ever opens it in text mode, a stray CR must not glue itself to the last field.
- **No record cap.** A library legitimately has six figures of tracks. The bound is the
  input the caller already holds in memory.
- `ok == false` means the header was not understood (empty, wrong magic, unsupported
  version, missing root line) and the index is empty. `ok == true` with
  `skipped_records > 0` means the file was readable but partly corrupt - still usable, and
  reported rather than hidden.

## 4. Cache location

`<config dir>/library.idx`, beside `remoct.conf` and `theme.conf` (`Config.cpp`'s
`themePath()` is the precedent). **Never in the music root** - the collection directory is
the user's.

Decided here, implemented in slice 2, which adds `libraryIndexPath()` mirroring
`themePath()` and does the actual I/O through `port::fopenUtf8`.

## 5. Query surface, and the grouping seam

```
ParseResult parseIndex(const std::string& text);
std::string serialiseIndex(const LibraryIndex&);

const std::string& groupingArtist(const LibraryTrack&);

std::vector<std::string> artists(const LibraryIndex&);
std::vector<std::string> albumsForArtist(const LibraryIndex&, const std::string& artist);
std::vector<std::size_t> tracksForAlbum(const LibraryIndex&, const std::string& artist,
                                        const std::string& album);
```

Every query returns a **deterministically sorted** result: the pane draws them directly,
and a list that reorders itself between launches reads as a bug. Sorting is
case-insensitive with a raw-byte tie-break so the order is total. The case fold is
ASCII-only and deliberately not locale-aware - the application runs under
`setlocale(LC_ALL, "")` with a CP1252 narrow encoding, so a locale-sensitive fold would
make list order machine-dependent.

Case variants collapse: "The Beatles" and "the beatles" are one row, keeping the first
spelling in sorted order. An empty grouping artist (a file with neither tag) is kept as its
own group rather than dropped - losing tracks silently would be worse. What that row is
*labelled* is the UI's call in slices 3-4.

`tracksForAlbum` orders by disc, then track number, then title, then path. The last two
are what make the order total for untagged rips, where every track number is 0 and only the
filename distinguishes them.

**The grouping seam.** `groupingArtist` is one named function that every hierarchy query
routes through, shipping the simplest defensible rule: album-artist when the tag carries
one, else artist. That already holds a properly-tagged compilation together. **Slice 7 owns
the real rule** - Various Artists recognition, albums whose tracks disagree on artist - and
changes only this function.

Shipping no rule at all was considered and rejected on review: it would not defer the
decision, it would scatter it into slices 3-4 as ad-hoc UI logic that slice 7 would then
have to hunt down.

## 6. Paths are strings, start to finish

**CORRECTED 2026-07-26 (LIB-S2), measured on both toolchains.** This section previously
said the throw happens for any byte CP1252 cannot map, i.e. for non-ASCII. That was
reasoned rather than measured, and it is wrong. The corrected rule:

**libstdc++ on Windows treats a narrow `fs::path` string as UTF-8. The trigger is INVALID
UTF-8, not non-ASCII.**

| input | Windows (UCRT64) | Linux |
|---|---|---|
| valid UTF-8 - accents, smart quotes, CJK, 4-byte emoji | no throw, byte-exact round-trip, real files found | fine |
| **invalid** UTF-8 - a raw `0x92`/`0x81`/`0x9D`/`0xE9`, a lone continuation byte, a truncated sequence | **throws** `Illegal byte sequence`, from `fs::path()` **and** from `fs::exists(s, ec)` | fine, bytes pass through |

The old wording could not explain why `UIManager.cpp`'s `fs::directory_iterator(current_dir_)`
has always worked over a collection containing 137 non-ASCII paths. The corrected rule
explains both that and the slice-5 crash: slice 5 built a **path out of feed title text**,
and feed text carries raw Latin-1 bytes.

**So: paths that come from the OS are safe. Paths built from feed or tag text are not.**

That distinction is load-bearing rather than academic. Under the old rule, LIB-S2 was about
to hand-roll a `FindFirstFileW`/`opendir` seam purely to avoid `fs::path` - complexity
generated entirely by a wrong mechanism in this document. `std::filesystem` is usable for
an OS-origin walk; the scanner keeps a `try`/`catch` only because an index file can carry a
path written on another platform.

This unit therefore never converts a path to anything - it stores, escapes, compares and
sorts them as strings. Slice 1 needs no filesystem call at all, which is the confirmation
the brief asked for that scope has not drifted into slice 2. Where a later slice must
actually open a file, `port::fopenUtf8` (`_wfopen` over `utf8_to_wide`, `PortUtil.h`) is
the one sanctioned route.

## 7. Test and gate

`tests/library_index_test.cpp` - header-only, device-free, both matrix jobs. 116 checks.

- **Exact round-trip** of every string field through tab, newline, CR, backslash, all four
  combined, a lone trailing backslash, the literal characters `\t`, empty, UTF-8
  multi-byte, the CP1252 landmine bytes (a smart quote), an embedded NUL, and a 171-char
  path (the real longest in the collection).
- **Numeric extremes**, including `INT64_MIN` and `UINT64_MAX`.
- **Header rejection**: empty, blank, header-with-no-root, wrong magic, future version,
  version 0, non-numeric version, missing root key, over-long header. A valid header with
  zero records is an *empty library*, not an error - a scan that found nothing must be
  distinguishable from a corrupt file.
- **Record rejection**: too few or too many fields, non-numeric numerics, negative size,
  size overflow, empty path, and a line that is not a record at all. Each must keep the
  surrounding good records and count exactly one skip.
- **Truncation and line endings**: missing trailing newline, CRLF throughout, truncated
  mid-record, and *every prefix* of a valid file parsed without throwing or hanging.
- **Queries**: compilation held together by album-artist, case-variant collapse, empty
  group kept, case-insensitive lookup, unknown album empty rather than crashing, untagged
  rips ordered by the path tie-break, and repeat runs identical.
- **Scale, measured and printed rather than asserted** as a promised number.

### Measured

Release, both platforms. Real scale is the actual collection (2,155 + 618 = 2,773 audio
files); 100k is synthetic headroom an order of magnitude beyond it.

| platform | n | bytes | serialise | parse | artists() |
|---|---|---|---|---|---|
| Windows UCRT64 | 2,773 | 358 KB | 0.9 ms | 2.0 ms | 1.3 ms |
| Windows UCRT64 | 100,000 | 12.6 MB | 32.0 ms | 56.4 ms | 54.3 ms |
| Linux Debian | 2,773 | 358 KB | 1.0 ms | 3.5 ms | 0.9 ms |
| Linux Debian | 100,000 | 12.6 MB | 26.7 ms | 84.3 ms | 43.2 ms |

The index for the real collection is **358 KB and parses in 2-4 ms**, so loading it at
startup is free. Note that the default `build/` directory has an empty `CMAKE_BUILD_TYPE`
and its unoptimised numbers are roughly 10x worse (21 ms parse at real scale); the table
above is `-O2`, which is what ships.

Gates: **Windows 42/42, Linux 43/43** (the new test is the +1). Brace-balanced,
scoped-diff clean - one tracked file modified (`tests/CMakeLists.txt`) and two added; zero
`src/` or `plugins/` files touched; nothing in `ar_crc`, the CD path, the audio thread,
`UIManager`, or `Version.h`.

## 8. Carried into slice 3

**`tracksForAlbum` returns indices, and they are valid only against the index instance that
produced them.** Once slice 2 can rebuild the index while the UI holds a selection, the UI
must either re-query after a rebuild or hold the path - the stable identity - rather than
the index. Deliberately not papered over here with a generation counter this slice cannot
test. Slice 3's design note owns the answer.

## 9. Fences honoured

No scanner, no directory walk, no TagLib, no threading, no cancellation, no UI, no section
flag, no `UIManager` change, no config key or toggle, no album-artist or compilation
grouping *rules* (the field is stored and the seam exists; the rule is slice 7's), and no
writing the cache to disk. Nothing deferred silently.
