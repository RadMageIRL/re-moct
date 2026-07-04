# Phase 4 readiness + C-ABI plugin design proposal

**Status: PROPOSAL. No code. Awaiting Dos ratification of the ABI shape before slice (a).**
This is the design-first artifact for Phase 4, the twin of `phase3-readiness.md`. It answers
four questions: (1) is the internal interface ready to harden into a binary contract; (2) what
the C ABI looks like - the one contract in the project that is genuinely hard to change after
the fact; (3) how iHeart comes out as the first real plugin; (4) how we prove it identical to
compiled-in, and the slices to get there.

Pinned, not re-litigated: the 150-sector preamble is physical (AccurateRip design); AR math /
offset / disc-ID normalization untouched; the live StreamSource read loop stays sacred; live
audio machinery (ring, `prebuffered_`, `ringClear`, re-pin) is not opened by this work.

---

## 1. Readiness - is the internal interface ready to freeze into an ABI?

**Verdict: the ISource CORE is ready; the plugin ABI is a strict SUPERSET, and its extra
facets are the three Phase-2 deferrals now coming due - that is expected, not a rough edge.
There is exactly ONE internal boundary question to settle before carving (§1.3).**

### 1.1 What is proven and freezable as-is
`core::ISource` (readFrames / caps / positionSec / durationSec / seekTo / close) is:
- **Proven across two platforms** (Phase 3) and **all four real sources** (file, iHeart-HLS,
  ICY, CD) - the strongest evidence a shape can have before becoming a contract.
- **Honest about divergence** - `SourceCaps{seekable,finite,live}` declares differences rather
  than faking uniformity, which is exactly what a plugin host wants to branch on.
- **Audio-thread clean** - `readFrames` is lock-free, never blocks, and live sources ALWAYS
  return `frame_count` (silence-pad on buffer/underrun; end is observable via state, never a
  short read). That property makes the audio side of the ABI trivial: one `uint32_t` return,
  no error channel needed on the hot path.

These six methods map to C function pointers essentially unchanged. No internal cleanup needed.

### 1.2 What ISource deliberately EXCLUDED - now due as ABI shape
Phase 2 excluded three facets by decision, explicitly flagging Phase 4 as where they resolve.
They are not defects in ISource; they are the parts a *plugin* must expose that a *statically
linked* source did not:
- **open()** - excluded because the three open shapes were irreconcilable (file sync, stream
  async/slow, CD two-level). For the plugin the shape is settled: iHeart/stream open is
  **`open(url)`, blocking and slow**, and the host **already** wraps it in an async-connect
  thread (`AudioManager::stream_connect_thread_`, `beginStream`). So the ABI's `open` stays a
  blocking C call and the host keeps owning the async orchestration verbatim - the exclusion
  resolves cleanly because we only have to model ONE open shape (streaming), not three.
- **metadata** - excluded as the least-uniform facet. For iHeart it is concretely three
  time-varying strings the host already pulls: `nowPlaying()` ("Artist - Title"),
  `currentArtUrl()`, `lastError()`. Small, pull-only, string-valued → a clean C surface
  (host-provided buffer, `snprintf`-style length return). See §2.4.
- **service injection** - the host hands the plugin `IHttp`. Today StreamSource and IHeartRadio
  both reach it through the **transitional global `core::http()`** (`StreamSource.cpp:378`,
  `IHeartRadio.cpp:58`). That global lives in the host binary and is **unreachable from a loaded
  .so/.dll** - which is precisely why the endgame was always DI. Phase 1's `IHttpSession` +
  cancel token were built for this moment; §2.5 shims them across the C line.

### 1.3 The ONE thing to decide before carving: what IS the "iHeart plugin"?
This is the real readiness finding and needs Dos's call. **iHeart is not a standalone audio
module today.** The IHeartRadio *metadata* service is cleanly isolated (own file, own session,
own sidecar). But the iHeart **HLS audio path is entangled inside `StreamSource`**, sharing the
SPSC ring, the FDK-AAC/miniaudio decoders, the producer thread, and `readFrames` with the
generic **ICY continuous** path. There is no "iHeart source" object to lift out.

Two ways to draw the plugin boundary:

- **Option A (recommended): the first plugin is the STREAMING SOURCE - `StreamSource` entire**
  (HLS + iHeart metadata + ICY), and iHeart is the headline capability that proves it.
  - Matches `architecture.md` verbatim: "Local file and CD likely stay built-in; plugins are
    for streaming sources." The streaming source *is* the plugin.
  - `StreamSource` already implements `core::ISource` with **zero .cpp hunks** - the cleanest
    possible thing to wrap in a C shim.
  - No risky pre-carve refactor of the shared ring/decoder/producer engine.
  - Fully serves the thesis: ALL iHeart logic (metadata SM, re-pin, HLS pump, deep log) moves
    into the plugin → "fix iHeart and ship without rebuilding the host" is real.
  - ICY riding along is free and correct - it is also a streaming source.

- **Option B: split iHeart-HLS out of `StreamSource` into its own source first, then plugin-ize
  only that.** More literal to "iHeart plugin," but requires disentangling the shared engine (a
  Phase-2-scale internal slice) BEFORE we carve the ABI - i.e. carving on freshly-moved,
  less-proven internals. Violates "prove internally before carving." Not recommended.

**Recommendation: Option A.** The plugin's public name can still be "iHeart / Internet Radio";
internally it is the streaming source. If Dos wants a literal iHeart-only module later, it is a
*plugin-side* refactor after the ABI is frozen - which is exactly the payoff we are building.

### 1.4 Smaller rough edges (settle in slice (a), none blocking)
- **StreamSource's public surface is much larger than ISource** (`nowPlaying`, `currentArtUrl`,
  `url`, `buffering`, `isPrebuffered`, `edgeIsMusic`, `pause`, `setPreferDigital`, `lastError`,
  `open`, `hlsProbeTest`, the staging lane). The ABI must expose only what the host actually
  consumes (§2.4/§3); the rest stays **plugin-private**. In particular the **staging lane /
  edge-peek** (`is_lane_`, `staging_`, `edgeIsMusic`) is inert scaffolding - the desync analysis
  KILLED Stage B (0/8), and its retirement is already a parked cleanup. It does NOT cross the
  ABI; it stays a plugin internal (retire it plugin-side whenever).
- **Config channel.** `setPreferDigital(bool)` comes from host `config_.prefer_digital_stream`
  and can change at runtime (Ctrl+K style reconnect). The ABI needs a tiny typed config path
  (§2.4) - do NOT grow it into a general settings bus; one string key/value setter, modeled on
  the one real knob.
- **Deep log ownership.** `IHeartDeepLog` is iHeart-specific and **already self-contained**: it
  resolves its own dir from `APPDATA`/`XDG_STATE_HOME` (`IHeartDeepLog.cpp:55-70`), portable
  since Phase 3 slice 1. It moves **into the plugin** wholesale - no host path service needed.
  Its enable toggle (host Ctrl+A) arrives via the config channel.
- **Codec deps move with the plugin.** FDK-AAC + the miniaudio MP3 decoder/`ma_data_converter`
  are streaming-only; they leave the host and link into the plugin. Real payoff: the host
  streaming branch stops carrying FDK-AAC. (Host keeps miniaudio for the device + file decode.)

---

## 2. The ABI shape - the core design

Principles (the expensive-to-change fundamentals, gotten right up front):
1. **C linkage only. No C++ across the line** - no vtables, no name mangling, no `std::` types,
   no exceptions. Only POD structs, primitives, UTF-8 C strings, and opaque `void*` handles.
2. **noexcept everywhere.** Every plugin/host entry is `noexcept`; errors are return codes or
   empty results (IHeartRadio already has the "never throws out of its public API" posture).
3. **Nobody frees across the boundary.** Whoever allocates, frees. Host buffers for host→plugin
   fills; host-owned transport results freed by a host function; plugin state freed by the
   plugin. No cross-heap `free` - the classic C-ABI landmine.
4. **Additive, struct-size-gated evolution** - see §2.2.
5. **Model the real consumer** (ISource + iHeart's real needs), not a hypothetical marketplace.

### 2.1 One exported symbol, two tables, opaque handles
The plugin exports exactly ONE C symbol (trivial `dlsym`/`GetProcAddress`):

```c
// remoct_plugin.h - the SDK header, shared by host and every plugin.
#define REMOCT_ABI_VERSION 1u

typedef struct RemoctHostServices RemoctHostServices; // host -> plugin (injected at create)
typedef struct RemoctSourceCaps   RemoctSourceCaps;   // POD mirror of core::SourceCaps

struct RemoctSourceCaps { int32_t seekable; int32_t finite; int32_t live; };

// The plugin descriptor: identity + the source op table. Returned by the one export.
typedef struct RemoctPlugin {
    uint32_t    abi_version;   // == REMOCT_ABI_VERSION the plugin was built against
    uint32_t    struct_size;   // sizeof(RemoctPlugin) at plugin build time (fwd-compat)
    const char* name;          // stable id, e.g. "stream"
    const char* display_name;  // "iHeart / Internet Radio"

    // ── lifecycle (host/UI thread) ──────────────────────────────────────────
    void* (*create)(const RemoctHostServices* host, void* host_ctx); // -> instance ("self")
    void  (*destroy)(void* self);
    int32_t (*handles_url)(void* self, const char* url);  // 1 if this plugin plays url
    int32_t (*open)(void* self, const char* url);         // BLOCKING; host wraps in its thread

    // ── audio (audio thread - lock-free, no alloc, no block) ────────────────
    uint32_t (*read_frames)(void* self, float* dst, uint32_t frame_count);

    // ── transport queries/commands (host/UI thread) ─────────────────────────
    void    (*get_caps)(void* self, RemoctSourceCaps* out);
    double  (*position_sec)(void* self);
    double  (*duration_sec)(void* self);
    int32_t (*seek_to)(void* self, double seconds);
    void    (*set_paused)(void* self, int32_t paused);
    int32_t (*is_buffering)(void* self);
    void    (*close)(void* self);

    // ── metadata (host/UI thread - pull, host-owned buffer) ─────────────────
    // Fill buf (cap bytes incl. NUL). Return the FULL length (snprintf semantics)
    // so the host can grow+retry. Empty ("" , return 0) = none yet.
    size_t  (*now_playing)(void* self, char* buf, size_t cap);
    size_t  (*art_url)(void* self, char* buf, size_t cap);
    size_t  (*last_error)(void* self, char* buf, size_t cap);

    // ── config (host/UI thread) ─────────────────────────────────────────────
    // One typed-enough knob channel. e.g. ("prefer_digital","1"), ("deeplog","1").
    void    (*set_config)(void* self, const char* key, const char* value);
} RemoctPlugin;

// THE one export. noexcept, no args, returns a pointer to a static descriptor.
REMOCT_EXPORT const RemoctPlugin* remoct_plugin_query(void);
```

`void* self` is the opaque instance handle - the C++ object lives entirely inside the plugin;
the host never sees its layout. `create` returns it, every method takes it, `destroy` ends it.

### 2.2 Versioning - reject the incompatible, evolve without breaking
- **Major gate.** Host compares `desc->abi_version` to its `REMOCT_ABI_VERSION`. Different major
  → **refuse to load** with a clear log line ("plugin 'stream' ABI v2, host expects v1"). One
  integer, checked at load, before `create`.
- **Additive growth, struct-size-gated.** Both tables carry `struct_size`. New capability = a
  new trailing function pointer, never a reordering or a signature change. Host uses
  `min(host_size, plugin_size)` to know which trailing pointers exist and **null-checks optional
  ones before calling**. Result: a v1 host loads a v1.1 plugin (ignores unknown trailing fields)
  and a v1.1 host loads a v1 plugin (sees the smaller size, treats new ptrs as absent). This is
  the same additive-only discipline every interface in the project already runs on - carried to
  the binary layer. Breaking changes (a signature change, a semantics change) bump the major and
  are rare by construction.
- One export name, forever. The loader stays a one-liner.

### 2.3 How open/read/caps/etc. become C-callable (the mechanical mapping)
| `core::ISource` / StreamSource | C ABI |
|---|---|
| construction (concrete today) | `create(host, host_ctx)` → `self` |
| `open(url)` (blocking, AM-owned async) | `open(self, url)` (blocking; host keeps its thread) |
| `readFrames(float*, n)` | `read_frames(self, dst, n)` - identical shape |
| `caps()` → `SourceCaps` | `get_caps(self, &pod)` |
| `positionSec/durationSec` | `position_sec/duration_sec(self)` |
| `seekTo(double)` | `seek_to(self, sec)` |
| `pause(bool)` | `set_paused(self, int)` |
| `buffering()` | `is_buffering(self)` |
| `close()` | `close(self)` |
| `nowPlaying/currentArtUrl/lastError` | `now_playing/art_url/last_error(self, buf, cap)` |
| `setPreferDigital(b)` | `set_config(self,"prefer_digital", b?"1":"0")` |

`~StreamSource` → `destroy`. The `hlsProbeTest`, staging lane, and `edgeIsMusic` surfaces do NOT
cross - plugin-private.

### 2.4 Metadata across the line (Phase-2 deferral #2, resolved)
Pull, not push. Host-owned buffer, plugin fills, `snprintf`-style length return - no allocation
crosses, no lifetime ambiguity, no cross-heap free. The three real consumers today:
- `now_playing` → UI now-playing label AND scrobble source (`UIManager.cpp:3141`).
- `art_url` → Discord Rich Presence cover (`UIManager.cpp:3193`).
- `last_error` → failure surfacing.
Chosen over "plugin returns a `const char*` valid until next call" because the host-buffer form
has zero lifetime rules to get wrong and matches `readFrames`' own host-buffer discipline.

### 2.5 Service injection - where Phase 1 pays off (Phase-2 deferral #3, resolved)
The host hands the plugin a **C function table** at `create`. The plugin calls back through it
to reach the host's `IHttp` - the mechanism `IHttpSession` + the cancel token were built for.

```c
typedef struct RemoctHttpSession RemoctHttpSession; // opaque host handle

typedef struct RemoctHttpReq {
    const char* url;
    const char* method;                 // "GET"/"POST"; NULL => GET
    const char* body; size_t body_len;  // POST payload (raw UTF-8 bytes - never widened)
    const char* content_type;
    const char* const* header_keys;     // parallel arrays, header_count long
    const char* const* header_vals;
    size_t      header_count;
    int32_t     redirect;               // mirror RedirectPolicy enum
    int32_t     pragma_no_cache;
    size_t      max_body;               // 0 => unlimited
    int32_t     reject_truncated;
    const int32_t* cancel;              // plugin-owned flag; host polls (acquire) per chunk
} RemoctHttpReq;

typedef struct RemoctHttpResp {        // HOST-allocated; freed by http_response_free
    int32_t     ok, cancelled, read_error, truncated;
    long        status;
    const char* body; size_t body_len; // borrowed - valid until http_response_free
    const char* final_url;
} RemoctHttpResp;

struct RemoctHostServices {
    uint32_t struct_size;
    void*    host_ctx;                  // opaque; pass back to host fns

    // HTTP - the Phase-1 IHttp/IHttpSession seam, C-shimmed.
    RemoctHttpSession* (*http_open_session)(void* host_ctx, const char* user_agent, int32_t timeout_ms);
    void               (*http_session_close)(RemoctHttpSession*);
    void               (*http_fetch)(RemoctHttpSession*, const RemoctHttpReq*, RemoctHttpResp* out);
    void               (*http_response_free)(RemoctHttpResp*);

    // Optional host log sink (host_ctx-scoped). Deep log stays plugin-owned (§1.4).
    void               (*log)(void* host_ctx, const char* msg);
};
```

Mapping to what exists:
- `http_open_session` → `core::http().openSession({ua,timeout})`. Both current session sites
  (`StreamSource.cpp:378`, `IHeartRadio.cpp:58`) become `host->http_open_session(...)`. The
  plugin no longer references `core::http()` (it can't - that global is in the host binary).
- `http_fetch` → `IHttpSession::fetch(HttpRequest)`. The host shim translates the POD `RemoctHttpReq`
  into a `core::HttpRequest`, calls the real transport (WinINet/libcurl), copies the result into a
  host-owned `RemoctHttpResp`, and hands back a borrowed pointer. Plugin copies out what it needs,
  then calls `http_response_free`.
- **cancel token** → today `const std::atomic<bool>*`. Across the line it is `const int32_t*`
  pointing at a **plugin-owned** flag; the plugin sets it from its `close()`/stop path, the host
  transport polls it with an acquire load per chunk - the exact slice-4 granularity preserved
  (worst-case abort = one receive timeout, never the remaining body). `std::atomic<bool>` is
  lock-free and layout-compatible with a 1/4-byte flag; the ABI pins it to `int32_t` and
  documents acquire semantics so no `std::atomic` type crosses.

This quietly finishes the DI endgame architecture.md always described: the transitional
`core::http()` global is what a compiled-in consumer used; the plugin gets the service handed in.

### 2.6 Ownership & lifetime - explicit, because C-ABI mistakes here are expensive
1. **Audio frames**: host allocates `dst`; plugin writes ≤`frame_count` frames; returns count.
   Host owns/frees. (Identical to today.)
2. **Metadata / error strings**: host allocates `buf[cap]`; plugin copies (truncate to `cap-1`
   + NUL); returns full length. No allocation crosses.
3. **HTTP responses**: **host-allocated**, handed to the plugin as a borrowed struct, freed by
   `http_response_free`. Plugin copies out before freeing. Plugin never frees host memory; host
   never frees plugin memory.
4. **Instance**: `create` → `destroy`, host-driven. The host GUARANTEES the audio device is
   stopped (no `read_frames` in flight) before `destroy` - it already quiesces the device around
   `stream_source_.close()` today, so this is an existing invariant, now written into the ABI.
5. **Services**: `RemoctHostServices*` and `host_ctx` are owned by the host and outlive the
   instance (valid through `destroy`). Documented as a contract.
6. **Threading**: `read_frames` is the audio thread (lock-free, no alloc, no block - enforced by
   the plugin, same as StreamSource today). Everything else is the host/UI thread. `open` is
   blocking but runs on the host's connect thread. The plugin's own producer thread is internal
   and invisible to the host, exactly as StreamSource's is now.

---

## 3. iHeart extraction plan - what crosses, what stays

Under Option A the plugin is the streaming source. Concretely:

**Moves INTO `plugins/stream/` (the .so/.dll):**
- `StreamSource.{h,cpp}` - the whole engine (HLS pump, ICY loop, ring, decoders, producer).
- `IHeartRadio.{h,cpp}` - metadata service (its `core::http()` → `host->http_open_session`).
- `IHeartNowPlayingSM.{h,cpp}` - pure SM (moves verbatim; already isolated).
- `IHeartDeepLog.{h,cpp}` - self-contained, resolves its own dir; deep-log enable via config.
- FDK-AAC + miniaudio-MP3 + `ma_data_converter` link dependencies.
- `AacDecoder` if stream-only (verify at slice (c)).
- The plugin's thin C entry (`remoct_plugin_query`, the `RemoctPlugin` table, the `self` = a
  `StreamSource` wrapper translating C calls to the C++ object).

**Stays in the HOST:**
- The `IHttp`/`IHttpSession` transport (WinINet/libcurl) - exposed to the plugin via services.
- The async-connect orchestration (`AudioManager::beginStream`, `stream_connect_thread_`,
  latest-wins pending slot) - it wraps the plugin's blocking `open`.
- The audio device + the callback that calls `read_frames` into the device buffer.
- now-playing consumption: UI label, **scrobbling** (`updateScrobbler` reads `now_playing`),
  **Discord RP** (reads `art_url`) - all unchanged, just sourced through the C ABI.
- `stationLabel`, station browsing, config, the whole UI.

**Crossings summary:**
| iHeart/stream need | Crosses via |
|---|---|
| encoded bytes over HTTP (manifest, segments, metadata JSON) | host `http_*` services (§2.5) |
| prompt abort on stop/close | `cancel` flag in `RemoctHttpReq` |
| PCM to the device | `read_frames` (host buffer) |
| now-playing / art / error to UI+scrobble+Discord | `now_playing`/`art_url`/`last_error` |
| prefer-digital / deeplog toggles from host | `set_config` |
| deep log to disk | plugin-owned (no host service) |

The sacred live read loop stays byte-verbatim - it just lives in the plugin binary now; nothing
about the ICY WinINet/curl transport changes, and StreamSource's `readFrames` is untouched.

---

## 4. The gate - "identical to compiled-in"

Two layers, both required, both platforms:

1. **Deterministic byte-identity (the strong proof).** `hls_pipeline_test` already drives the
   REAL StreamSource HLS/AAC pipeline headlessly with a `FakeHttp` (synthetic manifests + real
   FDK-encoded ADTS sine) and asserts PCM through `readFrames`. Retarget it **through the plugin
   boundary**: load the built `.so/.dll`, inject a `FakeHttp` as the host HTTP *service*, drive
   `read_frames`, and assert the produced PCM is **byte-identical** to the compiled-in run. This
   turns "identical to compiled-in" into a CI assertion on every push (both matrix legs). It also
   exercises the service-injection path and the cancel token (the test's prompt-close case).
2. **Live behavioral parity (the real-world proof).** On both platforms, from a `.so/.dll`:
   digital iHeart (Z100) plays and is audible (sink-monitor RMS, as in slice 2/3 gates);
   `nowPlaying` resolves through the host HTTP service; a scrobble round-trips (Last.fm + LB);
   Discord RP shows title/artist + iHeart cover; the ad-onset re-pin fires; ICY station plays
   with titles. Each behavior matched against the current compiled-in build.

Note honestly: live radio is not byte-reproducible across sessions, so live audio is a
*behavioral* match, not a `cmp`. The byte-identity claim rests on layer 1 (deterministic fixture)
- which is why retargeting `hls_pipeline_test` through the boundary is the load-bearing gate.

---

## 5. Slicing (confirm / revise)

Proposed four slices, refined from the roadmap sketch:

- **(a) Freeze the C ABI against a TRIVIAL plugin.** Author `remoct_plugin.h` (the SDK header:
  `RemoctPlugin`, `RemoctHostServices`, versioning, ownership contract in comments). Build the
  host-side loader (`dlopen`/`LoadLibrary`, one symbol, version gate) and a **throwaway test
  plugin** (a sine/silence generator - no network) proving load → version-check → `create` →
  `open` → `read_frames` → `now_playing` → `set_config` → `close` → `destroy` on **both
  platforms**, plus the reject path (wrong `abi_version` → refused). This freezes the contract
  against a disposable consumer FIRST - the seam-test discipline ("prove the loader with a
  trivial plugin before the real one"). No StreamSource yet. **Gate:** loader test green both
  platforms; deliberate-mismatch rejected.
- **(b) Host drives a source through the ABI.** Wire `AudioManager`'s streaming branch to talk
  to a plugin instance through the C table instead of the by-value `stream_source_` member: the
  connect thread calls `open`, the callback calls `read_frames`, the UI calls
  `now_playing`/`art_url`/caps. Build the host HTTP **service shim** (§2.5) over the real
  `IHttp`. StreamSource can still be **statically present** here (loaded from an in-tree plugin
  target) so this slice is host-plumbing only, no behavior change. **Gate:** streaming works
  exactly as today, driven through the C ABI; Windows + Linux ctest unchanged; live stream plays.
- **(c) Extract the streaming source as the first real .so/.dll.** Move the §3 file set into
  `plugins/stream/`, build it as a shared library, implement `remoct_plugin_query` + the `self`
  wrapper, and route iHeart/HLS HTTP + metadata polls through the injected host services (drop
  every `core::http()` reference in the plugin). Keep it in the **monorepo** while the ABI churns
  (`architecture.md`: atomic host+plugin commits, one CI; extract to its own repo only after the
  ABI freezes). **Gate:** the byte-identity fixture (layer 1) + first live plays from a real
  `.so/.dll`.
- **(d) The identical-to-compiled-in gate, full.** `hls_pipeline_test` through the boundary
  asserting byte-identical PCM on both matrix legs; the full live parity battery (§4 layer 2) on
  both platforms; Discord/ICY/scrobble parity. This is the thesis proof: dynamically-loaded
  iHeart behaves identically to in-tree, and can now be fixed and shipped without rebuilding the
  host.

Confirmation: the four-slice shape from the roadmap holds; the refinements are (1) the trivial
test-plugin as the explicit freeze proof in (a), (2) naming the host HTTP service shim as the
concrete deliverable of (b), (3) pinning the byte-identity retarget of `hls_pipeline_test` as
(d)'s load-bearing assertion.

---

## 6. Decisions & remaining sign-off before slice (a)

**§1.3 boundary - RATIFIED: Option A** (Dos, Phase-4 kickoff). The first plugin is the streaming
source `StreamSource` entire (HLS + iHeart metadata + ICY), iHeart as its headline capability. No
pre-carve split of the shared engine; a literal iHeart-only module, if ever wanted, becomes a
plugin-side refactor after the ABI freezes. Everything in §2–§5 already assumes A.

Still wanting explicit sign-off before slice (a) begins (these are the expensive-to-change
fundamentals; cheap to confirm now, costly to change post-freeze):
- (i) **Ownership model (§2.6)** - host buffers for frames + strings; host-allocated & host-freed
  HTTP responses; `cancel` as a plugin-owned `int32_t*` polled by the host transport.
- (ii) **Versioning (§2.2)** - one major-gate integer checked at load + struct-size additive
  growth for optional trailing function pointers.
- (iii) **Metadata (§2.4)** - pull via host-provided buffer (`snprintf` semantics), NOT a
  plugin-owned `const char*` with a lifetime rule.

**STOP - no code until §2 (the ABI shape) is ratified.**
