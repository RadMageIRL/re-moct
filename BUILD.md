# Building RE-MOCT

RE-MOCT builds on **Windows** (MSYS2 UCRT64) and **Linux** (Debian Trixie) with
**CMake + Ninja**. The GitHub Actions matrix in [`.github/workflows/ci.yml`](.github/workflows/ci.yml)
- a `debian:trixie` container job and a `windows` MSYS2 UCRT64 job, both required green -
is the **canonical source of truth** for dependencies and build steps. If this file and
CI ever disagree, CI wins.

Reference environments:
- Windows 11 · MSYS2 UCRT64 · GCC 15.2
- Debian Trixie · GCC 14.2

The single-header dependencies **miniaudio** (`lib/miniaudio.h`) and **nlohmann/json**
(`lib/json.hpp`), plus the vendored public-domain MD5 (`lib/md5.{c,h}`), are checked into
the repo - no separate download step.

---

## Windows (MSYS2 UCRT64)

1. Install [MSYS2](https://www.msys2.org/) and open a **UCRT64** shell.
2. Install the toolchain and libraries:

   ```bash
   pacman -S --needed \
     mingw-w64-ucrt-x86_64-gcc \
     mingw-w64-ucrt-x86_64-cmake \
     mingw-w64-ucrt-x86_64-ninja \
     mingw-w64-ucrt-x86_64-pkgconf \
     mingw-w64-ucrt-x86_64-ncurses \
     mingw-w64-ucrt-x86_64-taglib \
     mingw-w64-ucrt-x86_64-flac \
     mingw-w64-ucrt-x86_64-lame \
     mingw-w64-ucrt-x86_64-libebur128 \
     mingw-w64-ucrt-x86_64-fdk-aac \
     mingw-w64-ucrt-x86_64-opus \
     mingw-w64-ucrt-x86_64-opusfile \
     mingw-w64-ucrt-x86_64-libopusenc \
     mingw-w64-ucrt-x86_64-wavpack \
     mingw-w64-ucrt-x86_64-libvorbis
   ```

3. Configure and build from the repo root:

   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

4. Run: `build/bin/remoct.exe`. The streaming plugin is built beside it at
   `build/bin/plugins/remoct_stream.dll`.

If you run the binary outside the UCRT64 shell, its runtime DLLs
(`libFLAC`, `libmp3lame`, `libebur128`, `libtag`, `libfdk-aac-2`, `libncursesw`, …) must
be reachable - either keep `C:\msys64\ucrt64\bin` on `PATH` or copy them next to
`remoct.exe`.

> **VSCode note:** IntelliSense may resolve headers against an unrelated project and
> report bogus diagnostics. The Ninja/GCC build is the only ground truth.

### Two Windows render backends

RE-MOCT renders on Windows two ways. Both build from the same source; pick one at
configure time:

- **ncursesw (default)** - the console build. Runs in any terminal (Windows Terminal,
  conhost); this is what the commands above produce. It keeps a ConPTY size-poll +
  repaint heartbeat so live resize works under Windows Terminal.
- **PDCursesMod wingui** (`-DREMOCT_PDCURSES=ON`) - draws the TUI in its own GDI
  window instead of a terminal, guaranteeing truecolor for Awesome mode. Adds an
  OS-matched dark title bar, process-private bundled-font loading (drop a `.ttf`/`.otf`
  in `<exeDir>/fonts/`), a remembered window size, and **Alt+Enter** borderless
  fullscreen. The wingui/GDI port is vendored (`lib/pdcursesmod/`) and compiled by
  CMake when the option is on - no extra packages needed.

  ```bash
  cmake -S . -B build-wingui -G Ninja -DCMAKE_BUILD_TYPE=Release -DREMOCT_PDCURSES=ON
  cmake --build build-wingui
  ```

  The `ncurses` package is still needed for the default build; the wingui build does
  not use it. Everything else (TagLib, FLAC, LAME, libebur128, FDK-AAC, opus/opusfile/
  opusenc, vorbis, wavpack) is common to both. The two configs use separate build
  directories, so you can keep both around.

**Fonts differ by backend.** The wingui build sets its own GDI font (config key
`wingui_font`, default a bundled JetBrains Mono Nerd Font); the ncursesw build - like
the Linux build - renders with the terminal emulator's font. See the README section
"Fonts and Nerd Font icons" for the per-platform details.

---

## Linux (Debian Trixie)

1. **Enable non-free** - `libfdk-aac-dev` lives in the `non-free` component. In the
   deb822 sources (`/etc/apt/sources.list.d/*.sources`), add `non-free` (and
   `non-free-firmware`) to the `Components:` line. The exact one-liner CI uses:

   ```bash
   sed -i 's/^Components: main.*/Components: main contrib non-free non-free-firmware/' \
     /etc/apt/sources.list.d/*.sources
   apt-get update
   ```

2. Install the toolchain and libraries:

   ```bash
   sudo apt-get install --no-install-recommends \
     build-essential g++ cmake ninja-build pkg-config git ca-certificates \
     libflac-dev libmp3lame-dev libebur128-dev libfdk-aac-dev \
     libopus-dev libopusfile-dev libopusenc-dev libwavpack-dev libvorbis-dev \
     libncurses-dev libtag-dev libcurl4-openssl-dev libnotify-dev \
     libasound2-dev libpulse-dev libsystemd-dev
   ```

   `libcurl` backs the HTTP seam; `libnotify` backs desktop notifications;
   `libasound2`/`libpulse` are miniaudio's runtime audio backends (loaded via `dlopen`);
   `libsystemd` (sd-bus) backs MPRIS OS media control. The opus/vorbis/wavpack
   `-dev` packages back the extra playback decoders and the Opus/WavPack rip and
   convert encoders.

3. Configure and build:

   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

4. Run: `build/bin/remoct` (plugin at `build/bin/plugins/remoct_stream.so`). A running
   PulseAudio/PipewWire (or ALSA) server is needed for audio; a notification daemon for
   toasts.

---

## Tests

```bash
ctest --test-dir build --output-on-failure
```

`xfade_handoff_test` self-skips (ctest `SKIP_RETURN_CODE 77`) when the machine has no
audio device - expected on headless CI. Some tests are Windows-only or Linux-only by
design (platform seam impls); the suite count differs per platform accordingly.

## CD ripping on Linux (note)

CD access uses SG_IO on `/dev/sr*` and needs read access to the device. Under WSL2 a
physical USB drive must be attached with `usbipd` - a virtual VM CD drive reports the
wrong read offset and will not match AccurateRip.
