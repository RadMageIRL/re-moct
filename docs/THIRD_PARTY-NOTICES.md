# Third-Party Notices

RE-MOCT incorporates or links the following third-party components. Each remains
under its own license; the texts referenced below are included with their respective
distributions.

| Component | Purpose | License |
|-----------|---------|---------|
| libFLAC (Xiph.Org) | FLAC encoding | BSD-3-Clause |
| libogg (Xiph.Org) | Ogg container (FLAC dependency) | BSD-3-Clause |
| LAME / libmp3lame | MP3 encoding | LGPL-2.1 |
| TagLib | Metadata tagging | LGPL-2.1 / MPL-1.1 (dual) |
| zlib | Compression (TagLib dependency) | zlib License |
| libebur128 | ReplayGain (EBU R128) | MIT |
| Fraunhofer FDK AAC | AAC / HE-AAC stream decoding | Fraunhofer FDK AAC Codec License (see note below) |
| ncurses | Terminal UI | MIT-style (X11) |
| miniaudio | Audio playback | Public domain (Unlicense) or MIT-0 |
| nlohmann/json | JSON parsing | MIT |
| WinINet / CryptoAPI / Shell (Win32) | HTTP transport, MD5 signing, browser launch | Microsoft Windows system APIs |

## Fraunhofer FDK AAC Codec License

RE-MOCT dynamically links and redistributes the Fraunhofer FDK AAC Codec Library
(`libfdk-aac-2.dll`) to decode AAC and HE-AAC internet-radio streams. This component
is **not** released under a standard FOSS license. It is provided under the
"Software License for The Fraunhofer FDK AAC Codec Library for Android"
(SPDX identifier: `FDK-AAC`). Redistribution is permitted, but the license imposes
specific obligations on anyone who distributes RE-MOCT with this DLL included:

- **Retain the full license text.** The complete text of the FDK AAC license must
  accompany binary redistributions. Include it in the release alongside this file
  (e.g. `licenses/FDK-AAC-LICENSE.txt`), copied verbatim from the library's own
  distribution.
- **Make the source available.** Recipients of the binary must be able to obtain the
  complete FDK AAC source code free of charge. RE-MOCT links an unmodified upstream
  build; the source is available at <https://github.com/mstorsjo/fdk-aac>.
- **No endorsement.** The Fraunhofer name may not be used to endorse or promote
  RE-MOCT without prior written permission.
- **No copyright license fees.** The library (and any modifications) may not be
  distributed for a copyright license fee. RE-MOCT is distributed free of charge, so
  this is satisfied; redistributors should keep it that way.

### Patent notice

The FDK AAC license **grants no patent rights, express or implied.** AAC is covered by
patents held by Fraunhofer and others; patent licenses for encoding or decoding
AAC-compliant bitstreams are administered separately (historically through Via
Licensing / Via-LA). This is independent of the copyright license above: being
permitted to copy the software is not the same as being permitted to practice the
patented codec. RE-MOCT uses the library for **decoding only**.

RE-MOCT's maintainers are not lawyers and this is not legal advice. Anyone
redistributing RE-MOCT with AAC support should evaluate whether AAC patent licensing
applies to their use and jurisdiction. If you prefer to avoid the question entirely,
RE-MOCT can be built without FDK AAC — only AAC/HE-AAC stream playback is lost; all
other functionality (CD, FLAC/MP3 files, MP3 streams) is unaffected.

## Reference material consulted

(Not redistributed.) Leo Bogert's `accuraterip-checksum` (GPLv3) was read as a
reference implementation. RE-MOCT's checksum code was written independently; no GPLv3
source was copied or adapted.

## Data services

The following are network services accessed at runtime, each under its own terms of
use. RE-MOCT transmits no audio and stores no third-party data beyond what is needed
to fulfill a request.

- **AccurateRip** — rip verification (used under non-commercial terms).
- **CUETools Database (CTDB)** — rip verification.
- **MusicBrainz** — disc and release metadata.
- **Discogs** — release metadata.
- **Cover Art Archive** — album artwork.
- **iTunes / Deezer** — album-art fallback lookup.
- **radio-browser.info** — internet-radio station directory and search.
- **Last.fm** — scrobbling and now-playing (used with the user's own API
  credentials and authorized session).
