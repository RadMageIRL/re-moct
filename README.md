# RE-MOCT — Music On Console Terminal

A feature-rich terminal audio player and CD ripper for Windows, built with C++20, ncurses, miniaudio, TagLib, libFLAC, LAME, and libebur128. Inspired by MOC (Music On Console), extended far beyond it.

```
┌─ RE-MOCT – Music On Console Terminal [REPEAT ALL] [SHUFFLE] [CD] [Tragic Kingdom (1995)] v1.0.0-rc1 ─┐
>> [Drives]
┌─────────────────────────────────┐  ┌─ Playlist [14]  46m 8s ──────────────────────────────────────────┐
│ [Recent]                        │  │ > No Doubt – Spiderwebs                                      4:28 │
│ [FAVs]                          │  │   No Doubt – Excuse Me Mr.                                   3:05 │
│ [Bookmarks]                     │  │   No Doubt – Just a Girl                                     3:29 │
│ C:\                             │  │   No Doubt – Happy Now?                                      3:43 │
│ D:\                             │  │   No Doubt – Different People                                4:35 │
│ E:\                             │  │   No Doubt – Hey You                                         3:34 │
│ F:\  ◄─ CD drive                │  │   No Doubt – The Climb                                       6:38 │
│ G:\                             │  │   No Doubt – Sixteen                                         3:22 │
└─────────────────────────────────┘  └──────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────┐
│ ════════════════════════════════════░░░░░░░░░░░░░░░░  2:14 / 4:28   │
└──────────────────────────────────────────────────────────────────────┘
 Ripping track 5/14  [47%]  8.3x  C2  →  AR: [A v2 OK conf=84]
 SPC:pause  n/p:next/prev  [/]:seek  -/+:vol  Tab:focus  Enter:play  ?:help  ^Q:quit
```

## Features

### Playback
- Plays MP3, FLAC, OGG, WAV, AAC, and any format supported by miniaudio
- Gapless crossfade between tracks (configurable duration)
- Repeat (track / all) and shuffle modes
- Seek forward/back, volume control
- Per-track play count and last-played tracking
- 10-band equalizer (Tab+E)
- ReplayGain tag support for consistent volume

### Library
- Split-pane: directory browser (left) + playlist/views (right)
- Drag-and-drop reorder in playlist
- Per-folder sort (name / date / size)
- Version-tracked note history with LCS diff in linked CODEX app
- Recent tracks, Favorites (FAVs) with star key, Bookmarks
- Full-text search across the playlist
- Tag editor (read/write via TagLib) — Title, Artist, Album, Year, Track
- Lyrics display (LRC file support)
- Queue system (separate from playlist)
- Goto bar for fast directory navigation

### CD Playback
- Red Book audio CD playback via Win32 IOCTL raw reads
- MusicBrainz disc lookup (Ctrl+R) — fetches album/track metadata
- Displays album/artist/year in title bar while CD is loaded
- Automatic track enumeration and length display

### CD Ripping (Ctrl+Y)
Three extraction modes selectable per-rip:

```
╔════════════════════════════════════════════════════════════════════╗
║         re-moct // SECURE AUDIO EXTRACTION PANEL                  ║
╠════════════════════════════════════════════════════════════════════╣
║ CD-ROM Target : F:\                                                ║
║ Disc          : 14 Tracks  (No Doubt - Tragic Kingdom)             ║
╠════════════════════════════════════════════════════════════════════╣
║ Select Ripping Strategy:                                           ║
║   [A] AccurateRip Mode  - Network CRC verify + drive offset        ║
║   [C] CUETools Mode     - Global CRC32 verify (offset-immune)      ║
║   [Y] Local Master      - Best-effort offline, clean encoding      ║
║   [N] Abort / Cancel    - Return to previous media navigation      ║
╠════════════════════════════════════════════════════════════════════╣
║ Output: C:\Users\...\Music\re-moct\No Doubt - Tragic Kingdom (1995)║
╚════════════════════════════════════════════════════════════════════╝
```

**Mode [A] — AccurateRip:** Full network handshake with accuraterip.com. Computes AR CRCv1 and CRCv2 checksums per track (per the whipper/CUETools reference implementation) and verifies against the database. Saves raw `.bin` payload and human-readable manifest to `.ar-db-info/` in the album folder.

**Mode [C] — CUETools:** Computes CTDBID (CRC32 of entire disc audio with ±10 sector trim for drive-offset immunity), queries cue.tools/db. Does not perform Reed-Solomon parity repair (verification only in this release).

**Mode [Y] — Local:** Best-effort rip with no network calls. Fastest. No AR/CTDB overhead.

**All modes produce:**
- FLAC (level 5, lossless)
- MP3 (LAME V0 VBR)
- Cover art embedded in both formats (fetched from Cover Art Archive via MusicBrainz)
- `folder.jpg` in album directory
- EBU R128 ReplayGain tags (track + album)
- Per-rip log in `logs/` subfolder
- C2 error pointer detection (if drive supports it)
- Dual-pass ripping on CRC mismatch

**Output structure:**
```
Music\re-moct\
└── No Doubt - Tragic Kingdom (1995)\
    ├── 01 - Spiderwebs.flac
    ├── 01 - Spiderwebs.mp3
    ├── 02 - Excuse Me Mr..flac
    ├── 02 - Excuse Me Mr..mp3
    ├── ...
    ├── folder.jpg
    ├── logs\
    │   └── rip_20260617_143022.log
    └── .ar-db-info\
        ├── accuraterip-id.txt
        └── 001dafc6-013faad0-d20df90e.bin
```

**Tags written (FLAC Vorbis / MP3 ID3v2):**
- Title, Artist, Album, Year, Track number
- ENCODER = `RE-MOCT v1.0.0-rc1`
- ACCURATERIP = status string (e.g. `AR v2 OK (conf 84)`)
- ACCURATERIPCRC = computed CRCv2 hex
- ACCURATERIPCOUNT = confidence score
- REPLAYGAIN_TRACK_GAIN / TRACK_PEAK
- REPLAYGAIN_ALBUM_GAIN / ALBUM_PEAK

## Prerequisites (MSYS2 UCRT64)

Open an **MSYS2 UCRT64** shell:

```bash
pacman -S --needed \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-ncurses \
    mingw-w64-ucrt-x86_64-taglib \
    mingw-w64-ucrt-x86_64-flac \
    mingw-w64-ucrt-x86_64-lame \
    mingw-w64-ucrt-x86_64-libebur128
```

Drop `miniaudio.h` into `lib/`:
```bash
curl -L https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h \
     -o lib/miniaudio.h
```

Also drop `nlohmann/json.hpp` into `lib/`:
```bash
curl -L https://github.com/nlohmann/json/releases/latest/download/json.hpp \
     -o lib/json.hpp
```

## Build

```bash
C:\msys64\usr\bin\bash.exe -l -c \
  'export PATH=/ucrt64/bin:$PATH && cd /e/code/remoct && \
   rm -rf build && mkdir build && cd build && cmake .. -G Ninja && ninja'
```

Binary: `build\bin\remoct.exe`

**Required DLLs alongside remoct.exe:**
```
C:\msys64\ucrt64\bin\libFLAC.dll
C:\msys64\ucrt64\bin\libmp3lame.dll
C:\msys64\ucrt64\bin\libebur128.dll
```

## Keybindings

### Global
| Key | Action |
|-----|--------|
| `Tab` | Toggle focus: DirBrowser ↔ Playlist |
| `Enter` | Play file / enter directory |
| `Space` | Pause / Resume |
| `s` | Stop |
| `n` / `p` | Next / Previous track |
| `[` / `]` | Seek back / forward 5s |
| `-` / `+` | Volume down / up |
| `Ctrl+Q` | Quit |
| `?` | Toggle help pane |

### Directory Browser
| Key | Action |
|-----|--------|
| `j` / `↓` | Navigate down |
| `k` / `↑` | Navigate up |
| `←` / `h` | Go to parent directory |
| `a` | Add selection to playlist |
| `*` | Star / unstar (add to FAVs) |
| `g` | Goto bar (type path) |
| `b` | Add bookmark |

### Playlist / Queue
| Key | Action |
|-----|--------|
| `d` | Delete track from playlist |
| `q` | Add to queue |
| `Q` | Show queue pane |
| `Shift+↑/↓` | Drag-and-drop reorder |

### Views (right pane)
| Key | Action |
|-----|--------|
| `Shift+T` | Track info / tag editor |
| `Shift+L` | Lyrics |
| `Shift+A` | About |
| `Shift+B` | Bookmarks |
| `Shift+E` | EQ (10-band equalizer) |
| `Shift+D` | Devices |

### CD Functions (CD mode only)
| Key | Action |
|-----|--------|
| `Ctrl+R` | Fetch MusicBrainz metadata |
| `Ctrl+Y` | Open rip mode selection panel |

## Configuration

Config file: `%APPDATA%\re-moct\remoct.conf`

```ini
# Playback
volume=5
repeat=all          # off / track / all
shuffle=0

# Crossfade
crossfade=1
crossfade_ms=2000

# UI
theme=dark

# Favorites (FIFO, max 50)
fav=C:\Music\Pink Floyd
fav=C:\Music\No Doubt

# Bookmarks
bookmark=C:\Music\Lossless
```

## Project Structure

```
remoct/
├── CMakeLists.txt
├── BUILD.md
├── README.md
├── lib/
│   ├── miniaudio.h          ← miniaudio, download separately
│   └── json.hpp             ← nlohmann/json, download separately
├── include/                 (headers; .cpp counterpart in src/ unless noted)
│   ├── UIManager.h          ← ncurses UI: panes, modals, Classic/Awesome
│   ├── AudioManager.h       ← miniaudio backend, lock-free ring, crossfade
│   ├── PlaylistManager.h    ← playlist, queue, recent, favs, [Radio]/[Books]
│   ├── CDSource.h           ← Win32 IOCTL CD playback + TOC/sector read
│   ├── CDRipper.h           ← CD ripping (AR/CTDB/Local modes)
│   ├── ar_crc.h             ← pure AccurateRip CRC math (off-disc testable)
│   ├── drive_offsets.h      ← AccurateRip drive-offset table (header-only)
│   ├── MBLookup.h           ← MusicBrainz disc ID + Discogs lookup
│   ├── CoverArt.h           ← cover art fetch (iTunes/Deezer/CAA) + embed
│   ├── StreamSource.h       ← HTTP/HLS radio producer, ring buffer, re-pin
│   ├── AacDecoder.h         ← FDK-AAC / HE-AAC stream decode
│   ├── IHeartRadio.h        ← iHeart station resolve + metadata API
│   ├── IHeartNowPlayingSM.h ← pure now-playing reconciliation state machine
│   ├── IHeartDeepLog.h      ← opt-in NDJSON deep-analysis log (Ctrl+A)
│   ├── RadioBrowser.h       ← radio-browser.info station search
│   ├── Mp4Chapters.h        ← .m4b audiobook chapter parsing
│   ├── LastFm.h             ← Last.fm scrobbling
│   ├── ListenBrainz.h       ← ListenBrainz scrobbling
│   ├── DiscordRP.h          ← Discord Rich Presence (named-pipe IPC)
│   ├── LrcData.h            ← LRC lyrics parser
│   ├── Config.h             ← config file read/write
│   ├── Log.h                ← logging + rotation
│   ├── Toast.h              ← transient status messages
│   └── StringUtils.h        ← UTF-8/wide, column-aware text (header-only)
├── src/                     (implementations; mirror include/ + main.cpp)
│   ├── main.cpp
│   ├── UIManager.cpp        ← all UI logic (largest TU)
│   ├── AudioManager.cpp     ← miniaudio device, CD + stream audio paths
│   ├── PlaylistManager.cpp
│   ├── CDSource.cpp         ← TOC read, raw sector read, drive-offset DB
│   ├── CDRipper.cpp         ← FLAC+MP3 encode, AR CRC, CTDB, ReplayGain
│   ├── ar_crc.cpp
│   ├── MBLookup.cpp         ← WinInet MB/Discogs API, JSON parse
│   ├── CoverArt.cpp
│   ├── StreamSource.cpp
│   ├── AacDecoder.cpp
│   ├── IHeartRadio.cpp
│   ├── IHeartNowPlayingSM.cpp
│   ├── IHeartDeepLog.cpp
│   ├── RadioBrowser.cpp
│   ├── Mp4Chapters.cpp
│   ├── LastFm.cpp
│   ├── ListenBrainz.cpp
│   ├── DiscordRP.cpp
│   ├── LrcData.cpp
│   ├── Config.cpp
│   ├── Log.cpp
│   └── Toast.cpp
└── tests/                   ← Phase 0 pure-unit tests (ctest)
    ├── CMakeLists.txt
    ├── ar_crc_test.cpp      ← AccurateRip CRC + offset normalization
    └── iheart_sm_test.cpp   ← now-playing state machine
```

## AccurateRip Implementation Notes

The AR verification follows the whipper / CUETools reference implementation:

**Disc ID computation:**
```
rel_lba[i] = absolute_lba[i] - 150      (normalize: track 1 = 0)
disc_id1    = sum(rel_lba) + leadout_rel
disc_id2    = sum(rel_lba[i] * (i+1)) + leadout_rel * (ntracks+1)
cddb_id     = standard FreeDB formula on absolute LBAs
```

**URL format:**
```
http://www.accuraterip.com/accuraterip/[id1[7]]/[id1[6]]/[id1[5]]/
    dbar-NNN-disc_id1-disc_id2-cddb_id.bin
```
(all lowercase, nibbles extracted by string index position)

**CRC computation (per whipper accuraterip-checksum.c):**
```cpp
// mul_by increments for EVERY sample (including boundary-skipped ones)
// Skip enforced via range check, not by withholding the counter
for each sample:
    uint32_t samp = left | (right << 16)   // L in low bits, R in high
    if (mul_by >= ar_check_from && mul_by <= ar_check_to):
        uint64_t p = (uint64_t)samp * mul_by
        csum_hi += upper32(p)
        csum_lo += lower32(p)
    mul_by++

crc_v1 = csum_lo
crc_v2 = csum_lo + csum_hi

// Track boundaries:
// First track: ar_check_from = 5*588 = 2940 (skip first 2940 samples)
// Last track:  ar_check_to   = total_samples - 2940
// Other tracks: full range (1 to total_samples)
```

**Binary payload format (.bin file):**
```
Per chunk (13 + ntracks*9 bytes):
  [1]  track_count
  [4]  disc_id1 (LE)
  [4]  disc_id2 (LE)
  [4]  cddb_id  (LE)
  Per track (9 bytes):
    [1]  confidence
    [4]  main_crc    (checksum to compare against crc_v1 or crc_v2)
    [4]  frame450_crc (offset-detection checksum, reserved for future use)
```

## Drive Offset Database

CDSource queries the drive model on open via `IOCTL_STORAGE_QUERY_PROPERTY` and looks up the AccurateRip read offset from a built-in table in `CDSource.cpp`. HL-DT-ST (LG laptop) drives default to +6 samples. Unknown drives default to 0.

To add your drive, find it at https://www.accuraterip.com/driveoffsets.htm and add an entry to `lookupDriveOffset()` in `src/CDSource.cpp`.

## Rip Log Format

Each rip writes a timestamped log to `<album_dir>\logs\rip_YYYYMMDD_HHMMSS.log`:

```
RE-MOCT CD Rip Log
==================
Album  : Tragic Kingdom
Artist : No Doubt
Year   : 1995
Drive  : F  (offset +6 samples)
Tracks : 14
Mode   : AccurateRip
Output : C:\Users\...\Music\re-moct\No Doubt - Tragic Kingdom (1995)

=== AccurateRip ===
ntracks=14 cddb=d20df90e disc_id1=001dafc6 disc_id2=013faad0
TRY 001dafc6/013faad0 -> HTTP 200
HIT! disc_id1=001dafc6 disc_id2=013faad0 size=1112

Track 1 Pass 1: crc_v1(=csum_lo)=c1541003  crc_v2(=csum_lo+hi)=...  status=1 (AR v2 OK)
  DB main_crcs (8):
    crc=c1541003 conf=84  <-- MATCH v1
    crc=37643fe3 conf=200
    ...

=== Summary ===
AR: 13 v2 + 1 v1 matched, 0 not found / 14 total
ReplayGain: album gain=-8.43 dB peak=1.033159
  Track 01: [AR v2 OK] conf=84  rg=-8.36dB
  Track 02: [AR v2 OK] conf=83  rg=-7.91dB
  ...
```

## CTDB (CUETools) Mode Notes

CTDBID = CRC32 of entire disc audio with first and last 10 sectors (23,520 bytes) trimmed. This makes the ID independent of drive read offset — any drive within ±10 sectors produces the same ID.

Queried at `http://db.cuetools.net/lookup2.php?version=3&ctdb=1&disc=<id>&tracks=<n>`

Reed-Solomon parity repair is not implemented in this release. Mode [C] currently provides verification status (Correct / Correctable / Unknown) without repair.

## Version History

**v1.0.0-rc1** (current)
- Full CD ripper with AccurateRip, CUETools, and Local modes
- A/C/Y/N rip mode selection panel
- Dual-pass ripping with CRC verification
- EBU R128 ReplayGain (inline, no second pass)
- MusicBrainz cover art from Cover Art Archive
- C2 error pointer detection
- Drive read offset lookup (CDSource)
- Per-rip logs + `.ar-db-info/` binary cache
- CUETools-standard ACCURATERIP* tags
- FAVs system (star key)
- Tag editor
- 10-band EQ
- Queue system
- Goto bar
- LRC lyrics
- Bookmarks
- Crossfade / gapless playback
