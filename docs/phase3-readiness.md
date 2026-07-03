# Phase 3 readiness survey — Linux port (2026-07-03)

Survey session only: NO port code written, nothing committed, tree untouched.
All experiments ran against shadow copies in WSL2 (`/root/remoct-survey`,
`/root/remoct-clean`). This doc = the readiness assessment + slicing proposal
for Dos to confirm before Phase 3's first slice.

## 0. State confirmed at session start
- Branch `restructure` clean, in sync with origin through `4a6a3c5` (handoff
  rev 3). Phases 0/1/2 DONE; slice C declined; all three cleanup items closed
  (VBR `adacbb1`, SET_SPEED `d2ad038`, desync analysis `2913a1b`).
- Pinned and honored throughout: the 150-sector preamble is physical (AR math
  untouched), live audio read loops sacred, DI endgame for the transitional
  global.

## 1. Environment stood up (this session)
- **WSL2 on 7of9** (WSL 2.7.10, kernel 6.18.33.2): installed the `Debian`
  distro — it IS **Trixie (13)**, exactly the decided target. GCC **14.2.0**,
  CMake 3.31.6, Ninja, pkg-config provisioned.
- Windows side for reference: MSYS2 UCRT64 GCC 15.2.0, taglib 2.2.1,
  ncurses 6.6. GCC 14 vs 15.2 is a comfortable C++20 pairing.

## 2. Does the portable core compile on Linux? — YES, with an enumerated fight list

**Method.** The tree turns out to be pervasively *whole-file* `#ifdef _WIN32`
gated: 11 TUs + 10 headers (StreamSource, UIManager consumers, CDRipper,
CDSource, MBLookup, RadioBrowser, LastFm, ListenBrainz, Log, IHeartRadio,
IHeartDeepLog, DiscordRP...) compile to *nothing* on Linux. So "does it
compile" is vacuously true and meaningless as-is — a naive Linux build
produces an empty shell, silently dropping radio, CD, scrobbling, the UI.
The survey therefore stripped every whole-file gate (nesting-matched, in a
shadow copy), commented out the `<windows.h>`-family includes so each Win32
*usage* errors individually, and ran `g++ -std=c++20 -fsyntax-only` per TU
against real Trixie headers (taglib 2.0.2, ncursesw 6.5, fdk-aac 2.0.3).

**Result: 12 of 23 TUs compile clean already**, and the failures classify
cleanly:

| Class | TUs | What fights | Effort |
|---|---|---|---|
| Clean today | AacDecoder, ar_crc, Config, CoverArt, IHeartNowPlayingSM, ListenBrainz, LocalFileSource, LrcData, **MBLookup**, Mp4Chapters, PlaylistManager, RadioBrowser | nothing | zero |
| Trivial | CDSource (3: `Sleep`), IHeartRadio (6: `GetTempPathA`/`localtime_s`), main (4: console-VT block + gated UIManager member), AudioManager (4: two `_WIN32`-gated members) | one-liners behind a tiny platform-util header | hours |
| Mechanical | Log (21), IHeartDeepLog (46): `FindFirstFile` enumeration, `GetLocalTime`, `%TEMP%`; CDRipper (60-cap): `SHGetKnownFolderPath(Music)`, `_wfopen`×~30, `WideCharToMultiByte`, `\\` path joins, `Sleep` | `std::filesystem` + `localtime_r` + UTF-8-native `fopen` + dir-policy mapping | a day-ish, wide but dumb |
| One real decision | LastFm (25): **CryptoAPI MD5** (`api_sig` signing) | needs an MD5 on Linux — see Decisions §7 | small once decided |
| Structural | **StreamSource** (60-cap): the raw WinINet ICY loop (`InternetOpenUrlA`/`InternetReadFile`), `HINTERNET` members in the header, `GetTickCount` timing | the ONE remaining WinINet site, by design — needs a Linux raw-transport twin, design-first (§7) | its own slice |
| Structural-lite | UIManager (60-cap), DiscordRP: mid-file `_WIN32`-gated members/blocks (`ShellExecuteA`, console handles, gated class body in DiscordRP.h L12–58, gated members in UIManager.h/AudioManager.h) | de-gate per subsystem as its Linux impl arrives; `GetCurrentProcessId`→`getpid` | mechanical once neighbors port |

**Notably absent from the fight list: ncurses.** Zero ncurses errors in
UIManager's 60 — the wide-API code compiles against Debian's ncursesw
unchanged (`__has_include(<ncursesw/ncurses.h>)` branch already present).

**CMake configures on Linux UNCHANGED** (exit 0): `find_library` falls
through the dead MSYS2 paths to system libs; TagLib found via pkg-config
(2.0.2); the `$<PLATFORM_ID:Windows>` link guards do their job. And the
**four pure test suites build and pass on Linux as-is**: `iheart_sm_test`,
`ar_crc_test`, `notify_toast_test`, `http_seam_test` — 4/4 green, zero
changes. The "pure suites are Linux-clean by design" claim is now a
demonstrated fact, which makes slice 0's CI gate meaningful on day one.

## 3. The two known-unknowns — both resolved GREEN
- **ncursesw / COLORS.** Probe (real initscr in a pty, Trixie ncursesw 6.5):
  `TERM=xterm-256color` → **COLORS=256, PAIRS=65536**; `TERM=xterm` → 8/64.
  The COLORS=8 ceiling is confirmed a Windows/MSYS2-terminfo artifact, not a
  code property. Wide API: `setcchar`+`mvwadd_wch`+`win_wch` round-trip 3/3
  (♪, CJK 音, ╭), `wcwidth` correct (1/2 cols). **The wide-glyph pipeline
  ports cleanly; Linux is strictly more capable.** (Parity note: pairs 1–14
  and the 8-color palette still work under 256 — no visual change required
  or wanted this phase.)
- **miniaudio / audio device.** Probe using the SAME vendored `lib/miniaudio.h`,
  remoct's exact device shape (f32/stereo/44100, data callback): backend
  **PulseAudio** came up inside WSL2 (WSLg "RDP Sink"), device init + start OK,
  **27,258 frames delivered in 0.6 s — the callback path is live**. On real
  Linux/the VM it'll be ALSA/Pulse natively. miniaudio's implementation TU
  also *parses* clean in AudioManager. Playback layer ports.

## 4. Dependencies on Trixie — all present, one flag
`libflac-dev 1.5.0` · `libmp3lame-dev 3.100` · `libebur128-dev 1.2.6` ·
`libfdk-aac-dev 2.0.3` ⚠ · `libncurses-dev 6.5` (ncursesw) · `libtag-dev
2.0.2` (TagLib 2.x, matches Windows 2.2.1 major) · `libcurl4-openssl-dev
8.14.1` · `libnotify-dev 0.8.6` + `libnotify-bin` · `sg3-utils 1.48` ·
`cdparanoia 3.10.2` · `libasound2-dev` / `libpulse-dev`.

⚠ **fdk-aac needs non-free enabled** (as suspected): `libfdk-aac-dev` lives in
Debian **non-free**; the stock WSL image ships `Components: main` only. Fixed
by adding `contrib non-free non-free-firmware` to the deb822 source (done in
the WSL instance; the CI container and any VM need the same one-liner).

## 5. CD gate path — a better option surfaced, verify before trusting
**Finding: the GHD3N is a USB drive** (`HL-DT-ST DVDRW GHD3N USB Device`,
G:). That changes the calculus:
- The WSL2 kernel has **`CONFIG_BLK_DEV_SR=y`, `CONFIG_CHR_DEV_SG=y`,
  `CONFIG_USB_STORAGE=m`, USBIP client=m — and all three modules load**
  (`modprobe usb-storage / sr_mod / sg` all OK, verified this session).
- So **usbipd-win → attach the GHD3N to WSL2 → real `/dev/sr0` + `/dev/sg0`
  with true SG_IO** is mechanically viable — no VM, same Trixie instance as
  the inner loop. This is genuine raw passthrough (USB/IP forwards the USB
  device itself; sr/sg sit on the real drive), not a virtual ISO.
- **VERIFIED LIVE (slice 0, 2026-07-03)** — usbipd-win 5.3.0 installed,
  GHD3N identified as busid 4-1 (JMicron 152d:0578 bridge; serial matched
  against the CDROM PnP path), bound + attached to WSL2: the drive enumerated
  as a real `scsi3-mmc` CD-ROM at `/dev/sr0` + `/dev/sg4` ("cdda tray").
  All probes passed: `sg_inq` INQUIRY (PDT=5 cd/dvd); `cdparanoia -Q` read
  the full TOC — **12 audio tracks, T1 begin LBA 32 = MSF frame 182 − the
  150 preamble: it's Relish, and the two addressing conventions reconcile
  exactly as the pinned framing says**; a raw `sg_raw` **READ CD 0xBE**
  (1 sector @ LBA 1000, 2352 bytes, no C2 — this drive's shape) returned
  SCSI Good with a **byte-identical re-read**. **The CD gate runs in WSL2;
  no VM.** The device stays bound ("Shared") — a gate session is just
  `usbipd attach --wsl --busid 4-1` (drive leaves Windows while attached;
  `usbipd detach --busid 4-1` returns it).
- **Fallback if usbipd disappoints:** a Debian Trixie VM on 7of9 with USB
  passthrough of the drive (VMware/VirtualBox forward the whole USB device —
  also real SG_IO; Hyper-V's DVD passthrough is NOT raw and is not
  acceptable for this gate). Distro alignment holds either way (§6).
- Caveat to note: rip speed over USB/IP has overhead; the gate cares about
  bytes, not minutes. And WSL2 device attach survives only until detach/
  reboot — attach is a per-gate-session ritual, fine for a gate.

## 6. CI matrix — proposal (and it comes FIRST)
- **Linux job: `ubuntu-latest` runner, steps inside `container:
  debian:trixie`.** This pins the CI userland to the SAME distro as the WSL2
  inner loop (and the VM, if one is ever needed) — a matched Trixie triple,
  immune to `ubuntu-latest` version drift. Steps: apt sources += non-free →
  install the §4 list → cmake+ninja → build pure tests → ctest.
- **Windows job: `windows-latest` + `msys2/setup-msys2`** (UCRT64, the
  package list from BUILD.md) → full build → ctest (13 today).
- **Ordering: CI lands in slice 0, before any seam code.** The survey proved
  the Linux job has a real gate from day one (4 pure tests green), and every
  subsequent slice grows it: seam slices port their `if(WIN32)` tests to the
  matrix as their impls arrive. This is the highest-leverage move — a
  Windows-ism leaking into core/ fails a PR the moment it appears, instead
  of at the next survey.
- Trigger: every push/PR on `restructure` (and `main` when merged). Release
  artifacts stay out of scope until parity is declared.

## 7. Decisions needed from Dos (design-first items, none blocking slice 0)
1. **LastFm MD5** (the `api_sig`): recommend **vendoring a single-file
   public-domain MD5 into `lib/`** and using it on BOTH platforms (CryptoAPI
   retired) — one code path, unit-testable offline against RFC 1321 vectors,
   zero new deps; live scrobble round-trip re-gates it per platform.
   Alternative: OpenSSL's MD5 (libcrypto arrives transitively with libcurl)
   — fewer vendored lines, but platform-split code paths remain.
2. **The ICY raw-loop Linux twin** (StreamSource's sacred read loop). The
   pinned rule keeps it OUT of IHttp on Windows permanently; the Linux
   sibling must preserve the same shape: blocking pull-reads feeding the
   ring, producer-owned handles, same backpressure/reconnect structure.
   Options to design-first in its slice (NOT now): (a) raw POSIX socket +
   hand-rolled GET/Icy-MetaData handshake — closest structural twin, but
   needs TLS for https:// stations; (b) libcurl in `CONNECT_ONLY` mode +
   `curl_easy_recv` — curl does TCP+TLS, WE keep the pull-read loop verbatim
   in `rawRead`; (c) curl multi/write-callback — rejected on shape (inverts
   to push, restructures the sacred loop). Current lean: (b).
3. **Directory policy mapping** (parity, not features): Music dir
   (`SHGetKnownFolderPath(FOLDERID_Music)` → `xdg-user-dir MUSIC` /
   `~/Music`), `%TEMP%` logs → `/tmp` or `$XDG_STATE_HOME`, config location
   on Linux. Small, but user-visible paths — worth one deliberate pass.
4. **CD gate venue**: ~~usbipd→WSL2 (pending the §5 live verification) vs VM
   fallback~~ — **RESOLVED on evidence (slice 0): usbipd→WSL2.** Real SG_IO
   INQUIRY/TOC/READ-CD-0xBE all verified against Relish in the GHD3N (§5);
   the VM fallback is documented but not needed.

## 8. Slicing proposal (firming up the sketch)
Dos's ordering confirmed — HTTP → IPC → notify → CD LAST — with two
survey-driven insertions: a "core compiles+links" slice before the seams
(the mechanical de-Win32 work is real and separable), and the ICY raw-loop
twin as its own design-first slice (it is NOT part of the HTTP seam, by the
pinned rule).

- **Slice 0 — CI matrix + env pin.** `.github/workflows/` matrix (§6); guard
  `MSYS2_PREFIX` under `if(WIN32)` (cosmetic; configure already works);
  document the WSL2 setup + non-free flag in BUILD.md. Side-quest: the
  usbipd CD-passthrough verification (§5) — evidence for the endgame gate,
  zero tree impact. **Gate: both matrix jobs green on a no-op push; 4/4
  pure tests on Linux in CI.**
- **Slice 1 — portable core compiles + links on Linux.** Introduce the tiny
  platform-util layer (sleep_ms, localtimeSafe already exists, temp/music/
  state dirs, UTF-8 fopen); do the Trivial+Mechanical classes (§2 table);
  MD5 per decision §7.1; de-gate the whole-file `_WIN32` wraps as each TU
  ports; four seams STUBBED in `src/platform/linux/` (compile-time no-ops);
  StreamSource's raw-loop internals `#ifdef`-split (header detox: `HINTERNET`
  members out of the platform-neutral part) but ICY stays Windows-only this
  slice. **Gate: `remoct` builds, launches, and PLAYS A LOCAL FILE on Linux**
  (survey says realistic: LocalFileSource/UI/audio all probe green) **+
  Windows byte-parity: ctest 13/13, short live gate, zero behavior change.**
- **Slice 2 — HTTP: libcurl `IHttp`** (+`IHttpSession` keep-alive, cancel
  token via `CURLOPT_XFERINFOFUNCTION` polling the same atomic,
  RedirectPolicy incl. SameScheme, `read_error`, no-cache header). Port the
  `if(WIN32)` request tests + build the POSIX twin of `http_cancel_test`
  (the cancel/keep-alive properties are impl properties — must be proven
  against curl, not assumed). **Gate on Linux: real MB lookup resolves
  Relish metadata; RadioBrowser search returns; a live scrobble round-trip
  lands on Last.fm/ListenBrainz; digital iHeart HLS resolves and PLAYS (the
  HLS path is fully seam-routed — it comes alive with this slice);
  hls_pipeline_test green in the matrix.**
- **Slice 3 — ICY raw-loop twin** (design-first pass per §7.2, correctness
  argument before code — this is live-audio-loop territory; producer/ring
  machinery zero-diff). **Gate: an ICY/SHOUTcast station plays on Linux,
  metadata updates, reconnect/backoff behaves; Windows StreamSource.cpp
  loop byte-diff empty.**
- **Slice 4 — IPC: Unix-domain-socket `IIpc`** (`$XDG_RUNTIME_DIR/<name>`,
  the mapping ICdIo-style already documented in IIpc.h). Discord itself may
  not run on the gate box, so: **gate = discord_ipc_test (FakeIpc, already
  pure) green on Linux + a live socat/echo-server probe exercising
  send/waitReadable/recvSome against a real Unix socket + documented note
  that full Discord RP remains Windows-verified** (revisit only if a Linux
  desktop with Discord materializes).
- **Slice 5 — notify: libnotify `INotify`** (`notify-send -a 'RE-MOCT'`
  semantics; two strings, fire-and-forget, best-effort). WSL2 has no
  notification daemon by default: **gate = a real notification rendered
  with `dunst` running in the WSL/WSLg session (or on any Linux desktop);
  headless = documented best-effort no-op — which IS the contract
  (baseline swallows failure).**
- **Slice 6 — CD: SG_IO `ICdIo` — LAST.** READ CD 0xBE with explicit C2
  bits (the `want_c2` flag becomes load-bearing exactly as slice 8
  designed), READ TOC 0x43 (TIME=1 MSF raw; format 01 session), TEST UNIT
  READY, INQUIRY, SET CD SPEED 0xBB — the one-to-one map in ICdIo.h.
  `got`-bytes surfaced from `sg_io_hdr` residuals. **Gate: Joan Osborne
  Relish on the GHD3N via usbipd/WSL2 (or VM fallback): 12/12 AR v2 conf
  200 AND byte-identical to the Windows baseline log — per-track
  crc_v1/crc_v2, all 12 pcm_crc32, frame450 global/local, T1 first-16
  dump, "C2 support: no" line. Conf-200 alone is NOT the bar; the
  byte-identical diff is what proves the SG_IO port didn't drift.**
  cd_toc/cd_pipeline tests (FakeCdIo, already injection-based) port to the
  matrix here.

Parity discipline throughout: no new features, no visual changes, no
Linux-native opportunities (those are noted post-parity work). Every slice
re-gates Windows (ctest + live spot-check) — the port must be a zero-diff
event for the Windows build.

## 9. What was NOT verified (honest limits)
- ~~usbipd attach → real SG_IO on the GHD3N~~ — closed at slice 0: verified
  live (§5). What remains for slice 6 itself: a full rip through the seam
  (sustained streaming reads, cache-flush behavior, rip DURATION over
  USB/IP), not the transport's existence.
- Rendering LOOK on a real Linux terminal (probe proved API + widths, not
  pixels) — eyeball at slice 1's gate.
- Link-level surprises past `-fsyntax-only` (member-gap errors capped some
  logs; residual small fights may surface at slice 1 link) — expected noise,
  same classes.
- WSLg PulseAudio ≠ bare ALSA: device bring-up on a non-WSL Linux box/VM
  still deserves its slice-1 listen check.
