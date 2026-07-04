# Session handoff — 2026-07-04 (PHASE 4 COMPLETE: iHeart is a real loadable plugin)

Read `CLAUDE.md` + `roadmap.md` (Done + Parked) + `architecture.md` + `lessons.md` +
`docs/phase4-readiness.md` + `docs/phase4-slice-b-design.md` + `docs/phase4-slice-c-design.md`
+ THIS file first.

## Where we are
**Phases 0–4 COMPLETE. The restructure branch is feature-complete.** The streaming source
(StreamSource + iHeart metadata + ICY) is a real loadable `remoct_stream.{so,dll}`, driven by
the host purely through the frozen C ABI, with its HTTP injected across the service table.
**"Fix iHeart and ship without rebuilding the host" is literally true** — and *identical to
compiled-in* is a deterministic byte-identity ctest on both matrix legs, not a claim.

**Merge to `dev`/`main` is Dos's call — nothing has been merged.**

## Phase 4 as-built (the plugin architecture)
- **The ABI (`include/core/remoct_plugin.h`, frozen v1):** one export `remoct_plugin_query`
  → POD `RemoctPlugin` op table + injected `RemoctHostServices`. C linkage, noexcept, nobody
  frees across the line, major-gate + struct-size additive growth. Cancel token is `int32_t`
  end to end.
- **The loader is the 5th platform seam:** `core::IPluginLoader` + `PluginLoaderWin`
  (LoadLibrary) / `PluginLoaderPosix` (dlopen `RTLD_LOCAL`); policy (`validatePlugin` /
  `loadPlugin` / `LoadedPlugin` RAII) is consumer-side in `PluginHost`.
- **The plugin (`plugins/stream/`, a MODULE lib):** StreamSource + IHeartRadio +
  IHeartNowPlayingSM + IHeartDeepLog + StreamPluginAdapter, plus its OWN miniaudio impl
  (`MA_NO_DEVICE_IO` — it never opens a device; the host owns it) + FDK-AAC + the SACRED raw
  ICY transport (`-lwininet` / `libcurl` — StreamSource's private read loop, NOT the seam).
  Exports `remoct_plugin_query`; keeps `remoct_stream_plugin_query()` as the compiled-in
  byte-identity reference.
- **HTTP crossing:** host `PluginHostServices` fills a `RemoctHostServices` table from
  `core::IHttp`; the plugin's `HostServiceHttp` is that shim INVERTED (a `core::IHttp` over
  the injected table), so StreamSource/IHeartRadio's `->openSession/fetch` is unchanged. The
  plugin reaches HTTP ONLY through the injected table — `grep core::http() plugins/stream/
  = 0`. The `core::IHttp` seam transport (WinINet/libcurl) stays host-side.
- **Acquisition:** `AudioManager` loads `port::exeDir()/plugins/remoct_stream.{so,dll}` via
  `core::loadPlugin`; a bad load → `PluginSource::valid()==false` → streaming disabled, host
  alive (the slice-a reject paths, now production).
- **Deep-log toggle (Ctrl+A)** crosses via `set_config("deeplog",…)` + host-tracked state
  (IHeartDeepLog moved into the `.so`).

## Slices (all DONE & PUSHED)
- **(a)** freeze the ABI vs a disposable pure-C sine plugin — `plugin_loader_test`.
- **(b)** host drives the source through the ABI (compiled-in) + the HTTP shim; cancel unified
  on `int32_t`.
- **(c)** extract the stack to the real `.so/.dll`; acquisition flip; the `core::http()`→
  injected-services rewire; live-gated both platforms + a remove-the-`.so`→silent negative
  control (code `6401e4f`, docs `b486fe4`).
- **(d)** the identical-to-compiled-in gate — `tests/plugin_hls_parity_test.cpp`: byte-identical
  PCM between the compiled-in reference and the loaded module, through the real host HTTP
  crossing; `refA==refB` (determinism, in-test) + `refA==loaded` (thesis) + `rms>0.15` +
  `segment_gets>0`, both platforms (code `c581f70`). See lessons.md for the reusable recipe.

## Verification (both platforms, run by Claude)
Windows ctest 20/20, Linux 21/21; loaded-module ABI round-trip identical; parity gate
byte-identical (rms 0.343, `segment_gets` 2). Live (slice c, banked): ICY 2111.6 + iHeart
4412.7 RMS from the loaded `.so` + nowPlaying + [LIVE]; Dos confirmed Windows iHeart+ICY
audible, nowPlaying, scrobble, re-pin.

## Honest residuals (accepted, both OUTSIDE the audio path)
1. Ctrl+A "Deep log: ON" toast no longer shows the capture path (plugin-side now; was
   lazily-created/racy anyway).
2. The plugin's `Log` has its own `g_enabled` — a host log-disable doesn't gate the plugin's
   `[stream]` line.

## Environment / workflow (unchanged, now proven on BOTH)
Work out of `E:\code\remoct` on `restructure`. **Claude can build + ctest locally on both
platforms:** Windows via MSYS2 UCRT on PATH (`/c/msys64/ucrt64/bin`); Linux via WSL2 Debian —
mirror the tree to `/home/dostrom/rb` (rsync absent → `tar` from `/mnt/e`, overlay without
`rm -rf` to keep `rb/build` for incremental), `PULSE_SERVER=unix:/mnt/wslg/PulseServer` +
`parec RDPSink.monitor` for the audio RMS gate, drive the TUI in a persistent `tmux` pty.
Commit code and docs separately, no Co-Authored-By.

## NEXT (the payoff surface)
- **Dos: the merge decision** (`restructure` → `dev`/`main`). Trunk-based; platform diff is
  build-time (CMake), never branches.
- **The thesis payoff, on demand:** a real iHeart fix (or a new streaming plugin) can now ship
  as just a rebuilt `.so/.dll` — no host rebuild. The parity gate guards regressions.
- **Parked items** consolidated in `roadmap.md` → "Parked / deferred" (Class-B OOB re-pin and
  the Stage-A scaffold retirement are the two that need their own design-first pass + Dos
  sign-off; the rest are small self-contained changes).
