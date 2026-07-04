# Phase 4 slice (c) — extract the streaming stack as a real `.so/.dll`

Ratified pre-code (Dos, 2026-07-04). Read after `phase4-readiness.md` (§2 = the frozen
ABI, §3 = what crosses) + `phase4-slice-b-design.md` (the in-process precursor). This is
the slice where "fix iHeart and ship without rebuilding the host" becomes literally true:
StreamSource stops being compiled into `remoct` and becomes a loadable module reached
through the frozen C ABI.

Because slices (a)/(b) de-risked the contract, this is **mostly a code-move onto an
already-frozen ABI** — the crux is (1) the exhaustive host-symbol survey and (2) the
build/link restructure, not new logic. The audio path is byte-verbatim.

---

## 0. Pinned / sacred (do NOT touch)
- The **150-sector preamble** — a physical property of the disc (AccurateRip design).
- StreamSource's **live read loop** (ring / producer / `rawRead` / de-interleave /
  `readFrames` / `prebuffered_` / `ringClear` / re-pin / staging lane) — moves
  **byte-verbatim**. This is a file move, not a rewrite.
- The frozen v1 ABI (`include/core/remoct_plugin.h`) — unchanged by this slice.
- The `int32_t` cancel token, `setStop()`/`http_cancel_` mechanism — unchanged.

---

## 1. The host-symbol survey (the crux)

Once the moving files are a `.so/.dll` that does **not** link the host's core/platform
TUs, *every* symbol they reference that lives in a non-moving host TU becomes an undefined
symbol at link/load. Sweeping ALL host symbols (not just `core::http()`) surfaced **8
categories**:

| # | Site(s) | Symbol | In-host source (STAYS host) | Resolution in the plugin |
|---|---------|--------|-----------------------------|--------------------------|
| 1 | `StreamSource.cpp:378` `hls_session_ = core::http().openSession(cfg)` | `core::http()` global | `HttpWinInet.cpp` / `HttpCurl.cpp` | **rewire → injected host services** |
| 2 | `IHeartRadio.cpp:58` `session_ = core::http().openSession(cfg)` | `core::http()` global | same | **rewire → injected host services** |
| 3 | `StreamSource.cpp:37` `Log::write("stream", …)` | `Log::write` | `Log.cpp` | **link `Log.cpp` into the plugin** (D1) |
| 4 | `StreamSource.cpp:286,303` `InternetOpenA`/`InternetOpenUrlA` | raw WinINet | `-lwininet` | **plugin links `-lwininet`** (sacred ICY loop) |
| 5 | Linux ICY twin `curl_easy_recv` (CONNECT_ONLY) | libcurl | `CURL::libcurl` | **plugin links `CURL::libcurl`** (sacred ICY loop) |
| 6 | `ma_decoder_*`, `ma_data_converter_*` | miniaudio impl | `MINIAUDIO_IMPLEMENTATION` in host `AudioManager.cpp:1` | **plugin gets its own `miniaudio_impl.cpp`** (D3) |
| 7 | `aacDecoder_*` (`aac_dec_`) | FDK-AAC | `${FDKAAC_LIB}` | **plugin links `fdk-aac`** |
| 8 | `UIManager.cpp:3822-3823` `IHeartDeepLog::toggle()`/`path()` (Ctrl+A) | `IHeartDeepLog::*` | those files **move into the plugin** | **route via ABI `set_config("deeplog", …)`** (D6) |

The two `core::http()` sites (rows 1–2) are the **entire seam rewire** — exhaustive. The
other 8 tree-wide `core::http()` sites (CoverArt, CDRipper, LastFm, ListenBrainz, MBLookup,
RadioBrowser) are non-moving host modules, untouched.

### The critical distinction — rows 4–5 (two different HTTP consumers)
"The plugin must NOT link its own HTTP transport" is true **only for the `core::IHttp`
seam** — `HttpWinInet.cpp`/`HttpCurl.cpp` stay host-side and are reached across the service
table. StreamSource's **sacred raw ICY read loop is private raw WinINet / raw libcurl** —
it is NOT `core::http()`, it moves with the plugin byte-verbatim, so **the plugin DOES link
`-lwininet` (Win) / `CURL::libcurl` (Linux)** for that loop. Two HTTP consumers in the
plugin; only the seam one crosses the table. Taking the "no HTTP in the plugin" phrasing
literally would have meant rewriting the sacred loop or a link failure.

### The HTTP rewire — the inverse of the slice-(b) shim
`PluginHostServices` (host side) fills a `RemoctHostServices` table **from** `core::IHttp`.
The plugin needs the **mirror**: a plugin-side `core::IHttp`/`IHttpSession` **over** the
injected C table, so StreamSource/IHeartRadio's existing `->openSession(cfg)` /
`session_->fetch(req)` C++ code is unchanged against the host transport.

New files `plugins/stream/HostServiceHttp.{h,cpp}`:
- `HostServiceHttp : core::IHttp` — `openSession(cfg)` → `host->http_open_session(host_ctx,
  ua, timeout)`, wraps the returned opaque `RemoctHttpSession*`. `fetch()` (the pure
  virtual, unused by the two consumers) = a one-shot transient session.
- `HostServiceSession : core::IHttpSession` — `fetch(HttpRequest&)` translates to
  `RemoctHttpReq` (parallel `header_keys`/`header_vals`, `cancel` `const int32_t*`
  **verbatim**), calls `host->http_fetch`, copies out, `host->http_response_free`. This is
  the exact **inverse of `svc_fetch`**. Dtor → `host->http_session_close`, so the session's
  lifetime tracks the `unique_ptr<IHttpSession>` the consumers already hold (HLS session
  dies at `disconnect()`, IHeart's lives with the object — parity preserved).

Injection: StreamSource/IHeartRadio take `core::IHttp&` by ctor (stored as `IHttp* http_`);
the two edits are rows 1–2 (`core::http()` → `http_->`). The adapter's `StreamInstance`
builds `HostServiceHttp` from the injected `RemoctHostServices*` and constructs
`StreamSource` with it. **NO default arg `= core::http()`** — that would leave the symbol
textually in a plugin header and fail both the link and the `grep core::http()
plugins/stream/ = 0` audit. Explicit injection everywhere; in-tree host callers pass
`core::http()`.

---

## 2. Build / link restructure

**New target `plugins/stream/` (MODULE), monorepo, `git mv` to preserve blame:**
```
add_library(remoct_stream MODULE
    StreamPluginAdapter.cpp   # + REMOCT_EXPORT remoct_plugin_query() (keeps remoct_stream_plugin_query — D4)
    StreamSource.cpp  IHeartRadio.cpp  IHeartNowPlayingSM.cpp  IHeartDeepLog.cpp
    HostServiceHttp.cpp       # NEW: core::IHttp over the injected RemoctHostServices table
    ${SRC}/Log.cpp  ${SRC}/StringUtils.cpp   # support files (mirror plugin_stream_test's proven set)
    miniaudio_impl.cpp        # NEW: MINIAUDIO_IMPLEMENTATION + MA_NO_DEVICE_IO (decode/convert only — D3)
)
set_target_properties(remoct_stream PROPERTIES PREFIX ""   # remoct_stream.{so,dll}, not libremoct_stream
    LIBRARY_OUTPUT_DIRECTORY ${BIN}/plugins  RUNTIME_OUTPUT_DIRECTORY ${BIN}/plugins)
target_link_libraries(remoct_stream PRIVATE ${FDKAAC_LIB}
    $<$<PLATFORM_ID:Windows>:wininet>
    $<$<PLATFORM_ID:Linux>:CURL::libcurl> $<..:dl> $<..:m> $<..:pthread>)
add_dependencies(remoct remoct_stream)   # the host build always builds the plugin
```
- **Exports exactly `remoct_plugin_query`** (`PREFIX ""` + `plugin_sine`/MODULE template).
- **Links** fdk-aac + own miniaudio impl + wininet/libcurl (raw ICY only). Does **NOT**
  link the seam impl TUs (`HttpWinInet.cpp`/`HttpCurl.cpp`) — HTTP crosses via the table.
- **Two miniaudio impls in one process is safe:** separate binaries; the loader uses
  `dlopen(RTLD_LOCAL)` (Posix) / per-module symbol space (Windows), so the plugin's
  miniaudio symbols never collide with the host's. Same reasoning as D1's dual Log.

**Host target (`remoct`):**
- **Remove** from `REMOCT_SOURCES`: `StreamSource.cpp`, `StreamPluginAdapter.cpp`,
  `IHeartRadio.cpp`, `IHeartNowPlayingSM.cpp`, `IHeartDeepLog.cpp`.
- **Add** (were test-only until now — the host is a loader now): `src/PluginHost.cpp` +
  `src/platform/win/PluginLoaderWin.cpp` (WIN32) / `src/platform/linux/PluginLoaderPosix.cpp`
  (UNIX).
- Host **keeps** fdk-aac + miniaudio (AacDecoder file path + the device), `PluginSource.cpp`,
  `PluginHostServices.cpp`.
- `AudioManager.h` drops `#include "StreamPluginAdapter.h"` / `remoct_stream_plugin_query()`
  → **the host no longer names any moving-file symbol** (the clean cut).

**Path resolution:** new `port::exeDir()` (Win `GetModuleFileNameW`→dir via
`WideCharToMultiByte(CP_UTF8)`; Linux `readlink(/proc/self/exe)`→dir). Load
`exeDir()/plugins/remoct_stream.{dll,so}` (D2). The `bin/plugins` output dir satisfies dev
+ install.

---

## 3. Acquisition flip + graceful failure

`AudioManager.h` — the single conceptual change; `PluginSource` driving code is byte-identical:
```
core::HostServices                  host_services_{};     // 1st (unchanged)
std::unique_ptr<core::LoadedPlugin> loaded_plugin_;       // NEW, 2nd
std::string                         plugin_load_error_;   // reject reason for the UI
core::PluginSource                  stream_plugin_;       // 3rd — wired in the ctor
```
The load moves to the ctor (needs the path + must capture the reject reason):
`loaded_plugin_ = core::loadPlugin(streamPluginPath(), &why)`, then
`stream_plugin_(loaded_plugin_ ? loaded_plugin_->plugin() : nullptr, host_services_.table())`.
Load failure (missing / ABI-mismatch / too-small — the slice-(a) reject paths, now in
**production**) → `nullptr` → `PluginSource::valid()==false` → all ops no-op, **host stays
alive**. `beginStream()` gains a `valid()` check → toasts `"streaming plugin unavailable:
<pluginLoadName(why)>"` instead of silently failing. This is where slice (a)'s reject-path
tests earn their keep.

---

## 4. The deep-log toggle rewire (row 8 — discovered during the survey)

`UIManager` (host) Ctrl+A calls `IHeartDeepLog::toggle()` + `path()` — static host→plugin
calls that break on the move. Fix follows the Ctrl+K template right below it (host-tracked
state + `set_config` across the ABI, exactly the readiness §3 "deep-log enable via config"):
- `IHeartDeepLog` gains `void setEnabled(bool)` (absolute state; `toggle()` unchanged for
  any in-plugin use). The adapter's `sp_set_config` handles `("deeplog","1"/"0")` →
  `IHeartDeepLog::setEnabled(...)` (in-plugin call, fine).
- `PluginSource::setConfig(key,value)` generic passthrough (setPreferDigital keeps working);
  `AudioManager::setDeepLog(bool)` → `stream_plugin_.setConfig("deeplog", …)`.
- UIManager tracks `bool deeplog_on_`, flips it on Ctrl+A, calls `audio_.setDeepLog(...)`.
- **Accepted residual:** the "ON" toast no longer shows `IHeartDeepLog::path()` (the host
  can't resolve the plugin-side path, and it was already lazily-created/racy — the file is
  only created on the first `emit()` on the producer thread). Toast becomes `"Deep log:
  ON"` / `"OFF"` with no path body. Diagnostic-only; the ABI is NOT grown for one string.

---

## 5. Rippled call sites (ctor injection)
StreamSource/IHeartRadio ctors gain `core::IHttp&`; the in-tree callers pass `core::http()`:
- `plugins/stream/StreamPluginAdapter.cpp` — `StreamInstance` builds `HostServiceHttp`, ctor.
- `StreamSource.cpp:1005` staging lane — `make_unique<StreamSource>(true, *http_)`.
- `tests/hls_pipeline_test.cpp` — `StreamSource ss(core::http())` (fake set via `setHttp`).
- `tests/icy_pipeline_test.cpp` (×3) — `StreamSource ss(core::http())` (pure raw ICY; `http_`
  unused).
- `tests/iheart_request_test.cpp:96` — `IHeartRadio ih(core::http())`.
- `plugin_stream_test` — drives via the adapter (no direct construction); its CMake source
  paths update to `plugins/stream/` + `HostServiceHttp.cpp`. Stays the **compiled-in
  byte-identity reference** slice (d) compares the loaded `.so` against (D4).
- Standalone `docs/IHeartRadio/SniffIHeartRadio.cpp` is not in the build — left as-is
  (would need the same one-line ctor change if ever recompiled).

---

## 6. Ratified decisions
- **D1** link `Log.cpp` into the plugin (atomic `fopen(…,"a")` per write interleaves safely;
  residual: independent `g_enabled`, so a host log-disable won't gate the plugin's `[stream]`
  line — documented, ABI not grown for one call). → lessons.md.
- **D2** `<bindir>/plugins/` via `port::exeDir()`.
- **D3** `miniaudio_impl.cpp` with `MA_NO_DEVICE_IO` — plugin decodes/converts, never opens a
  device (host owns it); the trim enforces device-ownership host-side in the build.
- **D4** keep `remoct_stream_plugin_query()` alongside the exported `remoct_plugin_query()`.
- **D5** `AacDecoder` stays host (file `.m4a/.m4b` backend); the plugin's `aac_dec_` is the
  distinct stream consumer. Does not move.
- **D6** deep-log toggle → `set_config("deeplog", …)` + host-tracked state (see §4).

---

## 7. Gate plan
1. **Static audits:** `grep core::http() plugins/stream/` = **0**; sacred-symbol grep over
   the StreamSource diff = **0** (ring/prebuffered_/ringClear all context — only rows 1–2 +
   ctor threading changed).
2. **Headless (slice c):** the built `.so/.dll` loads via `core::loadPlugin`, the lifecycle
   round-trips through the loaded table, existing stream tests pass **through the loaded
   plugin**, both platforms; the compiled-in `plugin_stream_test` stays green.
3. **Live (start of slice d — STOP for review before this):** ICY + digital iHeart play from
   the **loaded** plugin — audible (WSL sink-monitor RMS + Windows), nowPlaying updates,
   scrobble lands, re-pin fires — matched to slice (b)'s compiled-in behavior.
4. **Regression:** host builds; file/CD paths untouched; Windows + Linux ctest green.

Order: `git mv` the five files → HostServiceHttp + rows 1–2 rewire + ctor threading + row-8
deep-log → build/link restructure → acquisition flip + graceful failure. Stop for review
before the live gate / slice (d).
