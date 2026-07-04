# Phase 3 slice 3 — ICY continuous-stream Linux twin (design, pre-code)

Status: **PROPOSAL — no code written.** Awaiting Dos sign-off.
Scope: the ONE remaining WinINet site — StreamSource's continuous ICY transport.
After this slice the platform boundary is complete (slices 4/5/6 are seam impls,
not raw transports).

Pinned, not re-litigated: the Windows ICY path stays raw WinINet byte-verbatim,
permanently outside the IHttp seam. The live read loop is SACRED. This slice adds
a Linux `#else` twin only; zero changed lines inside any `#ifdef _WIN32` block.

---

## 1. Survey — the ICY path as it exists (verified file:line, this tree)

### 1a. Windows-specific transport (the part that gets a twin)

**`connect()` ICY branch — `src/StreamSource.cpp:96-133`** (inside `#ifdef _WIN32`):
- `:99-101` `InternetOpenA` with UA `"RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)"`, `INTERNET_OPEN_TYPE_PRECONFIG` (IE proxy settings).
- `:105-108` 8000 ms CONNECT / RECEIVE / SEND timeouts on `hInet_`.
- `:112-116` `InternetOpenUrlA` with the extra header `"Icy-MetaData: 1\r\n"` and
  flags `RELOAD | NO_CACHE_WRITE | PRAGMA_NOCACHE | KEEP_CONNECTION`; TLS by
  scheme; WinINet follows redirects and consumes the response headers internally.
- `:123-128` `HttpQueryInfoA(HTTP_QUERY_CUSTOM, "icy-metaint")` → `icy_metaint_`
  (absent/0 = no in-band metadata).
- `:129-131` post-connect state init: `icy_counter_ = icy_metaint_;
  raw_buf_.clear(); raw_pos_ = 0;`.
- `:134-141` `#else`: Linux refuses Continuous mode with a clear error (slice-1
  placeholder this slice replaces).

**`rawRead()` — `src/StreamSource.cpp:1006-1032`** (the sacred loop's transport
floor; `#ifdef _WIN32` at `:1008`):
- `:1011-1021` when the buffer is drained: guard `stop_.load() || hConn_ ==
  nullptr` → 0 (`:1012`); refill `raw_buf_` via ONE
  `InternetReadFile(hConn_, buf, 8192, &got)`; `!ok || got == 0` → clear + return
  0 (end-of-stream signal, `:1015-1018`).
- `:1022-1026` serve-from-buffer tail: memcpy min(want, avail), advance
  `raw_pos_`. Pull shape: "give me up to N bytes when I ask."
- `:1027-1031` `#else`: unreachable return 0.

**`disconnect()` — `src/StreamSource.cpp:145-152`**: `#ifdef _WIN32` closes
`hConn_`/`hInet_` (`:147-148`); `hls_session_.reset()` is common code (`:150`).

**Handle members — `include/StreamSource.h:219-225`**: `HINTERNET` on Windows,
**inert `void*` twins on Linux** (slice 1) — the machinery compiles unreferenced.

### 1b. Platform-neutral protocol logic (must NOT move, zero diff)

- **`readAudio()` de-interleave — `src/StreamSource.cpp:1044-1071`**: if
  `icy_metaint_ <= 0` → passthrough (`:1045-1046`); else count down
  `icy_counter_`, at 0 read 1 length byte + `len*16` metadata bytes via
  `rawReadExact`, `parseIcyMetadata(block)`, reset counter (`:1051-1061`); clamp
  each audio chunk to the counter (`:1063-1068`). **The metaint boundary lives
  entirely here, in bytes-consumed arithmetic** — it is transport-blind as long
  as the byte stream starts at offset 0 of the body.
- **`rawReadExact()` — `:1034-1042`**: loop over `rawRead` (neutral).
- **`parseIcyMetadata()` — `:1073-1087`**: `StreamTitle='…';` extraction →
  `now_playing_` under `now_playing_mtx_` (neutral).
- **`onRead()` miniaudio callback — `:1132-1146`**: guard `stop_.load() ||
  hConn_ == nullptr` → `MA_AT_END` (`:1136-1137`); pulls through `readAudio`
  (`:1139`). Shared, ungated code — **it keys on `hConn_`**, which is why the
  twin must populate `hConn_` (see §2).
- **Producer loops** (shared, ungated): MP3 `producerWorker` `:1158-1228` —
  re-pin block `:1176-1185` (`uninitDecoder`/`disconnect`/`prebuffered_ false`/
  `ringClear`/`connect`), reconnect-with-backoff `:1208-1222` (framesRead==0 →
  `disconnect` → sleep 500·n → `connect`, cap 10); AAC `producerWorkerAAC`
  `:1232-1356` — re-pin `:1253-1262`, reconnect `:1268-1283` (`got==0` from
  `readAudio`). Both plug into the twin exclusively through `connect()` /
  `disconnect()` / `rawRead()` — no other coupling.
- **stop_ abort points**: `close()` `:80-89` (sets `stop_`, joins producer);
  `rawRead` `:1012`; `onRead` `:1136`; producer loop heads `:1173/:1251` and
  mid-reconnect checks `:1182/:1210/:1221` and `:1258/:1270/:1279`.
- **Ring machinery** (`ringWrite/ringRead/ringClear/ringAvailable`,
  `prebuffered_`, `readFrames`): untouched, not even quoted here — sacred.

### 1c. Baseline behavior contract the twin must match

| Property | Windows baseline |
|---|---|
| Request | GET with `Icy-MetaData: 1`, app UA, TLS by scheme, redirects followed (WinINet internal) |
| `icy-metaint` | read from response headers; absent → 0 → passthrough |
| Read shape | pull: one ≤8192-byte network read when the buffer drains; serve from buffer |
| `rawRead` returns 0 | on `stop_`, on closed/failed read → producer treats as stream-end and reconnects |
| Blocked-read bound | RECEIVE_TIMEOUT 8 s (a wedged station fails the read within 8 s; `stop_` is only polled *between* chunk reads — the known parked "stalled-connect/read interrupt" gap) |
| Connect bound | 8 s; `stop_` NOT polled during connect (same parked gap; parity, don't fix as ride-along) |
| Re-pin/reconnect | `disconnect()` then `connect()`; connect re-inits `icy_counter_`/`raw_buf_`/`raw_pos_` |

---

## 2. The transport twin — curl CONNECT_ONLY + curl_easy_recv

### Why this shape (the argued lean)

The property that matters is **the pull-read shape verbatim**: `rawRead` stays
"when my buffer drains, fetch up to 8192 bytes NOW and hand them to me" — so the
ring-feed, de-interleave, and `stop_` polling above it are unchanged. Candidates:

1. **libcurl normal perform (WRITEFUNCTION)** — curl does parse `ICY 200 OK`
   status lines and would surface `icy-metaint` via HEADERFUNCTION. **Rejected:**
   the write callback is push-shaped — `curl_easy_perform` runs the whole
   transfer and pushes bytes at us. Making it pull-shaped needs either a
   pump-the-multi-handle-inside-rawRead state machine or a second thread + queue
   — both restructure the sacred loop's world. (The IIpc precedent: pick
   primitives that keep the consumer's loop verbatim.)
2. **Raw POSIX sockets + OpenSSL** — perfect pull shape, but hand-rolls TLS
   (handshake, SNI, cert verification, buffering). **Rejected:** large new trust
   surface for exactly what curl already gives us.
3. **curl `CONNECT_ONLY=1` + `curl_easy_send`/`curl_easy_recv`** — **chosen.**
   `curl_easy_perform` with `CURLOPT_CONNECT_ONLY=1` performs everything up to
   and including the connect — **for https URLs that includes the TLS
   handshake** — then does no transfer. `curl_easy_send`/`curl_easy_recv` then
   move raw bytes over that connection, transparently TLS-wrapped when the
   scheme is https. We own the socket: `curl_easy_recv` is the exact sibling of
   `InternetReadFile` in the refill slot.

### How the ICY request/response works over CONNECT_ONLY (the shown part)

CONNECT_ONLY sends no HTTP — **we write the request and read the response
headers ourselves.** That is a feature, not a workaround: ICY is
HTTP-with-a-twist (`ICY 200 OK` status lines from SHOUTcast v1), and owning the
parse means no dependency on any library's tolerance.

Connect sequence (all new code, Linux-only, file-local helpers in
StreamSource.cpp beside the gated Windows block):

1. **Parse the URL** with curl's own `CURLU` API (`curl_url_set(CURLUPART_URL)`,
   get scheme/host/port/path/query) — no hand-rolled parsing.
2. **Connect:** easy handle with `CURLOPT_URL` (full URL — drives host/port/TLS/
   SNI), `CURLOPT_CONNECT_ONLY=1`, `CURLOPT_CONNECTTIMEOUT_MS=8000`;
   `curl_easy_perform` → TCP (+TLS) established. Mirrors the 8 s connect bound;
   `stop_` not polled during this call (baseline parity, §1c).
3. **Send the request** via a bounded `curl_easy_send` loop (8 s total, `stop_`
   polled between attempts):

   ```
   GET <path[?query]> HTTP/1.0\r\n
   Host: <host[:port]>\r\n
   User-Agent: RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)\r\n
   Icy-MetaData: 1\r\n
   \r\n
   ```

   **HTTP/1.0 deliberately** (WinINet spoke 1.1 and internally handled anything
   the server did with it): a 1.0 request forbids `Transfer-Encoding: chunked`
   in the response, so the body is guaranteed to be the raw interleaved byte
   stream our de-interleave expects. Belt-and-braces: if a nonconformant server
   sends `Transfer-Encoding:` anyway, fail the connect with a clear log rather
   than feed framed bytes to the metaint arithmetic.
4. **Read response headers ourselves:** recv into a bounded header buffer (16 KB
   / 8 s cap, `stop_` polled) until the `\r\n\r\n` terminator (tolerate bare
   `\n\n`). Parse:
   - Status line: accept `ICY 200`, `HTTP/1.0 200`, `HTTP/1.1 200`.
   - `3xx` + `Location:` → close the handle, loop back to step 1 with the new
     URL (cap 5 hops) — WinINet followed redirects internally; we must too.
   - `icy-metaint:` case-insensitive → `icy_metaint_` (absent → 0, passthrough).
   - Anything else → fail (same as baseline `InternetOpenUrlA` returning null).
5. **THE alignment step:** any bytes received past the header terminator are the
   first body bytes. They are loaded into `raw_buf_` (`raw_pos_ = 0`) so the
   byte stream the de-interleave sees starts at body offset 0 exactly. (Windows
   never had this hazard — WinINet consumed precisely the headers. This is the
   one genuinely new correctness obligation; see §3.)
6. **Publish the connection:** `hConn_ = easy_handle` — the slice-1 `void*` twin
   member takes the CURL* — so the shared guards (`onRead:1136`,
   `rawRead:1012`) stay meaningful with **zero diff in shared code**. `hInet_`
   stays nullptr on Linux (documented). Then the same state init the Windows
   block does at `:129-131`: `icy_counter_ = icy_metaint_`, `raw_pos_ = 0`
   (with `raw_buf_` holding the §2.5 leftover instead of cleared — argued in §3).

`rawRead` twin (`#else` branch, self-contained; the Windows block is not
touched, its 5-line serve-from-buffer tail is repeated verbatim in the twin):

```
refill when drained:
  guard stop_/hConn_ (same line shape as :1012)
  loop:
    rc = curl_easy_recv(h, buf, 8192, &got)
    rc == CURLE_OK && got > 0  -> have data, done
    rc == CURLE_OK && got == 0 -> orderly close -> return 0   (== InternetReadFile eof)
    rc != CURLE_AGAIN          -> error -> return 0           (== InternetReadFile fail)
    CURLE_AGAIN:
      stop_?            -> return 0
      waited >= 8000ms  -> return 0   (mirrors RECEIVE_TIMEOUT: wedged station fails the read)
      poll(CURLINFO_ACTIVESOCKET, POLLIN, 100ms); waited += elapsed
serve-from-buffer tail: identical arithmetic to :1022-1026
```

**recv-first, poll-second is load-bearing:** with TLS, decrypted bytes can sit
buffered inside the TLS layer while the raw socket shows nothing readable — a
poll-first loop deadlocks on data it already holds. `curl_easy_recv` drains
curl's/TLS's internal buffer before touching the socket, so the order above is
the correct one. `EINTR` from poll = retry.

### Named residuals (accepted, documented)

- **Proxy:** WinINet PRECONFIG honored IE proxy settings. The hand-written
  origin-form GET does not speak through an HTTP proxy. Slice 2 already moved
  Linux to the env-var convention for the seam; for the raw ICY socket, proxied
  operation on Linux is out of scope (log-visible connect failure, not silent).
- **Redirect semantics:** hand-rolled 3xx handling (cap 5) vs WinINet's internal
  follow. Same outcome for the station shapes that exist (probe-verified, §5);
  exotic cases (auth challenges, cookies) were never supported by the baseline
  path either.
- **stop_ during a stalled read aborts in ≤~100 ms on Linux** vs ≤8 s on
  Windows — strictly better, the slice-2 "cancel works during CONNECT" class of
  accepted-better residual. The parked "stalled-connect interrupt" item stays
  parked for both platforms (connect itself is still an 8 s blind spot).
- **curl_global_init:** the twin does its own file-local `std::call_once`
  (HttpCurl.cpp's is TU-private); `curl_global_init` is thread-safe on the
  shipped curl (≥7.84 everywhere we build).

---

## 3. Correctness argument (the sign-off gate)

The transport swaps; everything above it must be behavior-identical. Walk:

**(a) The de-interleave fires at the exact metaint boundary.**
`readAudio`'s counter arithmetic (`:1051-1068`) is untouched and transport-blind:
it counts bytes *returned by rawRead*. Alignment therefore reduces to one
invariant — **the first byte rawRead ever serves is body offset 0** — plus
`icy_metaint_` being parsed correctly. On Windows, WinINet guarantees the
invariant by consuming exactly the headers. In the twin, the guarantee is §2.5:
the header reader hands every post-terminator byte into `raw_buf_` before
`connect()` returns. There is no other place bytes can be created or lost:
below the boundary, `curl_easy_recv` returns raw body bytes 1:1 (TLS framing is
transparent; no chunked framing can exist under an HTTP/1.0 request, and a
server that chunks anyway is rejected at connect). A byte-offset slip would
corrupt both audio and metadata — this invariant is exercised explicitly by the
fixture test (§5), where a slip garbles the decode and fails prebuffer/title
asserts deterministically.

**(b) stop_ still aborts promptly; close() cannot hang.**
Baseline abort ladder: `close()` sets `stop_` → `onRead:1136` returns
`MA_AT_END` on next decoder pull → producer loop head `:1173/:1251` exits →
join. The only blocking window is a network read in flight; Windows bounds it
at 8 s (RECEIVE_TIMEOUT). The twin polls `stop_` every ≤100 ms inside the
recv-wait loop AND keeps the 8 s dry-read bound — so every baseline abort path
exists with equal-or-better latency, and the producer's join in `close()`
(`:86`) is bounded exactly as before. Nothing new blocks: `curl_easy_recv`
never blocks (CURLE_AGAIN), `poll` is 100 ms-sliced, send/header phases are
8 s-bounded with `stop_` polls. The one blind spot that remains — connect
itself — is the baseline's own parked gap, preserved deliberately.

**(c) Re-pin / reconnect behaves identically.**
Both producer reconnect paths and the re-pin block interact with the transport
only via `disconnect()` → `connect()` (§1a/§1b). The twin's `disconnect()`
cleans up the easy handle and nulls `hConn_` (same post-state as `:147-148`);
its `connect()` ends with the identical state re-init the Windows block does at
`:129-131` (fresh counter, fresh buffer — plus the leftover-prime, which at
reconnect is again exactly "body offset 0 first"). `prebuffered_`/`ringClear`
sequencing in the producers is untouched shared code. A failed twin connect
returns false exactly where `InternetOpenUrlA` returned null — backoff/cap
logic unchanged.

**(d) The blocking-semantics delta, named.**
`InternetReadFile` = blocking-with-timeout; `curl_easy_recv` = non-blocking +
caller-supplied wait. The wait loop is the only genuinely new concurrency-free
code on the hot path; its two failure modes are busy-spin (prevented: `poll`
blocks the slice) and missed TLS-buffered data (prevented: recv-first order,
§2). It runs on the producer thread only — same single-thread ownership the
WinINet handles had (`StreamSource.h:213` comment) — so no new sharing.

**(e) Shared-code diff is zero.**
`onRead`, `readAudio`, `rawReadExact`, `parseIcyMetadata`, both producers, ring
machinery, `readFrames`, `close()`: no hunks. The `hConn_`-as-CURL* choice is
what makes the `onRead:1136` and `rawRead:1012` guards work unmodified.

---

## 4. Sacred-boundary audit plan (run before commit, results in the Done entry)

1. **Windows byte-verbatim:** `git diff src/StreamSource.cpp` shows every
   `#ifdef _WIN32` block ONLY as unchanged context — zero changed lines inside
   the gates. (The rawRead twin repeats the 5-line serve tail rather than
   re-spanning the gate around shared lines — the win block's bytes and its
   gate span both stay identical.)
2. **Sacred-symbol grep over the full diff:** `ringClear|ringWrite|ringRead|
   ringAvailable|prebuffered_|readFrames|producerWorker|producerWorkerAAC|
   onRead|readAudio|rawReadExact|parseIcyMetadata|icy_counter_|
   next_decoder|track_swap` → hits allowed only as unchanged context or inside
   the new `#else` bodies; zero modified lines.
3. **Enumerated change classes — everything in the diff must fall in one:**
   - (i) new `#else` transport bodies in `connect()` / `rawRead()` /
     `disconnect()` + file-local Linux helpers (URL parse, request write,
     header read/parse) + gated `<curl/curl.h>` include;
   - (ii) comment updates on the slice-1 "refuses Continuous" placeholders
     (removed) and the `void*` member note in StreamSource.h;
   - (iii) additive test files + their CMake entries (§5);
   - (iv) docs.
   Anything outside these four classes = stop and re-review.
4. **No new includes outside the gate;** `curl/curl.h` appears under
   `#ifndef _WIN32` only. Windows preprocessed output for StreamSource.cpp is
   bit-for-bit unaffected (verifiable with `gcc -E -D_WIN32` diff if wanted).
5. **CMake:** expected zero main-target churn (curl already linked on Linux,
   CMakeLists.txt:186); only test additions.

---

## 5. Test + gate plan

**Probe first (per the standing rule, before any StreamSource edit):** a
standalone `icy_probe.cpp` (scratch tool, same spirit as radio_probe.cpp)
exercising the exact CONNECT_ONLY recipe against real stations: one plain-http
SHOUTcast (`ICY 200` status), one https Icecast, one with `icy-metaint`, one
without, one redirecting. It prints the parsed status/headers/metaint, verifies
StreamTitle extraction at the metaint boundary for ~30 s of stream, and reports
leftover-bytes count at header parse (proving §2.5 fires in the wild). The
recipe is integrated only after the probe passes all five shapes.

**Portable ctest — `icy_pipeline_test` (new, BOTH platforms):** the ICY twin of
http_cancel_test's localhost fixture + hls_pipeline_test's real-pipeline drive.
A localhost TCP fixture server (existing winsock/POSIX shim pattern, including
the shutdown-then-close stopListener lesson) speaks ICY: asserts the client
request contained `Icy-MetaData: 1`, replies `ICY 200 OK` + `icy-metaint: 4096`,
then serves LAME-encoded MP3 sine audio (generated at test start — no binary
fixtures, the hls_pipeline_test precedent; LAME is already a link dep)
interleaved with metadata blocks at exact 4096-byte boundaries. Cases:
1. **Plays:** real `StreamSource::open()` on the Continuous path → prebuffered,
   `readFrames` yields non-silence.
2. **Metaint alignment + StreamTitle:** titles injected in successive metadata
   blocks appear in `nowPlaying()` exactly; any byte-slip garbles the MP3
   decode and fails case 1/2 deterministically.
3. **Prompt stop:** `close()` mid-stream returns bounded (assert <2 s on Linux,
   <10 s on Windows — the per-platform documented bounds).
4. **Reconnect:** fixture closes the connection mid-stream, accepts a new one →
   producer self-heals (prebuffered again, audio resumes) — drives the
   `:1208-1222`/`:1268-1283` loops through the twin for real.
5. **No-metaint passthrough:** response without the header → plays.

Note what this buys: it runs the **Windows WinINet path** against the same
fixture — the slice-0 "ICY cannot be replayed headlessly, live-gate only"
honest-limit closes, and the Windows baseline gets its first standing
regression net *before* the twin lands (run it on baseline first, then with
the twin — the slice-B test-first pattern).

**Hard live gate on Linux (WSL2, like the iHeart gate):**
- A real ICY/SHOUTcast station plays in the TUI via ^U (one http, one https):
  audible output measured off the Pulse sink monitor (RMS + non-zero ratio).
- In-band StreamTitle updates the now-playing/[LIVE] readout on a track change
  (lessons.md caveat stands: one missed transition can be the station's
  silence — verify across two changes before suspecting the client).
- Stop / station-switch aborts promptly — no multi-second hang (observed, not
  assumed: time the switch).
- Reconnect self-heal observed (deterministically via the fixture test; live
  drop optional).

**Windows regression:** full ctest (now including icy_pipeline_test over
WinINet) + the same live ICY station on 7of9 plays unchanged (this machine can
verify playing_/nowPlaying/[stream] log; ear spot-check with Dos optional).
Must be a non-event — the path is byte-verbatim (§4.1 proves it statically,
this proves it behaviorally).

---

## 6. Scope fence

ONLY the ICY continuous transport: `connect()`/`rawRead()`/`disconnect()`
Linux `#else` bodies + file-local helpers + the probe + `icy_pipeline_test`.
NOT touched: the HLS path (slice 2, done), `readAudio`/de-interleave (neutral,
stays), ring machinery / producers / `readFrames` (sacred), UI, IHttp seam,
anything Windows. No new members beyond reusing the existing `void*` `hConn_`
(and `hInet_` stays inert-null on Linux). No behavior improvements smuggled in
(the stalled-connect interrupt stays parked; proxy stays a named residual).
