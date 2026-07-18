# Third-Party Notices

RE-MOCT is licensed under the MIT License (see `LICENSE`). It incorporates, links,
or vendors the third-party components listed below. **Each component remains under
its own license and copyright.** When RE-MOCT is redistributed in binary form, the
license texts referenced here must accompany the distribution.

> **Maintainer note (not legal advice).** The RE-MOCT maintainer is not a lawyer.
> The three components that carry real redistribution obligations -
> **Fraunhofer FDK AAC**, **TagLib (LGPL)**, and **LAME / libmp3lame (LGPL)** - are
> flagged with a ⚠ below and expanded in [§ Redistribution obligations](#redistribution-obligations).
> Read those before shipping binaries.

## Summary table

| Component | Purpose in RE-MOCT | How linked | License | Upstream |
|-----------|--------------------|------------|---------|----------|
| **miniaudio** | Audio device I/O, decode/convert, resample | Vendored header (`lib/miniaudio.h`) | Public domain (Unlicense) **or** MIT-0 (dual, your choice) | https://github.com/mackron/miniaudio |
| **nlohmann/json** | JSON parsing (metadata, service APIs) | Vendored header (`lib/json.hpp`) | MIT | https://github.com/nlohmann/json |
| **Openwall MD5** (Solar Designer) | `api_sig` signing for Last.fm | Vendored C (`lib/md5.c`, `lib/md5.h`), byte-verbatim | Public domain | https://openwall.info/wiki/people/solar/software/public-domain-source-code-md5 |
| **libFLAC** (Xiph.Org) | FLAC encode (CD ripping) | Dynamic link | BSD-3-Clause | https://xiph.org/flac/ |
| **libogg** (Xiph.Org) | Ogg container (libFLAC + libopusfile dependency) | Transitive dynamic link | BSD-3-Clause | https://xiph.org/ogg/ |
| **libopus** (Xiph.Org) | Opus decode (`.opus` playback) | Dynamic link | BSD-3-Clause | https://opus-codec.org/ |
| **libopusfile** (Xiph.Org) | Ogg Opus demux/seek over libopus | Dynamic link | BSD-3-Clause | https://opus-codec.org/ |
| **libvorbis / libvorbisfile** (Xiph.Org) | Ogg Vorbis decode (`.ogg` playback) | Dynamic link | BSD-3-Clause | https://xiph.org/vorbis/ |
| **libopusenc** (Xiph.Org) | Opus encode (CD ripping) | Dynamic link | BSD-3-Clause | https://opus-codec.org/ |
| **WavPack (libwavpack)** | WavPack decode (`.wv` playback) | Dynamic link | BSD-3-Clause | https://www.wavpack.com/ |
| ⚠ **LAME / libmp3lame** | MP3 encode (CD ripping) | Dynamic link | LGPL-2.1-or-later | https://lame.sourceforge.io/ |
| ⚠ **TagLib** | Read/write audio tags | Dynamic link | LGPL-2.1-only **or** MPL-1.1 (dual) | https://taglib.org/ |
| **zlib** | Compression (TagLib dependency) | Transitive dynamic link | Zlib License | https://zlib.net/ |
| **libebur128** | EBU R128 ReplayGain measurement | Dynamic link | MIT | https://github.com/jiixyj/libebur128 |
| ⚠ **Fraunhofer FDK AAC** | AAC / HE-AAC decode (files + internet radio) | Dynamic link | Fraunhofer FDK AAC Codec Library License (SPDX `FDK-AAC`) | https://github.com/mstorsjo/fdk-aac |
| **ncurses (ncursesw / panelw)** | Terminal UI | Dynamic link | X11-style (MIT-family) | https://invisible-island.net/ncurses/ |
| **libcurl** | HTTP transport on Linux (`core::IHttp`) | Dynamic link (Linux only) | curl License (MIT/X-derivative) | https://curl.se/ |
| **Win32 system libraries** (WinINet, CryptoAPI, Shell, winmm, ws2_32, ole32, advapi32) | HTTP transport, browser launch, timing, sockets, IPC (Windows only) | Dynamic link (Windows only) | Microsoft Windows system APIs - no separate notice required | - |

Per-component required notice text is in [§ Required attributions](#required-attributions).
Network services used at runtime (no code redistributed) are in [§ Data services](#data-services).

---

## Redistribution obligations

### ⚠ Fraunhofer FDK AAC - SPDX `FDK-AAC`

RE-MOCT dynamically links and (in a binary release) redistributes the Fraunhofer FDK
AAC Codec Library (`libfdk-aac-2.dll` / `libfdk-aac.so`) to decode AAC and HE-AAC -
used both for AAC audio files and for internet-radio streams. This component is **not**
released under a standard FOSS license; it is provided under the *"Software License for
The Fraunhofer FDK AAC Codec Library for Android."* Anyone distributing RE-MOCT with
this library must:

1. **Retain the full license text**, copied verbatim from the library's own
   distribution, alongside the binaries (e.g. `licenses/FDK-AAC-LICENSE.txt`).
2. **Make the source available** - recipients must be able to obtain the complete FDK
   AAC source free of charge. RE-MOCT links an unmodified upstream build; source:
   https://github.com/mstorsjo/fdk-aac
3. **No endorsement** - the Fraunhofer name may not be used to promote RE-MOCT without
   prior written permission.
4. **No copyright license fee** - the library (and modifications) may not be
   distributed for a copyright license fee. RE-MOCT is distributed free of charge.

**Patent notice (separate from copyright).** The FDK AAC license **grants no patent
rights, express or implied.** AAC is patent-encumbered; encode/decode patent licensing
is administered separately (historically Via Licensing / Via-LA) and is independent of
the copyright permission to copy the software. RE-MOCT uses the library for **decoding
only**. Redistributors should evaluate whether AAC patent licensing applies to their
use and jurisdiction. RE-MOCT can be built **without** FDK AAC - only AAC/HE-AAC
playback is lost; CD, FLAC/MP3 files, and MP3 streams are unaffected.

> **VERIFY before shipping binaries:** copy the exact upstream `NOTICE` / license file
> from the FDK-AAC build you link and confirm the four obligations above against it.

### ⚠ TagLib - LGPL-2.1-only OR MPL-1.1 (dual)

RE-MOCT dynamically links TagLib. Under the LGPL branch, a binary distribution must:

- Give recipients the LGPL-2.1 license text and TagLib's copyright notice.
- Allow the user to **replace the TagLib library** and re-run RE-MOCT with their
  version. Dynamic linking against a system/redistributed shared library satisfies
  this in practice; **static linking would add relinking-material obligations** - RE-MOCT
  links dynamically, so keep it that way.
- Provide a way to obtain TagLib's source (a written offer or a link to the exact
  upstream version suffices).

The MPL-1.1 alternative is file-level copyleft on TagLib's own files only; either
branch is satisfied by shipping the shared library unmodified with its notices.

### ⚠ LAME / libmp3lame - LGPL-2.1-or-later

Same shape as TagLib: dynamically linked, so shipping `libmp3lame` as an unmodified
shared library with the LGPL-2.1 text + copyright notice, plus a pointer to upstream
source (https://lame.sourceforge.io/), satisfies the obligation. Do not statically
link it into `remoct` without adding the relinking-material provisions.

> **Note on patents:** MP3 decode/encode patents have expired worldwide (2017), so
> unlike AAC there is no live patent question for LAME.

---

## Required attributions

Verbatim copyright / permission notices that must be reproduced in binary
redistributions:

**libFLAC / libogg (Xiph.Org) - BSD-3-Clause**
> Copyright (C) 2000-2009 Josh Coalson, 2011-2023 Xiph.Org Foundation.
> Redistribution and use in source and binary forms, with or without modification, are
> permitted provided that the BSD-3-Clause conditions (retain copyright notice, this
> list of conditions, and the disclaimer; no endorsement) are met. THE SOFTWARE IS
> PROVIDED "AS IS".

**libopus / libopusfile / libopusenc (Xiph.Org) - BSD-3-Clause**
> Copyright (c) 1994-2013 Xiph.Org Foundation and contributors. Redistribution and use
> in source and binary forms, with or without modification, are permitted provided that
> the BSD-3-Clause conditions (retain copyright notice, this list of conditions, and the
> disclaimer; no endorsement) are met. THE SOFTWARE IS PROVIDED "AS IS".

**libvorbis / libvorbisfile (Xiph.Org) - BSD-3-Clause**
> Copyright (c) 2002-2020 Xiph.Org Foundation. Redistribution and use in source and
> binary forms, with or without modification, are permitted provided that the
> BSD-3-Clause conditions are met. THE SOFTWARE IS PROVIDED "AS IS".

**WavPack (libwavpack) - BSD-3-Clause**
> Copyright (c) 1998-2025 David Bryant. All rights reserved. Redistribution and use in
> source and binary forms, with or without modification, are permitted provided that the
> BSD-3-Clause conditions are met. THE SOFTWARE IS PROVIDED "AS IS".

**libebur128 - MIT**
> Copyright (c) 2011 Jan Kokemüller. Permission is granted, free of charge, … "AS IS".

**nlohmann/json - MIT**
> Copyright (c) 2013-2025 Niels Lohmann <https://nlohmann.me>. MIT License.

**miniaudio - public domain (Unlicense) or MIT-0**
> miniaudio. Copyright 2023 David Reid. Dual-licensed; choose either the Unlicense
> (public domain) or MIT-0. No attribution strictly required, but the copyright line is
> reproduced here as a courtesy.

**Openwall MD5 - public domain**
> This software was written by Alexander Peslyak (Solar Designer) in 2001 and placed in
> the public domain. There's absolutely no warranty. (See the header of `lib/md5.c`.)

**ncurses - X11-style**
> Copyright (c) 1998-2023 Free Software Foundation, Inc. Permission is hereby granted,
> free of charge … MIT/X11-style; the copyright and permission notice must be retained.

**libcurl (Linux) - curl License**
> Copyright (c) 1996-2025 Daniel Stenberg, and many contributors. Permission to use,
> copy, modify, and distribute … (MIT/X derivative). The copyright notice must appear in
> supporting documentation.

**zlib - Zlib License**
> Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler. Provided "as-is"; the origin
> must not be misrepresented and altered versions must be marked.

> For each library above, the **canonical** notice is the license file shipped in that
> library's own distribution (MSYS2 package on Windows, distro package on Linux). The
> lines here are convenience summaries; a binary release should bundle the exact upstream
> `COPYING` / `LICENSE` files under `licenses/`.

---

## Reference material consulted (not redistributed)

Leo Bogert's `accuraterip-checksum` (GPLv3) was **read as a reference** for the
AccurateRip CRC algorithm. RE-MOCT's checksum code was written independently against the
public algorithm description; no GPLv3 source was copied or adapted, so no GPL
obligation attaches.

## Data services

Network services accessed at runtime, each under its own terms. RE-MOCT transmits no
audio and stores no third-party data beyond what a request requires.

- **AccurateRip** - rip verification (non-commercial terms).
- **CUETools Database (CTDB)** - rip verification.
- **MusicBrainz** - disc / release metadata.
- **Discogs** - release metadata.
- **Cover Art Archive** - album artwork.
- **iTunes / Deezer** - album-art fallback lookup.
- **radio-browser.info** - internet-radio station directory / search.
- **iHeartRadio** - station resolution + HLS streaming + now-playing metadata.
- **Last.fm** / **ListenBrainz** - scrobbling and now-playing (user's own credentials).
- **Discord** - Rich Presence (local named-pipe / Unix-socket IPC to the desktop client).
