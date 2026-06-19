# RE-MOCT - Music On Console Terminal

> A homage to MOC - keyboard-driven terminal CD player and ripper for Windows.

📖 **[Documentation & Feature Guide](https://radmageirl.github.io/re-moct/)**

---

**Status:** Work in progress - no release yet. Polishing the Windows client, Linux port planned.

Built with C++20 · ncurses · miniaudio · TagLib · AccurateRip
-------------------------------------------------------------

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

## AccurateRip Pipeline (RE-MOCT Implementation)

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


