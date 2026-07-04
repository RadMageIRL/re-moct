# Session handoff — 2026-07-04 (Phase 4 kickoff: slices (a) + (b) DONE, live-gated both platforms; next = slice (c))

Read `CLAUDE.md` + `roadmap.md` + `lessons.md` + `architecture.md` +
`docs/phase4-readiness.md` + `docs/phase4-slice-b-design.md` + THIS file first.

## Where we are
Phase 3 COMPLETE (every platform call behind a seam, both platforms). Phase 4
(plugin-ize) IN PROGRESS. The ABI is ratified and the host now drives the streaming
source through it — StreamSource still compiled IN as an *in-process* plugin. Nothing
is on a `.so/.dll` yet; that's slice (c).

## Pinned (do NOT re-litigate)
- **The C ABI (`include/core/remoct_plugin.h`) is FROZEN v1.** One export
  `remoct_plugin_query` → POD `RemoctPlugin` op table + injected `RemoctHostServices`.
  C linkage only, noexcept everywhere, nobody frees across the boundary, major-version
  gate + struct-size additive growth. Boundary A: the first plugin is the STREAMING
  SOURCE entire (StreamSource + iHeart metadata + ICY), iHeart its headline.
- **The cancel token is `int32_t` end to end** (`HttpRequest::cancel` = `const int32_t*`,
  read via `std::atomic_ref`). Do NOT reintroduce a `std::atomic<bool>*` cancel or mix
  `std::atomic` with `atomic_ref` on one object (non-conforming — lessons.md).
- The 150-sector preamble, the live StreamSource read loop, `next_decoder_initialised_`,
  the retirement scheme — all still sacred, unchanged by Phase 4.

## Slice (a) — freeze the C ABI (DONE)
`remoct_plugin.h` (frozen v1) + the 5th platform seam `core::IPluginLoader` (
`PluginLoaderWin`/`PluginLoaderPosix`) + consumer-side policy `PluginHost`
(`validatePlugin`/`loadPlugin`, `LoadedPlugin` RAII). Proven by a pure-C sine plugin
(`tests/plugin_sine.c`) + `plugin_loader_test` (lifecycle + version-gate reject paths),
both platforms. Loader NOT in the remoct target yet (test-only + `remoct_linux_seams`
OBJECT check) — the host consumer arrived in (b), still in-process.

## Slice (b) — host drives the source through the ABI (DONE, live-gated)
Three parts:
1. **Cancel unified on int32.** `HttpRequest::cancel` → `const int32_t*`; shared
   `core::httpCancelRequested()` (atomic_ref acquire-load); both transports call it.
   `StreamSource::stop_` stays `atomic<bool>`; a plain `int32_t http_cancel_` written
   only via `setStop()` at stop_'s two write sites, passed as `&http_cancel_` (zero
   reinterpret). Gate: abort Win 393 ms / Linux 302 ms.
2. **HTTP shim** `PluginHostServices` (`core::HostServices(IHttp&)`): sessions wrap
   `IHttpSession`; `http_fetch` translates req/resp and forwards the `int32_t*` cancel
   verbatim; response memory host-malloc'd, freed by `http_response_free`; noexcept.
3. **Adapter + driver + rewire.** `StreamPluginAdapter` (`remoct_stream_plugin_query()`
   → RemoctPlugin table 1:1 over StreamSource); `core::PluginSource` (host driver; same
   method names; `url()`/`paused()` host-tracked, NOT ABI surface). AudioManager's
   `StreamSource stream_source_` → `HostServices host_services_{}` + `PluginSource
   stream_plugin_{...}` (in-class init; host_services_ first for init order). Audio-thread
   delta = one set-once indirect `readFrames` call; ring/producer/prebuffered_ unchanged.

Tests: `plugin_http_shim_test`, `plugin_stream_test` (both platforms). **Windows ctest
19/19; Linux plugin_* + AudioManager.cpp clean.** LIVE both platforms: Linux WSL2 pty —
SomaFM ICY through `PluginSource`, RDPSink.monitor RMS 4488.9 / 100% non-zero, nowPlaying
+ [LIVE] across the ABI (iHeart HLS DNS-dead in WSL — env quirk); Windows — Dos confirmed
digital iHeart + ICY play.

## As-built note flagged & blessed (design §5a)
Dos directed "stop_ → atomic<int32_t>"; implemented the conforming way instead (plain
int32 + atomic_ref, stop_ untouched) because the literal retype mixes atomic/atomic_ref
= UB. Blessed as built.

## NEXT — slice (c): extract the stream stack as a real .so/.dll
The four moves (the (b)→(c) boundary is a single line — the acquisition flip):
1. Move `StreamSource` + `IHeartRadio` + `IHeartNowPlayingSM` + `IHeartDeepLog` +
   `StreamPluginAdapter` into `plugins/stream/`, built as a shared library (MODULE),
   exporting `remoct_plugin_query` (the adapter's descriptor gains the export). Monorepo
   (`plugins/stream/`) while the ABI churns; one CI, atomic host+plugin commits.
2. Flip acquisition: `remoct_stream_plugin_query()` (direct) → `core::loadPlugin(<path>)`
   (dlopen/LoadLibrary), loaded from a known dir next to `remoct` (decide the path — e.g.
   `plugins/` beside the binary). `PluginSource` driving code is UNCHANGED.
3. **The rewire that matters most: iHeart/HLS HTTP `core::http()` → the injected host
   services** (`StreamSource.cpp:378` + `IHeartRadio.cpp:58`) — drop every `core::http()`
   in the plugin; the shim (`PluginHostServices`, built + unit-proven in (b)) gets its
   real in-plugin consumer. The plugin stops depending on host globals.
4. Move FDK-AAC + miniaudio-MP3 link deps into the plugin; the host streaming branch
   stops carrying FDK-AAC.
Design-first as always; then slice (d): `hls_pipeline_test` THROUGH the boundary
(byte-identical PCM) + the live parity battery.

## Working copy / environment
- **We work out of `E:\code\remoct`** — the git clone of `github.com/RadMageIRL/re-moct`
  on the **`restructure`** branch. All Phase 4 (and prior) work lives here. It is nested
  inside an unrelated remote-less junk repo at `E:\code` (`remoct/` is gitignored there,
  so the nesting is harmless).
- **`E:\code\digi` is an OLD, unrelated project — ignore it.** VSCode IntelliSense wrongly
  resolves `E:\code\remoct` headers against `E:\code\digi`, so its C++ diagnostics on
  remoct files are BOGUS (they cite digi paths / "undefined" symbols that exist here). The
  Ninja/GCC build is the ONLY ground truth for this repo.
- This clone builds + ctests locally: **Windows** — `/c/msys64/ucrt64/bin` on PATH,
  `cmake -S /e/code/remoct -B /e/code/remoct/build -G Ninja && cmake --build build &&
  ctest --test-dir build`. **Linux** — WSL2 Debian, source copied to `/home/dostrom/rb`,
  `PULSE_SERVER=unix:/mnt/wslg/PulseServer` for the audio gate (capture `RDPSink.monitor`
  via `parec`). Note the Bash-tool cwd can silently reset to `E:\code` (the outer junk
  repo) — use absolute paths / `git -C /e/code/remoct` in git one-liners.

## Operating discipline (unchanged)
Design-first; correctness argument before audio-thread changes; probe-first. Verify →
build → ctest → real-world gate → commit (code and docs separately, no Co-Authored-By).
