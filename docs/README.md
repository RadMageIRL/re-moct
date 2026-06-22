## 🚧 In Development (dev branch)

Features being tested, not yet in a stable release:
See current binary/exe in the binary-dev directory 

# What's New in This Branch (since v1.0.0-RC1)

This dev branch adds a complete **internet-radio subsystem** and **Last.fm scrobbling** on top of the RC1 CD player/ripper. Everything below is initially tested against live streams, but is not yet frozen for release.

## Internet Radio Streaming

Play HTTP/HTTPS audio streams directly in RE-MOCT, with the same keyboard-driven workflow as local files.

- **MP3 and AAC/HE-AAC** stream decoding. HE-AAC (AAC+ / SBR) is fully supported via FDK-AAC, so low-bitrate stations that other players choke on work correctly.
- Streams decode into the same lock-free audio path as CD/file playback — volume, balance, EQ, and the visualizer all apply.
- Resilient producer: prebuffering, automatic reconnect with backoff, and graceful handling of dropped connections.
- Output is normalized to 44.1 kHz / stereo; off-rate or mono stations are resampled transparently.

## `[Radio]` Station Pane

A dedicated radio view alongside the file browser.

- Saved stations persist across sessions.
- Add a stream by URL with **Ctrl+U**; remove a saved station with **d** or **Del**.
- Stations restore with a clean `RADIO:` label after restart.

## Station Discovery (radio-browser.info)

Find stations from inside the app instead of hunting for URLs elsewhere.

- Press **/** in the `[Radio]` pane to search the radio-browser.info directory.
- Results appear inline, most-popular-first, with bitrate / codec / country shown.
- Selecting a result plays it, saves it, and counts a courtesy "click" upstream.
- Plays the resolved stream URL (playlists/redirects pre-resolved); falls through mirror servers if one is down.

## Live Now-Playing (ICY Metadata)

For streams that broadcast Shoutcast/Icecast metadata, the current song is shown live.

- The bottom scrubber area — meaningless for a live stream — is repurposed to display the live `Artist – Title` with a `[LIVE]` indicator.
- Metadata is de-interleaved out of the byte stream before decoding, shared cleanly by both the MP3 and AAC paths.
- Stations without metadata fall back to a simple `[LIVE]` marker.

## Last.fm Scrobbling

Scrobble both local files and radio to Last.fm.

- **One-time in-app setup:** press **Ctrl+G**, enter your API key and secret (stored in config), approve in the browser — authentication then **completes automatically** (no second keypress).
- Scrobbles local files (from tags) and radio tracks (from ICY metadata).
- Sends "now playing" on track start and scrobbles once the track passes Last.fm's threshold (>30s, played to 50% or 4 minutes).
- All calls are MD5-signed via the OS crypto API. Scrobbling silently no-ops if you haven't logged in.

## Fixes

- Selecting a station no longer leaves stale file entries in the playlist (radio now replaces the playlist, matching CD behavior).
- `RADIO:` labels survive a restart instead of reverting to the raw URL.
- Last.fm auth no longer loops if the app is restarted mid-authorization (the in-flight token is persisted).

## New Runtime Dependency

AAC playback adds one bundled DLL: **`libfdk-aac-2.dll`** (Fraunhofer FDK AAC). Note its license is the Fraunhofer FDK AAC Codec License — it must be documented in `THIRD_PARTY-NOTICES.md` before release.

## New Keys

| Key | Action |
| --- | --- |
| `Ctrl+U` | Add / play a stream by URL |
| `/` | Search radio-browser.info (in `[Radio]`) |
| `d` / `Del` | Remove saved station (in `[Radio]`) |
| `Ctrl+G` | Last.fm login |

Build dependencies (pacman install)pacman -S \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-flac \
  mingw-w64-ucrt-x86_64-lame \
  mingw-w64-ucrt-x86_64-libebur128 \
  mingw-w64-ucrt-x86_64-fdk-aac \
  mingw-w64-ucrt-x86_64-ncurses \
  mingw-w64-ucrt-x86_64-taglib

Compile for DEV version or grab latest exe in binary-dev (need the dlls listed in this page)



# RE-MOCT - Music On Console Terminal

> A homage to MOC - keyboard-driven terminal CD player and ripper for Windows.

## Download

**[⬇ RE-MOCT v1.0.0-RC1 — Windows x64](https://github.com/RadMageIRL/re-moct/releases/download/v1.0.0-RC1/re-moct-v1.0.0-RC1-win-x64.zip)** · [all releases](https://github.com/RadMageIRL/re-moct/releases)

> Release candidate. Extract and run `remoct.exe` — all DLLs included. Windows 10+.

📖 **[Documentation & Feature Guide](https://radmageirl.github.io/re-moct/)**

---

**Status:** Work in progress - Release candidate available. A prebuilt Windows x64 binary is published as v1.0.0-RC1 on the GitHub Releases page. Polishing the Windows client, Linux port planned.

Built with C++20 · ncurses · miniaudio · TagLib · AccurateRip
<br>

## Highlights

- **AccurateRip CRCv2 + CUETools (CTDB) verification.** Disc-absolute CRCs
  computed from the 150-sector lead-in; pressing offset auto-detected via
  frame-450 CRC scan; drive offset applied as a combined sample shift split
  into whole-sector LBA advance plus sub-sector skip. Offset-immune CTDB
  fallback when AccurateRip has no entry.
- **Real-time-safe audio path.** Raw CDDA sector reads on a dedicated thread
  feeding a lock-free ring buffer - no mutex in the audio callback. Next track
  is pre-decoded into a second buffer for gapless/crossfade transitions.
- **Correct handling of awkward discs** - hidden lead-in audio, Blue Book /
  CD-Extra multisession discs, non-standard pressings (see Verified Rips).
- Gapless, crossfade, ReplayGain, 10-band biquad EQ, BPM detection,
  MusicBrainz DiscID lookup with Discogs fallback, tag editor, LRC lyrics.

## Verified Rips

Real RE-MOCT logs from discs that trip up naïve rippers are in
[`logs/`](https://github.com/RadMageIRL/re-moct/tree/main/logs):

**Joan Osborne - *Relish* (1995)** - non-standard lead-in: track 1 begins at
`lba=182` instead of the usual 150, i.e. a 32-frame pregap. RE-MOCT detects the
true track-1 LBA and still verifies all 12 tracks at AccurateRip v2, conf 200.

```
  track[01] lba=182  rel=32        # normal is lba=150 — 32-frame pregap
  t1_lba=182  leadout_rel=275790
  === Summary ===
  AR: 12 v2 + 0 v1 matched, 0 not found / 12 total
```

**Goo Goo Dolls - *Gutterflower* (2002)** - Blue Book / Enhanced CD with a data
session alongside the audio. RE-MOCT parses the audio TOC, excludes the data
track, and verifies all 12 tracks at AccurateRip v2, conf 200.

```
  === Summary ===
  AR: 12 v2 + 0 v1 matched, 0 not in database, 0 network error / 12 total
```

Both ripped on a HL-DT-ST GHD3N at +6-sample offset. A third log
(`Prince_Rip.log`) shows the same pipeline verifying clean on a *different*
drive (ASUS SDRW-08U7M-U) - evidence the offset handling generalises across
drives, not just one unit.

Example TUI:

<img width="1367" height="664" alt="image" src="https://github.com/user-attachments/assets/d0d189b7-d301-4ae7-a2ac-dd62a507cbb0" />
<br><br>
<img width="1125" height="618" alt="image" src="https://github.com/user-attachments/assets/eccefb9e-1d83-4422-b72a-113b282a68d7" />
<br><br>
<img width="1124" height="616" alt="image" src="https://github.com/user-attachments/assets/0b67803d-2800-4e4b-aec4-697e0e807744" />
<br><br>

## Joan Osborne - Relish (The Lead-in LBA Issue - Solved)

<br>
<img width="703" height="795" alt="image" src="https://github.com/user-attachments/assets/00ca1e84-b779-4bb9-b47e-f8cf5fea6c19" />

## Goo Goo Dolls - Gutterflower (Enhanced CD and Hidden Track - Solved)
<br>
<img width="700" height="795" alt="image" src="https://github.com/user-attachments/assets/4df0a844-56f6-4fff-8584-a5af428f3aad" />
<br>

## AccurateRip Pipeline (RE-MOCT Native Handshake Implementation)

```mermaid
%%{init: {'flowchart': {'htmlLabels': true, 'padding': 18}}}%%
graph TD
    classDef hardware fill:#13161b,stroke:#4fc8a0,stroke-width:2px,color:#cdd5df;
    classDef handshake fill:#1a1e25,stroke:#c98bff,stroke-width:2px,color:#cdd5df;
    classDef forensic fill:#111418,stroke:#f0c060,stroke-width:2px,color:#cdd5df;
    Start([START RIP]) --> A[Phase 01: Disc ID Gen]
    A --> B[Phase 02: WinInet Handshake]
    B --> C{HTTP 200?}
    C -- Yes --> D[Phase 03: .bin Binary Parse]
    C -- No/404 --> F([NOT FOUND])
    D --> O[Phase 04: Drive Offset Correction]
    O --> P["Phase 04a: Pregap Preamble Read<br/>150 sectors before track start"]
    P --> E["Phase 04b: CRC Accumulate<br/>disc-absolute mul_by anchoring"]
    E --> G[Phase 05: Verify CRCv1/v2 vs DB]
    G --> H{Match Found?}
    H -- Yes --> I([VERIFIED: BIT-PERFECT])
    H -- No --> J{Retry Pass 2?}
    J -- Yes --> K["Flush Hardware Cache<br/>Reduce to 1x Speed"]
    K --> P
    J -- No --> L([ABORT: VERIFY FAIL])
    class A,B,D,O hardware;
    class C,G,H,J handshake;
    class P,E,K forensic;
```

## Acknowledgements
 
### Verification &amp; metadata services
 
- **[AccurateRip](https://www.accuraterip.com/)** (Spoon / Illustrate) — the AccurateRip
  verification database, used under non-commercial terms. Drive-offset table sourced
  from [accuraterip.com/driveoffsets.htm](https://www.accuraterip.com/driveoffsets.htm).
- **[CUETools Database (CTDB)](http://db.cuetools.net/)** — secondary rip verification
  via the CTDB lookup service.
- **[MusicBrainz](https://musicbrainz.org/)** — open music metadata database; DiscID
  lookup and text search.
- **[Cover Art Archive](https://coverartarchive.org/)** — release cover art, embedded
  into output files via TagLib.
- AccurateRip CRC and disc-identifier algorithms, and the offset-finding CRC technique, are documented by Spoon (Illustrate / dBpoweramp). RE-MOCT implements them independently.
### Algorithm references
 
- **[HydrogenAudio thread #97603](https://hydrogenaud.io/index.php?topic=97603)** — the
  canonical public description of the AccurateRip v1/v2 checksum; the primary source for
  the multiply-accumulate formula used here.
- **[Leo Bogert — accuraterip-checksum](https://github.com/leo-bogert/accuraterip-checksum)**
  (`accuraterip-checksum.c`, GPLv3) — a clean reference implementation of the per-track
  CRC formula and the first/last-track 5-sector skip; consulted to validate the
  accumulation against an independent implementation.
- **[whipper](https://github.com/whipper-team/whipper)** &amp;
  **[CUETools](http://cue.tools/wiki/CUETools)** — reference implementations consulted
  during AccurateRip research.
- **[Blue Book (CD-Extra / CD Plus)](https://en.wikipedia.org/wiki/Blue_Book_(CD_standard))**
  — the multi-session Enhanced-CD standard. Understanding the two-session layout
  (audio first, data second) was key to computing correct AccurateRip disc IDs for
  Enhanced discs such as Goo Goo Dolls — *Gutterflower*.
  
### Libraries
 
RE-MOCT links the following third-party libraries. See
[`THIRD_PARTY-NOTICES.md`](THIRD_PARTY-NOTICES.md) for full license details.
 
- **[libFLAC](https://xiph.org/flac/)** (Xiph.Org) — FLAC encoding. *BSD-3-Clause.*
- **[LAME](https://lame.sourceforge.io/)** (`libmp3lame`) — MP3 encoding. *LGPL-2.1.*
- **[TagLib](https://taglib.org/)** — audio metadata tagging. *LGPL-2.1 / MPL-1.1.*
- **[libebur128](https://github.com/jiixyj/libebur128)** — EBU R128 / ReplayGain
  loudness measurement. *MIT.*
- **[ncurses](https://invisible-island.net/ncurses/)** — terminal UI. *MIT-style (X11).*
- **[miniaudio](https://miniaud.io/)** — audio playback. *Public domain (Unlicense) or MIT-0.*
- **WinINet** (Microsoft Windows) — HTTP transport for database lookups. *Windows system API.*
---

![License: MIT](https://img.shields.io/badge/license-MIT-green)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Platform: Windows](https://img.shields.io/badge/platform-Windows-lightgrey)
![AccurateRip](https://img.shields.io/badge/AccurateRip-verified-gold)


