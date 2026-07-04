# Phase 4 slice (b) — host drives a source through the ABI + the HTTP service shim

**Status: DESIGN, awaiting Dos ratification before code.** Per the project's
per-slice discipline (correctness argument BEFORE audio-thread changes; confirm
before touching concurrency-sensitive paths). Slice (a) is landed & approved —
the ABI is frozen (`include/core/remoct_plugin.h`) and proven against the
disposable sine plugin on both platforms. Slice (b) is host plumbing: teach the
host to load and DRIVE a source through the C table, and build the HTTP service
shim over `core::IHttp`. **StreamSource stays compiled INTO the host here** (an
in-process plugin); slice (c) physically moves it to a `.so/.dll`.

Gate (from `phase4-readiness.md` §5b): streaming works EXACTLY as today, driven
through the C ABI; Windows + Linux ctest unchanged; live stream plays.

---

## 1. What the host consumes from `stream_source_` today (the real surface)

Grounded survey (the only call sites — nothing else touches it):

| site | call | thread |
|---|---|---|
| `AudioManager.cpp:1178` | `stream_source_.open(url)` | connect worker (blocking) |
| `AudioManager.cpp:581` | `stream_source_.readFrames(out, n)` | **audio callback** |
| `AudioManager.cpp:103,362,1084,1207,1209,1231` | `stream_source_.close()` | main thread |
| `AudioManager.cpp:337-338` | `pause(!paused())` / `paused()` | main thread |
| `AudioManager.h:114` | `setPreferDigital(b)` | main thread |
| `AudioManager.h:123` | `buffering()` | main (UI) |
| `AudioManager.h:124` | `nowPlaying()` | main (UI + scrobble) |
| `AudioManager.h:125` | `currentArtUrl()` | main (UI + Discord) |
| `AudioManager.h:126` | `url()` | main (UI) |
| `AudioManager.h:127` | `positionSec()` | main (UI) |

Two of these — `url()` and `paused()` — are **host-side state, not source
state**: the host opened the URL and set the pause flag, so it can track both
itself. The ABI has no `url`/`get_paused` getter, and this confirms it correctly
shouldn't: they are host bookkeeping. Everything else maps 1:1 to the frozen C
table (`read_frames`, `now_playing`, `art_url`, `is_buffering`, `position_sec`,
`open`, `close`, `set_config("prefer_digital", …)`).

## 2. The host-side driver — `core::PluginSource`

A thin host wrapper holding the C table + the instance, exposing the SAME method
names `stream_source_` had so the AudioManager call sites change by rename only:

```cpp
// include/PluginSource.h  (host-side; platform-free)
namespace core {
class PluginSource {
public:
    // Bind to a descriptor + host services; create the instance ONCE (mirrors
    // the by-value stream_source_ member's lifetime — created at AudioManager
    // construction, destroyed at teardown; open/close cycle per station).
    PluginSource(const RemoctPlugin* plugin, const RemoctHostServices* host);
    ~PluginSource();                       // plugin->destroy(self_)

    int  open(const std::string& url);     // plugin->open; remembers url_
    void close();                          // plugin->close (instance kept alive)
    uint32_t readFrames(float* dst, uint32_t n); // plugin->read_frames  [audio thread]

    void pause(bool p);                    // plugin->set_paused; remembers paused_
    bool paused() const { return paused_; }        // host-tracked
    bool buffering() const;                // plugin->is_buffering
    std::string nowPlaying() const;        // plugin->now_playing (snprintf-grow)
    std::string currentArtUrl() const;     // plugin->art_url
    const std::string& url() const { return url_; } // host-tracked
    double positionSec() const;            // plugin->position_sec
    void setPreferDigital(bool b);         // plugin->set_config("prefer_digital", b?"1":"0")

private:
    const RemoctPlugin* plugin_;
    void*               self_ = nullptr;   // created once in the ctor
    std::string         url_;
    std::atomic<bool>   paused_{false};
};
}
```

`nowPlaying()`/`currentArtUrl()` use the ratified snprintf-grow pattern: call
with a stack buffer, grow+retry once if the returned length exceeds it. The
string is copied into host memory — it never crosses ownership.

AudioManager change: `StreamSource stream_source_;` → `core::PluginSource
stream_plugin_;`, constructed from the stream plugin's descriptor + the host
services (both held by AudioManager). Every `stream_source_.X` becomes
`stream_plugin_.X` — the 10 sites in §1, mechanical.

## 3. The audio-thread correctness argument (the load-bearing part)

The only audio-thread change is `AudioManager.cpp:581`:
`stream_source_.readFrames(out, n)` → `stream_plugin_.readFrames(out, n)`, which
is `plugin_->read_frames(self_, out, n)`.

- **Identical lifetime.** `self_` is created ONCE in the PluginSource ctor and
  destroyed ONCE in its dtor — exactly the lifetime the by-value `stream_source_`
  member had. open()/close() cycle the producer inside the SAME instance (as
  StreamSource::open/close always did). `plugin_` and `self_` are set at
  construction and never reassigned while streaming. So the pointers the audio
  callback reads are as stable as the member reference was.
- **Identical access guard.** The callback enters the stream branch only under
  `stream_mode_.load()` (`AudioManager.cpp:579`), unchanged. Teardown quiesces
  the device (`ma_device_stop/uninit`) before any close — the callback cannot be
  running when close()/destroy runs, exactly as today.
- **The producer/ring is byte-unchanged.** `self_` IS a StreamSource object; its
  SPSC ring, `prebuffered_`, `ringClear`, re-pin, producer thread are untouched —
  the sacred machinery is not opened. The only delta is one indirect call
  (function pointer + `self`) replacing a direct member call.
- **No new allocation/lock/exception on the path.** `read_frames` is `noexcept`,
  lock-free, fills the whole buffer (live-source contract). One pointer hop.

Net: this is the mechanical indirection Phase 2 already accepted for virtual
dispatch, here as a C function pointer, with the ring semantics identical.

## 4. The StreamSource C-ABI adapter (in-process in (b) → the .so in (c))

A `RemoctPlugin` descriptor wrapping the in-tree StreamSource:

```
src/StreamPluginAdapter.cpp   (compiled into the host in (b); MOVES to
                               plugins/stream/ in (c) unchanged)
  create      -> new StreamSource()            (host services stored, UNUSED in
                                                 (b) — see §5)
  destroy     -> delete
  handles_url -> StreamSource::isStreamUrl / IHeartRadio::isIHeartUrl style check
  open        -> StreamSource::open
  read_frames -> StreamSource::readFrames
  get_caps    -> {live}
  position_sec-> positionSec ; duration_sec -> 0 ; seek_to -> false
  set_paused  -> pause ; is_buffering -> buffering ; close -> close
  now_playing -> nowPlaying ; art_url -> currentArtUrl ; last_error -> lastError
  set_config  -> "prefer_digital" -> setPreferDigital
  remoct_stream_plugin_query()  -> &descriptor
```

**Acquisition — the ONLY (b)→(c) difference.** In (b) the host calls
`remoct_stream_plugin_query()` DIRECTLY (a compiled-in extern), so no dlopen, no
new binary — pure host plumbing. In (c) that same function becomes the `.so`'s
exported `remoct_plugin_query`, reached via `core::loadPlugin(path)` (slice a).
The host-driving code (§2/§3) is byte-identical across the flip; only where the
`const RemoctPlugin*` comes from changes. This keeps (b) low-risk and makes (c) a
near-pure code-move.

## 5. The HTTP service shim over `core::IHttp` (§2.5, now built)

```
src/PluginHostServices.{h,cpp}
  struct HostServicesCtx { core::IHttp* http; };   // host_ctx points here
  RemoctHostServices buildHostServices(core::IHttp& = core::http());
    http_open_session(ctx,ua,timeout) -> ctx->http->openSession({ua,timeout})
                                          wrapped in an opaque RemoctHttpSession*
    http_fetch(sess,req,out) -> translate RemoctHttpReq -> core::HttpRequest,
                                sess->fetch(), translate HttpResponse ->
                                host-allocated RemoctHttpResp (ok/status/body/
                                final_url/flags)
    http_response_free(out)  -> free the host-owned body/final_url
    http_session_close(sess) -> delete the wrapper (drops the IHttpSession)
    log -> core::slog
```

Ownership per §2.6: `RemoctHttpResp.body`/`final_url` are host-allocated (a
`std::string` held in a host-owned wrapper the plugin frees via
`http_response_free`); the plugin copies out before freeing; nobody frees across
the boundary.

**Wiring in (b):** `create` receives the services and the adapter stores them,
but **StreamSource still reaches HTTP via `core::http()` in (b)** — the
`core::http()` → injected-services rewire is explicitly slice (c) work
(`phase4-readiness.md` §5c: "drop every `core::http()` reference in the plugin").
So in (b) the shim is BUILT and proven by its own test (§6), and gets its real
in-plugin consumer in (c). This keeps (b) from touching StreamSource internals.

### 5a. The cancel-token impedance — the one decision that needs your call
The ABI cancel is `const int32_t*` (frozen). The transport polls
`core::HttpRequest::cancel`, today `const std::atomic<bool>*`
(`HttpWinInet.cpp:81`, `HttpCurl.cpp:64` both do `req.cancel->load()`), set at
exactly ONE production site (`StreamSource.cpp:394` `req.cancel = &stop_`, plus
`http_cancel_test`). A `std::atomic<bool>` (1 byte) is NOT layout-compatible with
an `int32_t` (4 bytes), so the shim cannot reinterpret the plugin's `int32_t*` as
the transport's `atomic<bool>*` without UB + endianness assumptions (the exact
class of hack we rejected in favor of the memcpy idiom in slice a).

To preserve slice-4 per-chunk granularity with **zero extra threads and no
polling-interval degradation**, the transport must poll the int32 directly.
Proposed resolution (recommend **A**):

- **A — unify the cancel type on `int32_t` end-to-end (recommended).** Change
  `HttpRequest::cancel` to `const int32_t*`; the two transports poll it via
  `std::atomic_ref<int32_t>(...).load(std::memory_order_acquire)` (C++20) — the
  SAME per-chunk poll, a 4-byte flag with explicit acquire. Migrate the two
  consumers: `StreamSource::stop_` becomes `std::atomic<int32_t>` (used purely as
  0/1 — `store(1)`/`load()` truthy, mechanical, no logic change), so `&stop_`
  passes to both its own http cancel AND, in (c), straight across the ABI;
  `http_cancel_test`'s flag becomes int32. The shim then passes the plugin's
  `const int32_t*` through with ZERO bridging — the ABI type IS the transport
  type. Cost: a small, contained IHttp evolution (justified — the ABI is now the
  primary cancel consumer) that grazes StreamSource with a mechanical type change.
  *Note:* this is the one spot (b) touches StreamSource; it is a type change, not
  the (c) logic move. Flag for your ratification precisely because it crosses that
  line.
- **B — defer.** Keep `HttpRequest::cancel` as `atomic<bool>*` in (b); the shim's
  cancel path stays unproven-with-a-real-transport until (c) bridges it. Weaker:
  it leaves "the shim preserves cancel granularity across the boundary" (your
  explicit (b) requirement) unproven in (b).

I recommend **A** — it's the honest way to meet "the service shim must preserve
the slice-4 cancel granularity across the boundary," and it makes (c) a clean
move. It's also the smaller total change (one type unified vs a bridge that has
to be un-built in (c)).

**AS BUILT (option A ratified; realized the conforming way — flag for review).**
Dos directed "StreamSource::stop_ → atomic<int32_t>". Implemented with one
deliberate deviation, because the literal retype is non-conforming for the exact
reason A was chosen (standard-correct atomic access): retyping `stop_` to
`std::atomic<int32_t>` and having the transport read it via `std::atomic_ref<int32_t>`
MIXES `std::atomic` member writes with `atomic_ref` reads on the same object —
[atomics.ref.generic] requires the object be accessed *exclusively* through
atomic_ref while any atomic_ref references it, so the mix is UB (works on GCC,
but not conforming). Retyping `stop_` to a *plain* int32 accessed via atomic_ref
everywhere would be conforming but churns all 22 `stop_` sites + `icyHop`'s
signature. So the as-built:
- `core::HttpRequest::cancel` → `const int32_t*` (the ABI type); shared
  `core::httpCancelRequested(const int32_t*)` reads it via
  `std::atomic_ref<int32_t>` acquire-load; both transports call it.
- `StreamSource::stop_` stays `std::atomic<bool>` (icyHop + the 22 sites
  untouched). A NEW plain `int32_t http_cancel_` is written *only* through a
  `setStop(bool)` helper (which sets `stop_` and `atomic_ref`-stores the mirror
  together, so it can never drift) at `stop_`'s only two write sites (open→0,
  close→1), and `hlsHttpGet` passes `&http_cancel_` — a plain `int32_t*`, so it
  reaches the transport (and, in (c), crosses the ABI) with **zero reinterpret**
  and fully conforming atomic access on both sides. `http_cancel_test`,
  `http_seam_test`, `hls_pipeline_test` migrated to the int32 flag.
- Verified: Windows http_cancel_test aborts 393 ms, Linux 302 ms (the slice-4
  ≤one-receive-timeout property survived bool→int32); full ctest green both jobs.
This meets A's intent (int32 end-to-end, no reinterpret, standard-correct
atomic_ref) without the UB the literal retype would introduce or the 22-site
churn the plain-int retype would. If you'd rather have the literal single-flag
retype (accepting the atomic/atomic_ref mix, which GCC honors), it's a localized
swap — but I judged conformance + minimal StreamSource churn the better call.

## 6. Tests

- **Adapt existing streaming tests through the ABI (the no-regression proof).**
  `hls_pipeline_test` / `icy_pipeline_test` drive the REAL StreamSource today. In
  (b) they gain a variant (or a flag) that drives it through `PluginSource` +
  `remoct_stream_plugin_query()` + the host services — asserting byte-identical
  PCM and the same now-playing/buffering behavior via the C table. This is the
  headless "identical to compiled-in" proof one slice early (slice (d) adds the
  `.so` boundary on top).
- **`plugin_http_shim_test` (new, both platforms).** The http_cancel_test twin
  through `RemoctHostServices`: build the shim over the real transport, drive a
  localhost fixture, and prove (i) a session fetch round-trips a body, (ii)
  setting the plugin's `int32_t*` cancel aborts a wedged fetch promptly
  (per-chunk granularity — the slice-4 property, now across the C table), (iii)
  `http_response_free` ownership. This is where the service table "crosses the C
  line with a real transport behind it."
- Windows + Linux ctest otherwise **unchanged** (16→+ new; every existing test
  stays green — the gate).

## 7. Gate (both platforms)
- ctest green, existing suites unchanged; the two new/adapted tests pass.
- Live: a real stream (digital iHeart + an ICY station) plays, shows now-playing,
  buffers/recovers, and switches — driven through `PluginSource` — indistinguishable
  from today. Windows preprocessed-TU check on the StreamSource TU stays
  bit-identical if option A's mechanical `stop_` type change is the only diff.

## 8. What moves to slice (c) (kept OUT of (b) deliberately)
- Physically relocate StreamSource + IHeartRadio + SM + DeepLog + the adapter into
  `plugins/stream/`, build as a `.so/.dll`.
- Flip acquisition from `remoct_stream_plugin_query()` (direct) to
  `core::loadPlugin(path)` (dlopen).
- Rewire StreamSource/IHeartRadio HTTP from `core::http()` to the injected host
  services (the shim's real consumer).
- Move FDK-AAC/miniaudio-MP3 link deps into the plugin.

---

## Decision needed before code
**§5a: cancel-type resolution — option A (unify on `int32_t`, grazing
StreamSource's `stop_` with a mechanical type change) vs option B (defer the
bridge to (c)).** Recommend A. Everything else in §2–§7 is the ratified §2 ABI
applied; A is the one spot that touches a frozen-ish neighbor (IHttp) and the
sacred file (StreamSource, type-only), so it's yours to bless.

**STOP — no code until §5a is ratified.**
