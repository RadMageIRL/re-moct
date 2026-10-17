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
| **PDCursesMod** (Bill Gray) | Terminal UI on Windows (the wingui/GDI port) | Vendored C (`lib/pdcursesmod/`), compiled in (Windows only) | Public domain | https://github.com/Bill-Gray/PDCursesMod |
| **libFLAC** (Xiph.Org) | FLAC encode (CD ripping) | Static on Windows, dynamic on Linux | BSD-3-Clause | https://xiph.org/flac/ |
| **libogg** (Xiph.Org) | Ogg container (libFLAC + libopusfile dependency) | Static on Windows, dynamic on Linux (transitive) | BSD-3-Clause | https://xiph.org/ogg/ |
| **libopus** (Xiph.Org) | Opus decode (`.opus` playback) | Static on Windows, dynamic on Linux | BSD-3-Clause | https://opus-codec.org/ |
| **libopusfile** (Xiph.Org) | Ogg Opus demux/seek over libopus | Static on Windows, dynamic on Linux | BSD-3-Clause | https://opus-codec.org/ |
| **libvorbis / libvorbisfile** (Xiph.Org) | Ogg Vorbis decode (`.ogg` playback) | Static on Windows, dynamic on Linux | BSD-3-Clause | https://xiph.org/vorbis/ |
| **libopusenc** (Xiph.Org) | Opus encode (CD ripping) | Static on Windows, dynamic on Linux | BSD-3-Clause | https://opus-codec.org/ |
| **WavPack (libwavpack)** | WavPack decode (`.wv` playback) | Static on Windows, dynamic on Linux | BSD-3-Clause | https://www.wavpack.com/ |
| ⚠ **LAME / libmp3lame** | MP3 encode (CD ripping) | Static on Windows, dynamic on Linux | LGPL-2.0-or-later | https://lame.sourceforge.io/ |
| ⚠ **TagLib** | Read/write audio tags | Static on Windows, dynamic on Linux | LGPL-2.1-only **or** MPL-1.1 (dual) | https://taglib.org/ |
| **zlib** | Compression (TagLib dependency) | Static on Windows, dynamic on Linux (transitive) | Zlib License | https://zlib.net/ |
| **libebur128** | EBU R128 ReplayGain measurement | Dynamic link | MIT | https://github.com/jiixyj/libebur128 |
| ⚠ **Fraunhofer FDK AAC** | AAC / HE-AAC decode (files + internet radio), AAC-LC encode (M4A rip and recording) | Dynamic link (one copy, shared by the player and the streaming plugin) | Fraunhofer FDK AAC Codec Library License (SPDX `FDK-AAC`) | https://github.com/mstorsjo/fdk-aac |
| **ncurses (ncursesw / panelw)** | Terminal UI on Linux | Dynamic link (Linux only) | X11-style (MIT-family) | https://invisible-island.net/ncurses/ |
| **GCC runtime** (libstdc++, libgcc) | C++ standard library and compiler support | Static on Windows, dynamic on Linux | GPL-3.0-or-later with the GCC Runtime Library Exception 3.1 | https://gcc.gnu.org/ |
| **mingw-w64 winpthreads** | POSIX threads on Windows | Static (Windows only) | MIT-style, with BSD-3-Clause portions | https://www.mingw-w64.org/ |
| **libcurl** | HTTP transport on Linux (`core::IHttp`) | Dynamic link (Linux only) | curl License (MIT/X-derivative) | https://curl.se/ |
| **Win32 system libraries** (WinINet, CryptoAPI, Shell, winmm, ws2_32, ole32, advapi32) | HTTP transport, browser launch, timing, sockets, IPC (Windows only) | Dynamic link (Windows only) | Microsoft Windows system APIs - no separate notice required | - |

**Windows and Linux link differently.** The Windows release is one static executable:
every component marked "static on Windows" is compiled into `remoct.exe`, and the only
DLLs it loads besides Windows' own are `libfdk-aac-2.dll` and `libebur128.dll`. On Linux
every library is the distribution's shared library.

Per-component required notice text is in [§ Required attributions](#required-attributions).
Network services used at runtime (no code redistributed) are in [§ Data services](#data-services).

---

## Redistribution obligations

### ⚠ Fraunhofer FDK AAC - SPDX `FDK-AAC`

RE-MOCT dynamically links and (in a binary release) redistributes the Fraunhofer FDK
AAC Codec Library (`libfdk-aac-2.dll` / `libfdk-aac.so`) to decode AAC and HE-AAC -
used both for AAC audio files and for internet-radio streams - and to encode AAC-LC for
M4A rip and stream-recording output. This component is **not**
released under a standard FOSS license; it is provided under the *"Software License for
The Fraunhofer FDK AAC Codec Library for Android."* Anyone distributing RE-MOCT with
this library must:

1. **Retain the full license text**, copied verbatim from the library's own
   distribution, alongside the binaries (the Windows release ships it as
   `licenses/FDK-AAC-LICENSE`).
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
the copyright permission to copy the software. RE-MOCT uses the library to **decode**
AAC and HE-AAC and to **encode** AAC-LC (M4A output only; every other rip and recording
format is unaffected). Redistributors should evaluate whether AAC patent licensing
applies to their use and jurisdiction. FDK AAC is a required dependency of the current
build; there is no configure option that leaves it out.

> **VERIFY before shipping binaries:** copy the exact upstream `NOTICE` / license file
> from the FDK-AAC build you link and confirm the four obligations above against it.
> For 1.6.3, `licenses/FDK-AAC-LICENSE` is byte-identical to the file the MSYS2
> fdk-aac 2.0.3 package installs.

### ⚠ TagLib - LGPL-2.1-only OR MPL-1.1 (dual)

On Linux RE-MOCT links the system's shared TagLib. The Windows release links TagLib
**statically** into `remoct.exe`. Under the LGPL branch, a binary distribution must:

- Give recipients the LGPL-2.1 license text and TagLib's copyright notice. The Windows
  release ships the text as `licenses/GNU LGPL.txt`; the notice is in
  [§ Required attributions](#required-attributions).
- Allow the user to **replace the TagLib library** and relink RE-MOCT against their
  version. A shared library allows this by itself. For the static Windows build it is met
  through source: RE-MOCT's complete source, build files included, is public under MIT
  at every release tag, the release page that offers the zip offers that source too, and
  [BUILD.md](BUILD.md) gives the exact configure line the release is built with. Anyone
  can rebuild `remoct.exe` against a TagLib of their own.
- Provide a way to obtain TagLib's source (a written offer or a link to the exact
  upstream version suffices). The Windows release links the MSYS2 UCRT64 package of
  TagLib 2.2.1, unmodified.

The MPL-1.1 alternative is file-level copyleft on TagLib's own files only, so it allows
static linking as long as TagLib's source stays available.

### ⚠ LAME / libmp3lame - LGPL-2.0-or-later

LAME's headers grant the GNU Library General Public License version 2 "or (at your
option) any later version", so the LGPL-2.1 text shipped for TagLib covers it too.
Same shape as TagLib otherwise: a shared library on Linux, linked statically into
`remoct.exe` on Windows, with relinking met the same way, through RE-MOCT's public
source and the release's configure line. The copyright notice is in
[§ Required attributions](#required-attributions), and the source is upstream
(https://lame.sourceforge.io/); the Windows release links the MSYS2 UCRT64 package of
LAME 3.100, unmodified.

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

**TagLib - LGPL-2.1-only or MPL-1.1**
> Copyright (C) 2002-2008 Scott Wheeler, and the TagLib contributors. TagLib is free
> software; you can redistribute it and/or modify it under the terms of the GNU Lesser
> General Public License version 2.1, or the Mozilla Public License version 1.1. It is
> distributed WITHOUT ANY WARRANTY. Full LGPL-2.1 text: `licenses/GNU LGPL.txt`.

**LAME - LGPL-2.0-or-later**
> Copyright (c) 1999 Mark Taylor, and the LAME developers. This library is free software;
> you can redistribute it and/or modify it under the terms of the GNU Library General
> Public License, version 2 or (at your option) any later version. It is distributed
> WITHOUT ANY WARRANTY.

**PDCursesMod - public domain**
> The vendored PDCursesMod files (the portable core and the wingui port) are released to
> the public domain; see the "Distribution Status" sections in `lib/pdcursesmod/`. None of
> upstream's GPL build scripts were vendored. RE-MOCT's local patches are listed in
> `lib/pdcursesmod/VENDOR.md`.

**GCC runtime - GPL-3.0-or-later with the GCC Runtime Library Exception 3.1**
> The exception lets a program compiled with GCC carry the runtime under the program's
> own license (RE-MOCT's is MIT). No notice is required; this line is a courtesy.

**mingw-w64 winpthreads (Windows) - MIT-style, with BSD-3-Clause portions**
> Copyright (c) 2011 mingw-w64 project; portions (C) 2010 Lockless Inc. The copyright and
> permission notices must be included in all copies; the full text ships as
> `licenses/winpthreads-COPYING`.

**ncurses (Linux) - X11-style**
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
>
> The Windows release's `licenses/` folder carries the files the MSYS2 packages install
> for FDK AAC, FLAC, libogg, libopus, libopusfile, WavPack, zlib, libebur128, ncurses,
> winpthreads and the GCC runtime, the LGPL-2.1 text for TagLib and LAME, and RE-MOCT's
> own MIT license. MSYS2 installs no license file for libvorbis or libopusenc; their
> BSD-3-Clause notices are the ones above.

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
