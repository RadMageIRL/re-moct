# Phase 3 slice 2 — design: core::IHttp → libcurl (+ vendored MD5, + portable cancel test)

Design-first pass, NO code yet. For Dos sign-off. Parity reference =
`src/platform/win/HttpWinInet.cpp` read end-to-end this session; curl facts
verified against Trixie's libcurl **8.14.1** (REDIR_PROTOCOLS_STR ✓, share-API
connection caching ✓, XFERINFOFUNCTION ✓).

## 0. State confirmed
Slices 0+1 (+ quit/resize follow-up) landed, matrix green through `71eff4c`,
tree clean. ICY transport is slice 3 and appears here ONLY as "not this slice."
The 150-sector preamble is physical (HydrogenAudio topic 97603) — untouched,
unre-litigated.

## 1. Shape of the impl
**New file `src/platform/linux/HttpCurl.cpp`** (`platform::lnx::CurlHttp` /
`CurlSession`) — the sibling of HttpWinInet.cpp, same TU layout: file-static
`executeRequest()` shared by one-shot and session paths; `core::http()/
setHttp()` defined here with the EXACT g_override shape HttpWinInet has (tests
inject identically on both platforms). **SeamStubs.cpp drops its StubHttp +
http()/setHttp() bridges** (they move here); Ipc/Notify/CdIo stubs stay.
CMake: `find_package(CURL REQUIRED)` + link, UNIX only. **WinINet untouched —
Windows is zero-diff this slice** (except LastFm's MD5, §4).

`curl_global_init(CURL_GLOBAL_DEFAULT)` via `std::call_once` before first use
(8.14 is internally thread-safe there, but explicit is auditable).

## 2. Primitive-by-primitive mapping

| IHttp surface | WinINet (baseline) | libcurl mapping |
|---|---|---|
| one-shot `fetch()` | fresh `InternetOpen` per call, closed after | fresh `curl_easy_init` per call, `curl_easy_cleanup` after — same "no reuse across one-shots" wire shape |
| `IHttpSession` keep-alive | ONE held `InternetOpen` + `KEEP_CONNECTION` per fetch; **concurrent fetches from multiple threads are contract** | **a held `CURLSH` share handle** (`CURL_LOCK_DATA_CONNECT` + `DNS` + `SSL_SESSION`, mutex lock callbacks); each fetch = its own easy handle bound via `CURLOPT_SHARE`. Connection cache lives in the share ⇒ keep-alive reuse ACROSS fetches AND thread-safe concurrency. (A single shared easy handle was REJECTED: curl easy handles can't run two transfers at once — serializing would change the contract.) |
| cancel token (`req.cancel`, polled pre-open + per chunk) | manual poll in `readBody` | pre-check before `curl_easy_perform` (the "pre-cancelled = zero network" property), then `CURLOPT_XFERINFOFUNCTION` returning nonzero → `CURLE_ABORTED_BY_CALLBACK` → `finalizeCancelled()` (ok=false, body cleared). **Accepted-better residual:** curl's progress callback also fires DURING connect, so a cancel interrupts a stalled connect on Linux where WinINet can't (the parked stalled-connect item) — within the contract's "best-effort prompt," strictly better, documented. |
| `RedirectPolicy::FollowAll` | WinINet default follow | `CURLOPT_FOLLOWLOCATION=1`, `MAXREDIRS=30` |
| `FollowNone` | `NO_AUTO_REDIRECT` | `FOLLOWLOCATION=0` (3xx returned as final, same as baseline) |
| `FollowSameScheme` | `IGNORE_REDIRECT_TO_HTTP\|HTTPS` (blocks both cross-scheme directions) | `FOLLOWLOCATION=1` + `CURLOPT_REDIR_PROTOCOLS_STR` = the ORIGINAL scheme only ("https" or "http"). Residual: on a cross-scheme redirect WinINet errors at the redirect step, curl fails with `CURLE_UNSUPPORTED_PROTOCOL` — same observable outcome (ok=false), different internal error. CAA's https→https follow (the real consumer) is identical. |
| `max_body` cap / `truncated` | read loop breaks past cap | write callback appends; past cap sets a per-request `truncated` flag and returns a short count → curl aborts (`CURLE_WRITE_ERROR`, disambiguated from real write errors by the flag) → `finalizeBody()` applies keep-vs-reject exactly as Windows |
| `reject_truncated` | pure `finalizeBody()` | same pure call — no platform code involved |
| `read_error` (mid-body transport error) | `InternetReadFile` fails after partial body | perform returns an error (RECV_ERROR/PARTIAL_FILE/…) with body non-empty and not-cancel/not-cap → `read_error=true`, partial KEPT, ok per finalizeBody — the hls-gates-on-it/six-sites-keep-partial split preserved |
| `status` | `HttpQueryInfo(STATUS_CODE)`; 4xx body still read | `CURLINFO_RESPONSE_CODE`; `FAILONERROR` stays OFF (curl default) so 4xx bodies are read — same |
| `final_url` | `INTERNET_OPTION_URL` post-redirect | `CURLINFO_EFFECTIVE_URL` |
| headers (+ `content_type` FIRST, then caller's — byte order preserved) | CRLF-joined wide string | `curl_slist_append` in the same order; Content-Type first |
| UA (`user_agent` empty → app default) | same `kDefaultUA` string | `CURLOPT_USERAGENT`, byte-identical default string |
| POST body **raw UTF-8, never widened** | `HttpSendRequest` byte buffer | `CURLOPT_POSTFIELDS` + `POSTFIELDSIZE` — byte buffer; no wide APIs exist on this path at all. Non-GET/POST verbs via `CURLOPT_CUSTOMREQUEST` (none used today) |
| `timeout_ms` (WinINet CONNECT+RECEIVE[+SEND on GET] — per-operation waits) | `InternetSetOption` ×3 | `CURLOPT_CONNECTTIMEOUT_MS = timeout_ms` + **`LOW_SPEED_LIMIT=1`/`LOW_SPEED_TIME=ceil(ms/1000)`** — i.e., abort when the transfer stalls for ~timeout, the closest analog of a receive timeout. **Deliberately NOT `CURLOPT_TIMEOUT_MS`** (whole-transfer deadline — would wrongly kill a healthy 10 s cover-art download on a slow link; WinINet's receive timeout never did that). GET-only SEND nuance is moot (curl has no separate send timeout; bodies are tiny scrobbles). |
| `pragma_no_cache` | `INTERNET_FLAG_PRAGMA_NOCACHE` | explicit `Pragma: no-cache` request header |
| `RELOAD`/`NO_CACHE_WRITE` (always set) | bypass/skip WinINet's LOCAL cache | **no-op**: curl has no client cache to bypass. Residual: request-header bytes may differ slightly from WinINet's cache-flag side effects; no consumer or service keys on this. Live gates confirm. |
| proxy (`PRECONFIG` = system) | IE/system proxy | curl's default env-var convention (`http_proxy`/`https_proxy`/`no_proxy`) — the Linux system convention. Accepted platform-convention residual. |
| TLS from scheme (`INTERNET_FLAG_SECURE` derived) | explicit flag | automatic from the URL scheme; system CA store (OpenSSL). Plain-http AR/CTDB gets no TLS machinery — same as the scheme-derived rule. |
| `openSession` nullptr on failure | InternetOpen fails | share/easy alloc failure → nullptr (caller retries, as baselines did) |

**Per-site parity check** (the lessons.md rule: parity lives at call sites):
MBLookup 4 MB cap/no timeout/default UA/status-ignored; RadioBrowser 6 s;
LastFm+LB 8 s POST raw-UTF-8; CoverArt 10 MB reject+clear, CAA FollowSameScheme;
CDRipper AR/CTDB plain-http + CTDB short UA; hls 8 MB/pragma/cancel/read_error;
IHeart 5 s session/pragma/4 MB. All flow through the generic field mapping
above — no per-site code in the impl, same as Windows.

## 3. What does NOT map cleanly (flagged, with the chosen answer)
1. **Session concurrency** → solved with the share-handle design (table above);
   the naive easy-handle-per-session was rejected.
2. **Receive timeout** → LOW_SPEED_LIMIT/TIME approximation; NOT whole-transfer
   timeout. Granularity is seconds (curl API), so a 6000 ms timeout becomes a
   6 s stall guard — behaviorally equivalent at every real call site.
3. **WinINet cache flags** → no-op on curl (no client cache exists).
4. **Cancel during connect** → works on Linux (progress callback fires in the
   connect phase), can't on Windows — accepted-better, documented.
5. **System proxy semantics** → per-platform convention (env vars vs IE
   settings) — accepted.

## 4. Vendored MD5 (both platforms, one code path)
- Vendor **Alexander Peslyak's public-domain MD5** (the Openwall
  implementation — the standard clean vendoring choice, ~250 lines, zero
  deps, no OpenSSL) as **`lib/md5.h`**, header-only-ified (inline) so no new
  TU enters CMake for the main target or scrobble_request_test. Add a tiny
  `md5::hex(const std::string&) -> std::string` convenience.
- `LastFm::md5Hex` uses it on **BOTH platforms**: the CryptoAPI block and the
  slice-1 Linux placeholder are both deleted; `<wincrypt.h>` leaves LastFm.cpp.
  This is the one deliberate Windows behavior-adjacent change of the slice —
  and it's identity-preserving by construction (MD5 is MD5), which the gates
  prove rather than assume:
- **Proof, three layers:** (1) new pure `md5_test` ctest target — the seven
  RFC 1321 reference vectors + a UTF-8 multibyte vector ("Björk…" bytes) —
  runs in BOTH matrix jobs; (2) Windows regression: live scrobble round-trip
  lands on Last.fm + ListenBrainz (api_sig accepted = byte-identical hash in
  practice); (3) Linux gate: same round-trip from the Linux build.
- Note for the record: MD5 here is Last.fm's protocol-mandated API signature,
  not a security choice of ours.

## 5. Portable cancel test (replaces the Windows-only twin idea with ONE test)
`http_cancel_test.cpp` becomes **portable** — same three scenarios, same
assertions, one file, run by BOTH matrix jobs (better than a POSIX sibling:
zero drift by construction):
- A ~30-line platform shim at the top: `SockT` (SOCKET vs int),
  `closeSock()`, `initNet()/cleanupNet()` (WSAStartup vs no-op), POSIX
  `SO_RCVTIMEO/SNDTIMEO` take `struct timeval` (vs DWORD ms), `socklen_t`
  for getsockname, `port::sleepMs` for Sleep.
- **Linux-specific correctness:** `MSG_NOSIGNAL` on sends (or `signal(SIGPIPE,
  SIG_IGN)` in main) — a fixture send to a dead client must not kill the test
  process; Windows has no SIGPIPE.
- Scenario B (keep-alive, `connections == 1`) is the live proof that the
  share-handle design actually pools: two easy handles, one session, ONE TCP
  connection observed at the fixture.
- CMake: moves out of `if(WIN32)` into common; links HttpWinInet.cpp on
  WIN32 / HttpCurl.cpp on UNIX.

## 6. Test plan beyond the cancel test
- **Port to the Linux matrix now** (all fake-driven consumer tests that only
  stayed Windows-only because they LINK the WinINet impl):
  `scrobble_request_test`, `iheart_request_test`, `group_c_request_test`
  (link HttpCurl on UNIX — they inject FakeHttp via setHttp, so they test
  consumer request-building identically), and **`hls_pipeline_test`** (in the
  approved slice gate: FakeHttp through the REAL StreamSource HLS pipeline —
  FDK is present on Trixie; any stray `Sleep` → `port::sleepMs`).
- **Stay with their slices:** `cd_toc/cd_pipeline` (slice 6), `discord_ipc`
  (slice 4), `xfade` (needs audio device — WSL2 has Pulse; will try, SKIP 77
  is honored if not).
- New `md5_test` (§4). Expected matrix counts: Windows 14 (13+md5), Linux
  grows from 4 to **10** (4 + md5 + cancel + scrobble/iheart/group_c + hls).

## 7. Live gates
**Linux** (WSL2 + Dos's VM if desired):
1. **RadioBrowser Ctrl+U in the TUI** returns real stations (the Dos-found
   "expected-dead" key comes alive).
2. **Digital iHeart HLS plays** (URL via saved radio entry / known zc URL;
   pactl sink-input + nowPlaying updates). ICY stations still refuse with the
   slice-3 message — correct.
3. **Scrobble round-trip** lands on Last.fm AND ListenBrainz with accents
   intact (Björk-class track — the raw-UTF-8 wire rule). *Logistics: needs
   Dos's API keys on the Linux side — copying remoct's config to the WSL2/VM
   config dir before the gate (stays on his machines).*
4. **MB lookup**: the UI flow needs a physical CD (ICdIo is stubbed until
   slice 6), so the honest Linux gate is a **standalone probe** (probe-first
   discipline): link MBLookup + real HttpCurl, feed Relish's known TOC
   (T1@182 + the 12 LBAs from cd_toc_test), assert release metadata resolves.
   The full in-app path re-verifies at slice 6 with the real drive.
**Windows** (regression): live scrobble round-trip (MD5 vendoring is the one
Windows-visible change) + ctest 14/14; no producer-path timing touched — the
standing ear-check from slice 1 still stands with Dos, nothing new added.
**Matrix**: green both sides with the expanded test lists.

## 8. Order of work (after sign-off)
1. `lib/md5.h` + `md5_test` + LastFm switch → Windows build + ctest + live
   Windows scrobble (proves MD5 before anything Linux depends on it).
2. `HttpCurl.cpp` + SeamStubs shrink + CMake → Linux build; portable
   `http_cancel_test` next (proves cancel/keep-alive before consumers).
3. Port the four tests to the matrix.
4. Live gates (§7), then docs + commits (code/docs separate), push, matrix.
