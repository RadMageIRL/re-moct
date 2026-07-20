#include "StreamSource.h"
#include "Log.h"
#include "Version.h"     // REMOCT_VERSION (single source) for the raw ICY/HLS User-Agent
#include "StringUtils.h"
#include "HlsPrime.h"    // hlsFirstMusicBoundary — prime-to-music-boundary scan (clip fix)
#include "IHeartDeepLog.h"
#include "PortUtil.h"   // sleepMs/tickMs — expand to the baseline ::Sleep/::GetTickCount on Windows
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#ifndef _WIN32
// Phase 3 slice 3: the ICY twin's transport plumbing. Like WinINet on Windows,
// this is StreamSource-private raw transport — permanently outside the IHttp
// seam. curl CONNECT_ONLY gives TCP(+TLS); the ICY request/response and the
// byte stream are spoken by hand so the pull-read shape stays verbatim.
#include <curl/curl.h>
#include <poll.h>
#include <mutex>
#endif


// ── Tunables ─────────────────────────────────────────────────────────────────
// How long the now-playing reconciler may sit on the LIVE floor (digital mode)
// before we assume it has stalled (the dual-blind) and force a live-edge re-pin.
// Logs show healthy ad breaks clear the floor in ≤~30s; a real stall ran 7 min.
// 45s sits just above the healthy ceiling so normal breaks self-recover untouched
// while a genuine stall heals fast. Tune here.
// LIVE_STALL_MS moved to IHeartNowPlayingSM.h (used only by the now-playing SM).

// ─── Diagnostic log — routed through the shared operational logger (Log) ──────
static void slog(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log::write("stream", buf);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

bool StreamSource::open(const std::string& url) {
    close();                       // ensure any prior session is fully torn down

    url_ = url;
    last_error_.clear();
    setStop(false);                // clears stop_ + the HTTP-cancel mirror
    paused_.store(false);
    prebuffered_.store(false);
    playing_.store(false);
    position_sec_.store(0);
    frames_drained_.store(0);
    ring_write_.store(0);
    ring_read_.store(0);
    { std::lock_guard<std::mutex> lk(now_playing_mtx_); now_playing_.clear(); np_pub_q_.clear(); np_published_.clear(); }

    // Codec choice by URL (content-type sniffing is a later refinement).
    std::string lower = url;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    codec_ = (lower.find(".aac") != std::string::npos || lower.find("aac") != std::string::npos)
             ? Codec::AAC : Codec::MP3;

    // HLS detection: an .m3u8 is a manifest, not a byte stream. Route it through
    // the segment pump (mode_=HLS) and force the AAC worker — iHeart segments are
    // raw ADTS, so the existing FDK decode/resample path handles them unchanged.
    if (lower.find(".m3u8") != std::string::npos) {
        mode_  = Mode::HLS;
        codec_ = Codec::AAC;
    } else {
        mode_ = Mode::Continuous;
    }

    if (!connect()) {              // initial connection on the caller thread
        last_error_ = "connection failed";
        slog("open: connect FAILED url=%s", url.c_str());
        disconnect();
        return false;
    }
    slog("open: connect OK codec=%s url=%s",
         codec_ == Codec::AAC ? "AAC" : "MP3", url.c_str());

    if (codec_ == Codec::AAC)
        producer_thread_ = std::thread(&StreamSource::producerWorkerAAC, this);
    else
        producer_thread_ = std::thread(&StreamSource::producerWorker, this);
    return true;
}

void StreamSource::close() {
    setStop(true);                 // sets stop_ + the HTTP-cancel mirror
    // The producer owns the WinINet handles and decoder; it exits within one
    // read chunk because onRead() returns MA_AT_END as soon as stop_ is set,
    // then tears its own resources down. (Promptly interrupting a fully stalled
    // connection is a later hardening item — see note in producerWorker.)
    if (producer_thread_.joinable()) producer_thread_.join();
    playing_.store(false);
    prebuffered_.store(false);
}

// ─── Connection ───────────────────────────────────────────────────────────────

#ifndef _WIN32
// ── Slice 3: the Linux ICY transport twin (curl CONNECT_ONLY + easy_send/recv).
// Design + correctness argument: docs/phase3-slice3-design.md. Everything above
// rawRead() — de-interleave, ring feed, producers — is shared and unchanged;
// these helpers exist only to hand connect() a connected socket whose next
// byte is body offset 0 (THE alignment invariant) with icy-metaint parsed.
namespace {

// curl_global_init is process-wide; HttpCurl.cpp's guard is TU-private, so this
// TU carries its own (thread-safe since curl 7.84; a second init is refcounted).
std::once_flag g_icy_curl_init;
void icyEnsureCurlInit() {
    std::call_once(g_icy_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Wait one slice for the connection's socket. Timeout/EINTR both mean "try the
// recv/send again" — the caller's loop owns stop_/deadline policy.
void icyWaitSocket(CURL* h, short events, int slice_ms) {
    curl_socket_t s = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(h, CURLINFO_ACTIVESOCKET, &s) != CURLE_OK || s == CURL_SOCKET_BAD)
        return;
    struct pollfd pfd { s, events, 0 };
    (void)poll(&pfd, 1, slice_ms);
}

// Send the whole request, bounded (the WinINet SEND_TIMEOUT twin), stop-aware.
bool icySendAll(CURL* h, const std::string& data, const std::atomic<bool>& stop) {
    size_t off = 0;
    uint32_t start = port::tickMs();
    while (off < data.size()) {
        size_t sent = 0;
        CURLcode rc = curl_easy_send(h, data.data() + off, data.size() - off, &sent);
        if (rc == CURLE_OK) { off += sent; continue; }
        if (rc != CURLE_AGAIN) return false;
        if (stop.load()) return false;
        if ((long)(port::tickMs() - start) >= 8000) return false;
        icyWaitSocket(h, POLLOUT, 100);
    }
    return true;
}

// One connect+request+header hop of the ICY handshake.
struct IcyHop {
    CURL*       easy = nullptr;   // owned by the caller on ok
    int         metaint = 0;
    std::string leftover;         // bytes received PAST the header terminator = body offset 0..n
    std::string redirect;         // raw Location value when the hop 3xx'd
    bool        ok = false;       // 200 reached, easy/metaint/leftover valid
};

bool icyHop(const std::string& url, const std::atomic<bool>& stop, IcyHop& out) {
    icyEnsureCurlInit();
    CURL* h = curl_easy_init();
    if (!h) return false;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 1L);        // TCP + TLS (for https), no HTTP
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);// the WinINet CONNECT_TIMEOUT twin
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    // ALPN OFF: with it on, a CDN edge (cloudflare) negotiates HTTP/2 and the
    // hand-written HTTP/1.x request below is protocol garbage on that
    // connection — the server just closes. Without ALPN it must assume
    // HTTP/1.1. (Probe finding, 2026-07-03 — see the design doc.)
    curl_easy_setopt(h, CURLOPT_SSL_ENABLE_ALPN, 0L);
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        slog("icy connect: %s (%s)", curl_easy_strerror(rc), url.c_str());
        curl_easy_cleanup(h);
        return false;
    }

    // Build the request off curl's own URL parser (CURLU — nothing hand-rolled).
    // HTTP/1.0 deliberately: a 1.0 request forbids Transfer-Encoding: chunked,
    // so the response body is guaranteed to be the raw interleaved byte stream.
    std::string req;
    {
        CURLU* u = curl_url();
        if (!u || curl_url_set(u, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
            if (u) curl_url_cleanup(u);
            curl_easy_cleanup(h);
            return false;
        }
        char *host = nullptr, *port_s = nullptr, *path = nullptr, *query = nullptr;
        curl_url_get(u, CURLUPART_HOST, &host, 0);
        curl_url_get(u, CURLUPART_PORT, &port_s, 0);      // null unless explicit in the URL
        curl_url_get(u, CURLUPART_PATH, &path, 0);
        curl_url_get(u, CURLUPART_QUERY, &query, 0);      // may be null
        req = std::string("GET ") + (path ? path : "/")
            + (query ? (std::string("?") + query) : std::string())
            + " HTTP/1.0\r\nHost: " + (host ? host : "")
            + (port_s ? (std::string(":") + port_s) : std::string())
            + "\r\nUser-Agent: RE-MOCT/" REMOCT_VERSION " (https://github.com/RadMageIRL/re-moct)\r\n"
              "Icy-MetaData: 1\r\n\r\n";
        if (host)   curl_free(host);
        if (port_s) curl_free(port_s);
        if (path)   curl_free(path);
        if (query)  curl_free(query);
        curl_url_cleanup(u);
    }
    if (!icySendAll(h, req, stop)) { curl_easy_cleanup(h); return false; }

    // Read the response headers OURSELVES (CONNECT_ONLY sends no HTTP, so no
    // library consumed them) — that ownership is what makes "ICY 200 OK"
    // tolerance ours. Bounded 16KB / 8s, stop-aware, tolerate bare \n\n.
    std::string hdr;
    size_t term = std::string::npos, tlen = 0;
    uint32_t start = port::tickMs();
    char buf[2048];
    while (hdr.size() < 16384) {
        size_t got = 0;
        rc = curl_easy_recv(h, buf, sizeof(buf), &got);
        if (rc == CURLE_AGAIN) {
            if (stop.load() || (long)(port::tickMs() - start) >= 8000) {
                curl_easy_cleanup(h);
                return false;
            }
            icyWaitSocket(h, POLLIN, 100);
            continue;
        }
        if (rc != CURLE_OK || got == 0) {                 // error or closed pre-headers
            slog("icy connect: header read failed (%s)", curl_easy_strerror(rc));
            curl_easy_cleanup(h);
            return false;
        }
        hdr.append(buf, got);
        if ((term = hdr.find("\r\n\r\n")) != std::string::npos) { tlen = 4; break; }
        if ((term = hdr.find("\n\n"))     != std::string::npos) { tlen = 2; break; }
    }
    if (term == std::string::npos) { curl_easy_cleanup(h); return false; }

    std::string head = hdr.substr(0, term);
    std::string lh   = head;
    for (char& ch : lh) ch = (char)std::tolower((unsigned char)ch);
    auto hdrval = [&](const char* name) -> std::string {  // case-insensitive lookup
        std::string key = std::string("\n") + name + ":";
        size_t p = lh.find(key);
        if (p == std::string::npos) return {};
        p += key.size();
        size_t e = head.find_first_of("\r\n", p);
        std::string v = head.substr(p, e - p);
        size_t s = v.find_first_not_of(" \t");
        return s == std::string::npos ? std::string() : v.substr(s);
    };

    size_t eol = head.find_first_of("\r\n");
    std::string sl = head.substr(0, eol == std::string::npos ? head.size() : eol);
    bool ok200 = sl.rfind("ICY 200", 0) == 0 || sl.rfind("HTTP/1.0 200", 0) == 0
              || sl.rfind("HTTP/1.1 200", 0) == 0;
    bool redir = (sl.rfind("HTTP/1.0 3", 0) == 0 || sl.rfind("HTTP/1.1 3", 0) == 0);
    if (redir) {
        out.redirect = hdrval("location");
        curl_easy_cleanup(h);                             // leftover of a 3xx hop = its html body; discarded with the hop
        if (out.redirect.empty()) { slog("icy connect: 3xx without Location"); return false; }
        return true;
    }
    if (!ok200) {
        slog("icy connect: unexpected status \"%s\"", sl.c_str());
        curl_easy_cleanup(h);
        return false;
    }
    // A conformant server cannot chunk an HTTP/1.0 response; one that does
    // anyway would break the metaint arithmetic — refuse loudly, never garble.
    std::string te = hdrval("transfer-encoding");
    if (te.find("chunked") != std::string::npos) {
        slog("icy connect: server sent Transfer-Encoding: chunked to an HTTP/1.0 request -> refusing");
        curl_easy_cleanup(h);
        return false;
    }

    out.easy     = h;
    out.metaint  = std::atoi(hdrval("icy-metaint").c_str());
    out.leftover = hdr.substr(term + tlen);               // body offset 0..n — MUST be preserved
    out.ok       = true;
    return true;
}

} // namespace
#endif // !_WIN32

bool StreamSource::connect() {
    if (mode_ == Mode::HLS) return hlsConnect();   // segment-pump setup (no persistent ICY conn)

#ifdef _WIN32
    // ── ICY/continuous transport: raw WinINet, permanently outside the IHttp
    //    seam by design. The block below is the baseline, verbatim. ──────────
    hInet_ = InternetOpenA("RE-MOCT/" REMOCT_VERSION " (https://github.com/RadMageIRL/re-moct)",
                           INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet_) return false;

    // Bound the connect/receive so a dead or wedged station can't block the
    // connect worker (and thus a queued station switch) indefinitely.
    DWORD to = 8000;  // ms
    InternetSetOptionA(hInet_, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet_, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet_, INTERNET_OPTION_SEND_TIMEOUT,    &to, sizeof(to));

    // InternetOpenUrlA parses the scheme; https is negotiated with TLS automatically
    // (same path the MusicBrainz/Discogs lookups already use).
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION;
    // Ask the server to interleave Shoutcast/Icecast now-playing metadata.
    const char* icy_hdr = "Icy-MetaData: 1\r\n";
    hConn_ = InternetOpenUrlA(hInet_, url_.c_str(), icy_hdr, (DWORD)-1L, flags, 0);
    if (!hConn_) {
        InternetCloseHandle(hInet_);
        hInet_ = nullptr;
        return false;
    }

    // icy-metaint = audio bytes between metadata blocks (absent/0 = no metadata).
    icy_metaint_ = 0;
    char  qbuf[64] = "icy-metaint";
    DWORD qlen = sizeof(qbuf), qidx = 0;
    if (HttpQueryInfoA(hConn_, HTTP_QUERY_CUSTOM, qbuf, &qlen, &qidx))
        icy_metaint_ = atoi(qbuf);
    icy_counter_ = icy_metaint_;
    raw_buf_.clear();
    raw_pos_ = 0;
    slog("connect: icy_metaint=%d", icy_metaint_);
    return true;
#else
    // ── Slice 3: the ICY twin — curl CONNECT_ONLY, hand-spoken ICY request,
    // hand-parsed response headers (helpers above). Redirects followed by hand
    // (WinINet followed them internally), resolved via the portable
    // hlsResolveUrl twin, capped at 5 hops. ─────────────────────────────────
    std::string cur = url_;
    for (int hops = 0; ; ++hops) {
        IcyHop hop;
        if (!icyHop(cur, stop_, hop)) return false;
        if (!hop.redirect.empty()) {
            if (hops >= 5) { slog("connect: redirect cap exceeded"); return false; }
            cur = hlsResolveUrl(cur, hop.redirect);       // absolute passes through
            slog("connect: redirect -> %s", cur.c_str());
            continue;
        }
        // Publish the connection in the slice-1 void* twin member — the shared
        // guards (rawRead's and onRead's hConn_ null checks) work unmodified.
        hConn_       = hop.easy;                          // hInet_ stays inert on Linux
        icy_metaint_ = hop.metaint;
        // Post-connect state init, same as the Windows block above — except
        // raw_buf_ is primed with the bytes the header reader received past the
        // terminator: THE offset-0 invariant. The first byte rawRead ever
        // serves is body offset 0, so readAudio()'s metaint arithmetic stays
        // exact. (Windows never sees leftover — WinINet consumes precisely the
        // headers.)
        icy_counter_ = icy_metaint_;
        raw_buf_.assign(hop.leftover.begin(), hop.leftover.end());
        raw_pos_ = 0;
        slog("connect: icy_metaint=%d", icy_metaint_);
        return true;
    }
#endif
}

void StreamSource::disconnect() {
#ifdef _WIN32
    if (hConn_) { InternetCloseHandle(hConn_); hConn_ = nullptr; }
    if (hInet_) { InternetCloseHandle(hInet_); hInet_ = nullptr; }
#else
    // Slice 3: hConn_ holds the ICY twin's CURL easy handle (hInet_ is inert).
    if (hConn_) { curl_easy_cleanup(static_cast<CURL*>(hConn_)); hConn_ = nullptr; }
#endif
    hls_session_.reset();   // HLS: drop the keep-alive session (fresh one per
                            // re-handshake/re-pin — the lifetime the raw handle had)
}

// ─── HLS increment 1: resolve / poll / fetch (standalone, no audio path) ──────
// Walks the same chain VLC does: master -> variant (media playlist) -> .aac
// segments. Relative refs resolve against each response's post-redirect URL so
// the rj-tok / 50_<session> token propagates. All output goes to slog ([stream],
// %TEMP%\remoct-*.log). None of the continuous-stream path is touched.

bool StreamSource::hlsEnsureSession() {
    if (hls_session_) return true;
    core::HttpSessionConfig cfg;
    cfg.user_agent = "";     // impl default == the exact UA the raw handle used
    cfg.timeout_ms = 8000;   // mirror connect() timeouts (connect/receive/send)
    hls_session_ = http_->openSession(cfg);   // slice c: injected host services (was core::http())
    if (!hls_session_) { slog("hlsEnsureSession: openSession FAILED"); return false; }
    return true;
}

// Short-lived GET over the shared keep-alive session (via the core::IHttp seam).
// Captures body (text and/or bytes) and, optionally, the post-redirect URL.
// stop_ rides in as the request's cancel token — the transport polls it per chunk,
// preserving the baseline's prompt mid-segment abort on stop.
bool StreamSource::hlsHttpGet(const std::string& url, std::string* out_text,
                              std::vector<uint8_t>* out_bytes, std::string* out_final_url) {
    if (!hls_session_) return false;
    core::HttpRequest req;
    req.url             = url;
    req.pragma_no_cache = true;                 // baseline INTERNET_FLAG_PRAGMA_NOCACHE
    req.max_body        = 8u * 1024u * 1024u;   // baseline 8 MB cap-and-keep
    req.cancel          = &http_cancel_;         // plain int32 flag; ABI cancel type
    core::HttpResponse res = hls_session_->fetch(req);

    if (res.cancelled) return false;            // our own stop, not a network failure
    if (!res.ok) { slog("hlsGet: open FAILED url=%s", url.c_str()); return false; }
    if (res.read_error) {                       // baseline fails the call on a mid-body error
        slog("hlsGet: read err url=%s", url.c_str());
        return false;
    }
    if (res.status >= 400) {                    // non-2xx => session expiry/dead
        slog("hlsGet: HTTP %ld url=%s", res.status, url.c_str());
        return false;
    }
    if (res.truncated) slog("hlsGet: oversized body, abort");   // cap-and-keep, as baseline

    if (out_final_url)                          // URL after redirects (token lives here)
        *out_final_url = res.final_url.empty() ? url : res.final_url;
    if (out_text)  *out_text = res.body;
    if (out_bytes) out_bytes->assign(res.body.begin(), res.body.end());
    return true;
}

// First non-comment, non-blank line of a playlist (a URI).
std::string StreamSource::hlsFirstUri(const std::string& body) {
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(i, e - i);
        while (!line.empty() && (line.back()=='\r'||line.back()==' '||line.back()=='\t')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s != std::string::npos) line = line.substr(s);
        if (!line.empty() && line[0] != '#') return line;
        i = e + 1;
    }
    return {};
}

std::string StreamSource::hlsResolveUrl(const std::string& base, const std::string& ref) {
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;  // absolute
#ifdef _WIN32
    char out[2048]; DWORD len = sizeof(out);
    if (InternetCombineUrlA(base.c_str(), ref.c_str(), out, &len, ICU_NO_ENCODE))
        return std::string(out, len);
    return ref;                                // fallback: hand back as-is
#else
    // Portable twin of InternetCombineUrlA for the manifest shapes this call
    // actually sees: scheme-relative ("//host/…"), host-relative ("/path"),
    // and directory-relative refs against the (query-stripped) base.
    size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return ref;
    if (ref.rfind("//", 0) == 0)                       // scheme-relative
        return base.substr(0, scheme_end + 1) + ref;
    size_t host_start = scheme_end + 3;
    if (!ref.empty() && ref[0] == '/') {               // host-relative
        size_t path_start = base.find('/', host_start);
        return (path_start == std::string::npos ? base : base.substr(0, path_start)) + ref;
    }
    std::string b = base;                              // directory-relative
    size_t q = b.find_first_of("?#", host_start);
    if (q != std::string::npos) b = b.substr(0, q);
    size_t last = b.rfind('/');
    if (last == std::string::npos || last < host_start) return b + "/" + ref;
    return b.substr(0, last + 1) + ref;
#endif
}

bool StreamSource::hlsResolveMaster() {
    std::string body, final_url;
    if (!hlsHttpGet(hls_.master_url, &body, nullptr, &final_url)) {
        slog("hlsResolveMaster: GET master FAILED"); return false;
    }
    if (body.find("#EXT-X-STREAM-INF") != std::string::npos) {        // master w/ variants
        std::string variant = hlsFirstUri(body);
        if (variant.empty()) { slog("hlsResolveMaster: no variant URI"); return false; }
        hls_.variant_url = hlsResolveUrl(final_url, variant);
        slog("hlsResolveMaster: variant=%s", hls_.variant_url.c_str());
    } else if (body.find("#EXTINF") != std::string::npos) {           // already a media playlist
        hls_.variant_url = final_url;
        slog("hlsResolveMaster: media playlist direct url=%s", final_url.c_str());
    } else {
        slog("hlsResolveMaster: unrecognized body (%zu bytes)", body.size());
        return false;
    }
    hls_.last_seq = 0;                          // fresh session -> reset sequence baseline
    return true;
}

bool StreamSource::hlsPollMedia() {
    std::string body, final_url;
    if (!hlsHttpGet(hls_.variant_url, &body, nullptr, &final_url)) {
        slog("hlsPollMedia: GET variant FAILED (session likely expired)"); return false;
    }
    size_t p = body.find("#EXT-X-TARGETDURATION:");
    if (p != std::string::npos) { int td = atoi(body.c_str()+p+22); if (td > 0) hls_.target_dur_ms = td*1000; }

    bool have_seq = false; uint64_t media_seq = 0;
    p = body.find("#EXT-X-MEDIA-SEQUENCE:");
    if (p != std::string::npos) { media_seq = strtoull(body.c_str()+p+22, nullptr, 10); have_seq = true; }

    bool disc = body.find("#EXT-X-DISCONTINUITY") != std::string::npos;

    // Live-edge re-pin (digital only). An iHeart ad pod begins with a
    // discontinuity. Because our digital session is independent (its own
    // listenerId / SSAI stitch), our ad pod can run longer than the broadcast
    // avail, so playing it through sequentially leaves us drifting behind the
    // shared live program — the web player seeks back to the live edge after the
    // pod, we don't. So on AD ONSET (first discontinuity of a break) we ask the
    // producer to re-handshake, which re-primes to the live edge and rejoins the
    // program in step with the web player. One-shot per break: fire on the first
    // discontinuity, then suppress for a cooldown (covers a whole pod, which
    // contains several discontinuities), then re-arm for the next break.
    if (digital_active_.load()) {
        // F6 re-pin mode gates the immediate ad-onset re-pin (f6-repin-finalize).
        // Ad-Escape (1) needs hard evidence (a PAID spot or cartcut churn); Hybrid (2)
        // also accepts the disc landing on a spot/ad segment, parsed FRESH here - at a
        // genuine ad onset the first ad segment arrives WITH the discontinuity, while a
        // talk-show disc lands on 'other' and must not fire (measured: 122 of 190 logged
        // discs sat on talk segments). Timed (3) keeps its legacy floor-only behaviour
        // (no disc fire); Off (0) never re-pins. Arm/cooldown bookkeeping is untouched
        // so a live mode switch stays coherent.
        const int rmode = repin_mode_.load();
        bool discFire = false;
        if (disc && hls_repin_armed_ && (rmode == 1 || rmode == 2)) {
            bool discSpot = false, discPaid = false;
            size_t dle = body.rfind("#EXTINF");
            if (dle != std::string::npos) {
                std::string dseg = body.substr(dle);
                char dsp = 0;
                size_t dp = dseg.find("song_spot=");
                if (dp != std::string::npos) {
                    size_t c = dp + 10;
                    while (c < dseg.size() && (dseg[c] == '\\' || dseg[c] == '"')) ++c;
                    if (c < dseg.size()) dsp = dseg[c];
                }
                bool dneg = dseg.find("spotInstanceId=\\\"-1\\\"") != std::string::npos ||
                            dseg.find("spotInstanceId=\"-1\"")     != std::string::npos;
                discSpot = (dsp == 'T');
                discPaid = (dsp == 'T' && !dneg);
            }
            const uint32_t dnow = port::tickMs();
            discFire = (rmode == 1) ? (discPaid || repinHardEvidence(dnow))
                                    : (discPaid || discSpot || repinHardEvidence(dnow));
        }
        if (discFire) {
            hls_repin_pending_.store(true);
            hls_repin_armed_ = false;
            hls_repin_cooldown_until_ = port::tickMs() + 90000;   // ~one ad pod
            slog("hlsPollMedia: AD-ONSET discontinuity -> requesting live-edge re-pin");
        } else if (!hls_repin_armed_ &&
                   (long)(port::tickMs() - hls_repin_cooldown_until_) >= 0) {
            hls_repin_armed_ = true;
            slog("hlsPollMedia: live-edge re-pin re-armed (cooldown elapsed)");
        }
    }

    uint64_t seq = media_seq;                   // sequence of the first segment listed
    int total = 0, new_count = 0;
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(i, e - i);
        while (!line.empty() && (line.back()=='\r'||line.back()==' '||line.back()=='\t')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        std::string uri = (s == std::string::npos) ? "" : line.substr(s);
        if (!uri.empty() && uri[0] != '#') {
            total++;
            if (!have_seq || seq > hls_.last_seq) {
                hls_.pending.push_back(hlsResolveUrl(final_url, uri));
                hls_.last_seq = seq;            // advance high-water mark
                new_count++;
            }
            seq++;
        }
        i = e + 1;
    }
    hls_.last_poll = port::tickMs();
    // Prime-to-music-boundary (clip fix): during a re-pin's connect poll only, record
    // where the first in-window ad->music boundary sits so hlsConnect can prime from
    // the song start. Gated on hls_priming_ so normal (non-connect) polls are untouched.
    if (hls_priming_)
        hls_prime_boundary_ = hlsFirstMusicBoundary(body);
    if (is_iheart_) updateIHeartNowPlaying(body);   // reconcile manifest + trackHistory
    // Diagnostic: live-edge offset. newest = last seq advertised in the manifest;
    // next_play ≈ the segment we're about to play (front of pending). edgeLag is
    // how many segments behind the live edge our audio is — watch this across an
    // ad break to see the drift, and confirm the re-pin collapses it.
    uint64_t newest_seq = have_seq ? (media_seq + (uint64_t)(total > 0 ? total - 1 : 0)) : 0;
    long long next_play = (long long)hls_.last_seq - (long long)hls_.pending.size() + 1;
    long long edge_lag  = (long long)newest_seq - next_play;

    // --- drift instrumentation (logging only; no behavior change) --------------
    // Segment numbers are GLOBAL and clock-locked: the same number is the same
    // broadcast instant across every session, so newest=<n> vs the line timestamp
    // IS broadcast-now. nextPlay is the segment at the front of our queue; edgeLag
    // is how many segments our queue sits behind the live edge. ringSec is decoded
    // audio buffered ahead of the speaker (so true playback ≈ nextPlay minus
    // ringSec). cls is the NEWEST segment's content: music; spot-ad (song_spot=T
    // with a real spotInstanceId = a paid commercial); spot-id (song_spot=T,
    // spotInstanceId=-1 = station id / promo / voice-track); or other. pdt flips
    // to 1 only if iHeart ever starts emitting EXT-X-PROGRAM-DATE-TIME (it doesn't
    // today) — that would unlock the web player's continuous live-pin. Across hours
    // these let us see exactly when/how we drift and which content precedes it.
    bool pdt = body.find("#EXT-X-PROGRAM-DATE-TIME") != std::string::npos;
    const char* cls = "none";
    {
        size_t le = body.rfind("#EXTINF");           // newest segment is the last EXTINF
        if (le != std::string::npos) {
            std::string seg = body.substr(le);
            char spot = 0;
            size_t sp = seg.find("song_spot=");
            if (sp != std::string::npos) {
                size_t c = sp + 10;
                while (c < seg.size() && (seg[c] == '\\' || seg[c] == '"')) ++c;
                if (c < seg.size()) spot = seg[c];
            }
            bool idNeg = seg.find("spotInstanceId=\\\"-1\\\"") != std::string::npos ||
                         seg.find("spotInstanceId=\"-1\"")     != std::string::npos;
            if      (spot == 'M' || spot == 'F') cls = "music";
            else if (spot == 'T')                cls = idNeg ? "spot-id" : "spot-ad";
            else                                 cls = "other";
        }
    }
    // Publish newest-segment class for the staging lane's coordinator to poll
    // (edgeIsMusic()): 1 music, 2 ad (spot-ad/spot-id), 3 other, 0 none.
    last_cls_.store(cls[0]=='m' ? 1 : cls[0]=='s' ? 2 : cls[0]=='o' ? 3 : 0,
                    std::memory_order_relaxed);
    double ring_sec = (double)ringAvailable() / (double)(SAMPLE_RATE * CHANNELS);

    slog("hlsPollMedia: target=%dms mseq=%llu total=%d new=%d disc=%d pending=%zu "
         "newest=%llu nextPlay=%lld edgeLag=%lld(~%.0fs) ringSec=%.1f cls=%s pdt=%d digital=%d armed=%d",
         hls_.target_dur_ms, (unsigned long long)media_seq, total, new_count, disc?1:0, hls_.pending.size(),
         (unsigned long long)newest_seq, next_play, edge_lag, edge_lag * (hls_.target_dur_ms / 1000.0),
         ring_sec, cls, pdt?1:0, digital_active_.load()?1:0, hls_repin_armed_?1:0);

    // Live-Edge mode (f6-live-edge, F6 mode 4): follow the live edge the way a browser
    // HLS engine does - DRIFT itself is the trigger, no ad-evidence, no floor timer,
    // no disc logic. When playback falls >= EDGE_LAG_MAX segments behind the edge,
    // re-anchor via the same pending/arm/cooldown path the other triggers use, so a
    // burst of drift = one re-anchor then the 90s cooldown (cannot storm). A healthy
    // stream rides at <=2 segments (measured), well under the threshold, so a normal
    // show never fires. This is the mode that fixes the ad-free countdown drift: the
    // web player stays on the current song because it never leaves the edge; mode 4
    // reproduces that (including playing through ads at the edge - always-current is
    // the point; the escape modes 1-3 keep their event-triggered meaning and their
    // drift-tolerant stability).
    if (have_seq && repin_mode_.load() == 4 && hls_repin_armed_ &&
        edge_lag >= EDGE_LAG_MAX) {
        hls_repin_pending_.store(true);
        hls_repin_armed_ = false;
        hls_repin_cooldown_until_ = port::tickMs() + 90000;
        slog("hlsPollMedia: live-edge drift %lld segs behind -> re-anchor to edge", edge_lag);
    }
    return true;
}

bool StreamSource::hlsFetchSegment(const std::string& url, std::vector<uint8_t>& out) {
    out.clear();
    if (!hlsHttpGet(url, nullptr, &out, nullptr)) { slog("hlsFetchSegment: FAILED url=%s", url.c_str()); return false; }
    slog("hlsFetchSegment: %zu bytes", out.size());
    return true;
}

// HLS branch of connect(): no persistent ICY connection — instead resolve the
// master, poll the first media playlist, and prime near the live edge so tune-in
// latency feels like a direct stream rather than a full window behind. Runs on
// the caller/connect-worker thread before the producer spawns (and again on the
// producer thread for reconnect, where re-resolving the master refreshes the
// session token). hls_ is owned by exactly one thread at a time, so no lock.
// Append the iHeart web-player param set to a raw zc####/hls.m3u8 base URL so the
// edge mints a token and serves the ad-reduced DIGITAL rendition. Uses the minimal
// anonymous param set (no profileId/skey — proven sufficient by StreamHandshakeProbe)
// with a fresh random listenerId per connect. Returns base unchanged if it isn't a
// bare iHeart URL we can parameterize.
std::string StreamSource::hlsBuildDigitalUrl(const std::string& base, const std::string& minted_profile) {
    if (base.find('?') != std::string::npos) return base;     // already parameterized
    std::string id;
    size_t p = base.find("/zc");
    if (p != std::string::npos) {
        p += 3;
        while (p < base.size() && std::isdigit((unsigned char)base[p])) id += base[p++];
    }
    if (id.empty()) return base;

    // Identity arm (probe, gated in hlsConnect). ANON (minted_profile empty, the default
    // and shipped behavior): fresh random listenerId, no profileId/skey. MINTED: a real
    // anonymous profileId (== skey), empty listenerId — mirroring the web player. Every
    // other param is identical between the two arms; the anon path stays byte-identical.
    std::string lid;
    if (minted_profile.empty()) {
        static bool seeded = false;
        if (!seeded) { std::srand(port::tickMs()); seeded = true; }
        static const char* H = "0123456789abcdef";
        lid.reserve(32);
        for (int i = 0; i < 32; ++i) lid += H[std::rand() & 15];
    }   // minted arm: lid stays empty (real profile, empty listenerId)

    std::string url = base + "?streamid=" + id +
        "&zip=&aw_0_1st.playerid=iHeartRadioWebPlayer&clientType=web&companionAds=false"
        "&deviceName=web-mobile&dist=iheart&host=webapp.US&listenerId=" + lid +
        "&playedFrom=157&pname=live_profile&stationid=" + id +
        "&terminalId=159&territory=US&us_privacy=1-N-";
    if (!minted_profile.empty())
        url += "&profileId=" + minted_profile + "&aw_0_1st.skey=" + minted_profile;
    return url;
}

bool StreamSource::hlsConnect() {
    if (!hlsEnsureSession()) { slog("hlsConnect: session FAILED"); return false; }
    std::string master = url_;          // the canonical zc####/hls.m3u8 the user pasted
    hls_ = HlsState{};
    // Stream mode: try the digital (web-player, ad-reduced) rendition first when the
    // user prefers it on an iHeart URL; otherwise the raw broadcast. The actual
    // fallback decision is made after resolve+poll below.
    ++connect_seq_;
    digital_active_.store(false);
    bool want_digital = prefer_digital_.load() && IHeartRadio::isIHeartUrl(master);
    // Identity A/B arm (probe): use a minted anonymous profileId ONLY when the arm is
    // selected AND the deep log is enabled (probe context) — otherwise force anon, so
    // the shipped binary and every non-probe session are unchanged. Mint is lazy and
    // fail-closed: on any mint failure we keep id_variant_="anon" and build the anon URL.
    id_variant_ = "anon";
    id_profile_tail_.clear();
    id_mint_ok_ = false;
    std::string minted_profile;   // empty -> hlsBuildDigitalUrl takes the anon branch
    if (want_digital && probe_minted_.load() && IHeartDeepLog::enabled()) {
        iheart_identity_ = IHeartIdentity::mintOrLoad(*http_,
            [](const std::string& s){ slog("%s", s.c_str()); });
        if (iheart_identity_.ok) {
            id_variant_      = "minted";
            id_profile_tail_ = iheart_identity_.profileTail();
            id_mint_ok_      = true;
            minted_profile   = iheart_identity_.profileId;
            slog("hlsConnect: minted identity arm (profile ...%s)", id_profile_tail_.c_str());
        } else {
            slog("hlsConnect: mint FAILED -> anon handshake fallback");   // fail closed
        }
    }
    hls_.master_url = want_digital ? hlsBuildDigitalUrl(master, minted_profile) : master;
    // iHeart now-playing: primary source is the variant manifest's EXTINF tags
    // (freeze-proof), trackHistory module as fallback. Set this up BEFORE the initial
    // poll so hlsPollMedia parses the manifest on connect — otherwise the producer's
    // first trackHistory fallback can clobber the (not-yet-set) manifest result on a
    // station switch. resolve() is url-aware, so switching re-resolves cleanly.
    is_iheart_          = IHeartRadio::isIHeartUrl(url_);
    last_iheart_poll_   = 0;
    iheart_th_cache_.clear();
    iheart_th_ended_    = -1;
    ih_sm_.reset();
    if (is_iheart_) {
        iheart_.setLogger([](const std::string& s){ slog("%s", s.c_str()); });
        iheart_.resolve(url_);   // sidecar-first; cheap; gives us stationName() for the ad label
    }
    // Resolve + initial poll. If a requested DIGITAL attempt fails at any step, fall
    // back to the raw broadcast URL once, so a changed/blocked handshake degrades to
    // exactly today's behavior (never worse).
    hls_priming_        = true;    // arm the prime-to-boundary scan for the connect poll(s)
    hls_prime_boundary_ = -1;
    bool resolved_ok = hlsResolveMaster() && hlsPollMedia();
    if (want_digital && resolved_ok) {
        digital_active_.store(true);
        slog("hlsConnect: DIGITAL rendition active (seq=%d)", connect_seq_.load());
    } else if (want_digital && !resolved_ok) {
        slog("hlsConnect: digital attempt FAILED -> falling back to raw broadcast");
        hls_.master_url = master;
        hls_.variant_url.clear();
        hls_.pending.clear();
        hls_.last_seq = 0;
        if (!hlsResolveMaster() || !hlsPollMedia()) {
            slog("hlsConnect: raw fallback also FAILED");
            return false;
        }
        slog("hlsConnect: raw fallback OK (seq=%d)", connect_seq_.load());
    } else if (!resolved_ok) {
        slog("hlsConnect: resolve/poll FAILED");
        return false;
    } else {
        slog("hlsConnect: raw rendition active (seq=%d)", connect_seq_.load());
    }
    // Prime near the live edge. On a re-pin (Section A clip fix), if the fresh manifest
    // showed a clean ad->music boundary in-window, prime from the song's first segment
    // so audio lands at the song start instead of ~2 segments behind live. Otherwise
    // (first tune-in, no clean boundary visible, or scan ambiguous) keep the last ~2
    // queued segments -- the unchanged, working live-edge prime. Fail toward current
    // behaviour whenever the boundary is not unambiguous (hls_prime_boundary_ <= 0).
    bool primed_boundary = false;
    if (hls_repin_active_ && digital_active_.load() &&
        hls_prime_boundary_ > 0 &&
        (size_t)hls_prime_boundary_ < hls_.pending.size()) {
        hls_.pending.erase(hls_.pending.begin(),
                           hls_.pending.begin() + hls_prime_boundary_);
        primed_boundary = true;
        slog("hlsConnect: re-pin primed to music boundary (idx=%d) pending=%zu",
             hls_prime_boundary_, hls_.pending.size());
    }
    if (!primed_boundary && hls_.pending.size() > 2)   // unchanged live-edge fallback
        hls_.pending.erase(hls_.pending.begin(), hls_.pending.end() - 2);
    hls_priming_ = false;
    icy_metaint_ = 0;                   // HLS metadata isn't ICY — readAudio passes straight through
    icy_counter_ = 0;
    raw_buf_.clear(); raw_pos_ = 0;
    if (is_iheart_)
        slog("hlsConnect: iHeart station — manifest-primary now-playing (station=\"%s\")",
             iheart_.stationName().c_str());
    slog("hlsConnect: primed pending=%zu variant=%s",
         hls_.pending.size(), hls_.variant_url.c_str());
    return true;
}

// ── iHeart manifest-primary now-playing ──────────────────────────────────────
// iHeart embeds the live song (and ad markers) directly in the variant playlist's
// per-segment EXTINF tags, e.g.:
//   #EXTINF:10,title="Listen To Your Heart",artist="D.H.T.",url="song_spot=\"M\" ..."
// Unlike trackHistory (which freezes for minutes during long breaks), the manifest
// rides the audio — it can't go stale while music plays. We read the live-edge
// segment's tag, classify song vs ad, and drive now_playing_ from it.
//   song: real artist + title, not flagged spot -> "Artist - Title"
//   ad:   song_spot="T" / blank artist / Spot Block -> "<station> - Commercial break"
//         (the scrobbler's looksLikeRealTrack already drops "commercial break")
// Discriminator verified against zc4366 + zc1469 song and ad captures.

// Read a quoted EXTINF attribute, honoring iHeart's backslash-escaped inner quotes.
static std::string mfAttr(const std::string& line, const char* key) {
    size_t k = line.find(key);
    if (k == std::string::npos) return {};
    size_t start = k + std::strlen(key);
    size_t i = start;
    while (i < line.size()) {
        if (line[i] == '"' && line[i-1] != '\\') break;   // unescaped closing quote
        ++i;
    }
    std::string v = line.substr(start, i - start), out;
    for (size_t j = 0; j < v.size(); ++j) {               // unescape \" -> "
        if (v[j] == '\\' && j + 1 < v.size() && v[j+1] == '"') { out += '"'; ++j; }
        else out += v[j];
    }
    return out;
}
static std::string mfTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// Classify the manifest's live-edge segment. iHeart embeds per-segment metadata in
// the EXTINF tag. Three outcomes, because a blank/boundary marker is NOT an ad:
//   Song : real artist + song_spot="M"/"F"            -> "Artist - Title"
//   Ad   : song_spot="T", or an active "Spot Block"   -> a commercial is playing now
//   None : "Spot Block End" / length="00:00:00" / blank with no ad flag
//          -> the manifest can't tell us; defer to trackHistory
// Verified: The Breeze songs are song_spot="M"; Breeze ads song_spot="T"; Z100 songs
// leave the manifest stuck on "Spot Block End" (None) while trackHistory has the song;
// Z100 active ads show "Spot Block" with a real length (Ad).
// (enum IHeartMfCls now lives in IHeartNowPlayingSM.h, included via StreamSource.h.)

static IHeartMfCls classifyIHeartManifest(const std::string& body,
                                          std::string& artistOut, std::string& titleOut) {
    std::string last;                                     // live-edge EXTINF with a title= attr
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        if (body.compare(i, 7, "#EXTINF") == 0) {
            std::string line = body.substr(i, e - i);
            if (line.find("title=\"") != std::string::npos) last = line;
        }
        i = e + 1;
    }
    if (last.empty()) return IHeartMfCls::None;

    std::string title  = mfTrim(mfAttr(last, "title=\""));
    std::string artist = mfTrim(mfAttr(last, "artist=\""));

    char spot = 0;                                        // song_spot letter (M/F=song, T=ad)
    size_t sp = last.find("song_spot=");
    if (sp != std::string::npos) {
        size_t c = sp + 10;
        while (c < last.size() && (last[c] == '\\' || last[c] == '"')) ++c;
        if (c < last.size()) spot = last[c];
    }

    bool spotBlock    = title.find("Spot Block") != std::string::npos;
    bool spotBlockEnd = title.find("Spot Block End") != std::string::npos;
    bool zeroLen      = last.find("length=\\\"00:00:00\\\"") != std::string::npos ||
                        last.find("length=\"00:00:00\"")     != std::string::npos;

    // Real song: a genuine artist, not flagged as a spot.
    if (!artist.empty() && spot != 'T' && !spotBlock) {
        artistOut = artist; titleOut = title;
        return IHeartMfCls::Song;
    }
    // Boundary / zero-length / "Spot Block End": the manifest is stuck or blank here.
    if (spotBlockEnd || zeroLen) return IHeartMfCls::None;
    // Confident active ad: explicit traffic flag, or a Spot Block that's actually running.
    if (spot == 'T' || spotBlock)  return IHeartMfCls::Ad;
    return IHeartMfCls::None;                             // blank/ambiguous -> defer
}

// Reconcile the two iHeart sources into a debounced now-playing state. Both the
// manifest and trackHistory lie in different ways (The Breeze: manifest reliable,
// trackHistory freezes; Z100: manifest stuck on "Spot Block End", trackHistory
// reliable; and The Breeze's manifest can even show a PHANTOM ad over a real song).
// So we trust confident songs instantly, make ads earn corroboration + persistence,
// and floor everything uncertain to an honest "<station> - LIVE".
//   Song : manifest song_spot="M"+artist, OR trackHistory song currently in-window
//          -> committed immediately (1 tick); songs are never phantoms.
//   Ad   : manifest says ad AND trackHistory corroborates (recently active, no
//          current song) -> committed only after holding 3 ticks.
//   Live : anything else — boundary, disagreement, or a manifest "ad" while
//          trackHistory is frozen (can't confirm) -> "<station> - LIVE" after 2 ticks.
// ── Ad-evidence tracker (f6-repin-finalize) ──────────────────────────────────
// Producer thread only. Ring-pruned to the evidence window; the (int32_t) diffs
// are the wrap-safe tick comparison (LP64: never (long)(u32-u32)).
void StreamSource::noteRepinEvidence(uint32_t now, bool isSpotSeg, bool paid, const std::string& cartcut) {
    repin_cls_hist_.emplace_back(now, isSpotSeg);
    while (!repin_cls_hist_.empty() &&
           (int32_t)(now - repin_cls_hist_.front().first) > (int32_t)kRepinEvidenceWindowMs)
        repin_cls_hist_.pop_front();
    if (!cartcut.empty()) {
        repin_cart_hist_.emplace_back(now, cartcut);
        while (!repin_cart_hist_.empty() &&
               (int32_t)(now - repin_cart_hist_.front().first) > (int32_t)kRepinEvidenceWindowMs)
            repin_cart_hist_.pop_front();
    }
    if (paid) repin_paid_tick_ = now;
}
bool StreamSource::repinHardEvidence(uint32_t now) const {
    if (repin_paid_tick_ && (int32_t)(now - repin_paid_tick_) <= (int32_t)kRepinEvidenceWindowMs)
        return true;
    // >=2 DISTINCT cartcutIds in the window = a real pod churning spots. Tiny deque,
    // quadratic scan is fine (and avoids a <set> include).
    for (size_t i = 0; i < repin_cart_hist_.size(); ++i)
        for (size_t j = i + 1; j < repin_cart_hist_.size(); ++j)
            if (repin_cart_hist_[i].second != repin_cart_hist_[j].second)
                return true;
    return false;
}
int StreamSource::repinSpotSegsInWindow(uint32_t now) const {
    int n = 0;
    for (const auto& p : repin_cls_hist_)
        if (p.second && (int32_t)(now - p.first) <= (int32_t)kRepinEvidenceWindowMs) ++n;
    return n;
}

void StreamSource::updateIHeartNowPlaying(const std::string& body) {
    std::string mfArtist, mfTitle;
    IHeartMfCls cls   = classifyIHeartManifest(body, mfArtist, mfTitle);
    std::string mfSong = (cls == IHeartMfCls::Song) ? (mfArtist + " - " + mfTitle) : std::string();

    // trackHistory snapshot (throttled ~9s; caches song + staleness between polls).
    uint32_t nowtick = port::tickMs();
    if (last_iheart_poll_ == 0 || nowtick - last_iheart_poll_ >= 9000) {
        last_iheart_poll_ = nowtick;
        long ended = -1;
        IHeartRadio::PollDiag thDiag;
        iheart_th_cache_ = iheart_.resolve(url_) ? iheart_.pollNowPlaying(&ended, &thDiag) : std::string();
        iheart_th_ended_ = ended;
        iheart_th_diag_  = thDiag;   // cache beside th/thEnded so the Record reads a consistent snapshot every tick
        // currentTrackMeta: the web player's own now-playing source. Polled while the
        // deep log is on (probe), OR in digital mode -- there ctm rides the same timeline
        // as the audio and carries the album-art URL we surface to Discord.
        if (IHeartDeepLog::enabled() || digital_active_.load())
            iheart_.pollCurrentTrackMeta(&iheart_ctm_);
    }
    // Album art for Discord RP: in DIGITAL mode, currentTrackMeta rides the same
    // timeline as the audio. Publish only when a SONG is the committed state (read from
    // the SM = the previous tick's commit) AND ctm agrees (title match).
    {
        std::string art;
        if (digital_active_.load() && ih_sm_.state() == IHNow::Song &&
            iheart_ctm_.ok && !iheart_ctm_.imagePath.empty()) {
            auto squash = [](const std::string& s){
                std::string o; for (unsigned char c : s)
                    if (std::isalnum(c)) o += (char)std::tolower(c);
                return o;
            };
            std::string t = squash(iheart_ctm_.title);
            if (!t.empty() && squash(ih_sm_.stateDisp()).find(t) != std::string::npos) {
                art = iheart_ctm_.imagePath;
                if (art.rfind("http://", 0) == 0) art = "https://" + art.substr(7); // Discord media proxy prefers https
            }
        }
        std::lock_guard<std::mutex> lk(now_playing_mtx_);
        iheart_art_ = art;
    }

    // Dual-stream staging lane (coordinator only). Reads the committed state from the
    // PREVIOUS tick (ih_sm_.state()) -- kept ahead of ih_sm_.tick() so it still observes
    // pre-commit state, exactly as before (it used to run before the debounce commit).
    if (!is_lane_) serviceStaging();

    // ── Pure reconciliation tick (target ladder + debounce + LIVE-floor stall). ──
    IHeartTick tk;
    tk.nowMs           = nowtick;
    tk.mfCls           = cls;
    tk.mfSong          = mfSong;
    tk.thSong          = iheart_th_cache_;
    tk.thEnded         = iheart_th_ended_;
    tk.digitalActive   = digital_active_.load();
    tk.ctmOk           = iheart_ctm_.ok;
    tk.ctmStatus       = iheart_ctm_.httpStatus;
    tk.ctmArtist       = iheart_ctm_.artist;
    tk.ctmTitle        = iheart_ctm_.title;
    tk.ctmEndedSecsAgo = iheart_ctm_.endedSecsAgo;
    tk.stationName     = iheart_.stationName();
    tk.repinArmed      = hls_repin_armed_;
    tk.repinMode       = repin_mode_.load();   // F6: 0 off / 1 ad-escape / 2 hybrid / 3 timed
    IHeartDecision d   = ih_sm_.tick(tk);

    // ── Deep-analysis capture (opt-in, Ctrl+A; no-op unless enabled). Records the
    //    full tick's pre-commit state (from the decision snapshot), so a metadata
    //    freeze still emits ticks. ──
    {
        IHeartDeepLog::Record dr;
        dr.stationId = iheart_.stationId();
        dr.station   = tk.stationName;
        dr.audioSec  = (double)frames_drained_.load(std::memory_order_relaxed) / (double)SAMPLE_RATE;
        dr.posSec    = positionSec();
        dr.mfCls     = (cls == IHeartMfCls::Song) ? "Song" : (cls == IHeartMfCls::Ad ? "Ad" : "None");
        dr.mfArtist  = mfArtist;
        dr.mfTitle   = mfTitle;
        dr.mfSong    = mfSong;
        dr.mfSeq     = IHeartDeepLog::extractMediaSeq(body);
        dr.mfBodyLen = body.size();
        dr.pdt       = body.find("#EXT-X-PROGRAM-DATE-TIME") != std::string::npos;
        {   // newest segment paid-ad? (song_spot=T with a real, non -1 spotInstanceId)
            size_t le = body.rfind("#EXTINF");
            if (le != std::string::npos) {
                std::string seg = body.substr(le);
                char spot = 0; size_t sp = seg.find("song_spot=");
                if (sp != std::string::npos) {
                    size_t c = sp + 10;
                    while (c < seg.size() && (seg[c] == '\\' || seg[c] == '"')) ++c;
                    if (c < seg.size()) spot = seg[c];
                }
                bool idNeg = seg.find("spotInstanceId=\\\"-1\\\"") != std::string::npos ||
                             seg.find("spotInstanceId=\"-1\"")     != std::string::npos;
                dr.spotPaid = (spot == 'T' && !idNeg);
                // Ad identity: pull the quoted value of an EXTINF attribute, honoring
                // iHeart's backslash-escaped inner quotes (key=\"value\").
                auto attr = [&seg](const char* key) -> std::string {
                    size_t k = seg.find(key);
                    if (k == std::string::npos) return {};
                    k += std::strlen(key);
                    // KEY=\"value\" (escaped) or KEY="value". Consume EXACTLY the opening
                    // =, optional backslash, opening quote -- not the whole quote run, or an
                    // empty value (KEY=\"\") would swallow the next field's name.
                    if (k < seg.size() && seg[k] == '=')  ++k;
                    if (k < seg.size() && seg[k] == '\\') ++k;
                    if (k < seg.size() && seg[k] == '"')  ++k;
                    size_t e = k;
                    while (e < seg.size() && seg[e] != '\\' && seg[e] != '"') ++e;
                    return seg.substr(k, e - k);
                };
                dr.spotInstanceId = attr("spotInstanceId");
                dr.cartcutId      = attr("cartcutId");
                // f6-repin-finalize: feed the ad-evidence tracker from this same parse.
                // Runs every manifest tick regardless of the deep log being enabled -
                // the re-pin gates below depend on it.
                noteRepinEvidence(port::tickMs(), spot == 'T', dr.spotPaid, dr.cartcutId);
            }
        }
        dr.th        = iheart_th_cache_;
        dr.thEnded   = iheart_th_ended_;
        dr.thCurrent = d.thCurrent;
        dr.thAccMaxStart = iheart_th_diag_.acceptedMaxStart;
        dr.thNewestStart = iheart_th_diag_.newestAiredStart;
        dr.thEntryCount  = iheart_th_diag_.entryCount;
        dr.thChosenIdx   = iheart_th_diag_.chosenIdx;
        dr.thFutureSkip  = iheart_th_diag_.futureSkipped;
        dr.thHeldBy      = iheart_th_diag_.heldBy;
        dr.repinArmed    = hls_repin_armed_;
        dr.tgtKind   = ihNowName(d.tgtKind);
        dr.tgtDisp   = d.tgtDisp;
        dr.stState   = ihNowName(d.stKind);
        dr.stDisp    = d.stDisp;
        dr.pendKind  = ihNowName(d.pendKind);
        dr.pendDisp  = d.pendDisp;
        dr.streak    = d.streak;
        dr.ctmOk           = iheart_ctm_.ok;
        dr.ctmStatus       = iheart_ctm_.httpStatus;
        dr.ctmArtist       = iheart_ctm_.artist;
        dr.ctmTitle        = iheart_ctm_.title;
        dr.ctmAlbum        = iheart_ctm_.album;
        dr.ctmTrackId      = iheart_ctm_.trackId;
        dr.ctmStartSec     = iheart_ctm_.startSec;
        dr.ctmEndSec       = iheart_ctm_.endSec;
        dr.ctmDurationSec  = iheart_ctm_.durationSec;
        dr.ctmEndedSecsAgo = iheart_ctm_.endedSecsAgo;
        dr.ctmImage        = iheart_ctm_.imagePath;
        dr.ctmDataSource   = iheart_ctm_.dataSource;
        dr.streamMode      = digital_active_.load() ? "digital" : "raw";
        dr.digitalRequested = prefer_digital_.load();
        dr.digitalActive   = digital_active_.load();
        dr.connectSeq      = connect_seq_.load();
        // identity A/B probe: which digital-handshake identity this connect used, so the
        // ad-fill outcome (spotPaid/cartcutId/thEnded/streamMode) and the identity input
        // are self-correlated in one NDJSON stream. idProfileTail is TAIL-ONLY by design.
        dr.idVariant       = id_variant_;
        dr.idProfileTail   = id_profile_tail_;
        dr.idMintOk        = id_mint_ok_;
        if (!is_lane_) IHeartDeepLog::emit(dr);
    }

    // Live-floor stall -> live-edge re-pin. The SM owns the floor timer and reports the
    // expiry; the CALLER gates the actual fire on ad-evidence per the F6 mode
    // (f6-repin-finalize). Duration alone cannot tell a 30-min talk show from a stuck ad
    // pod - the old duration-only escape fired every ~170s through whole shows (87
    // spurious re-handshakes measured on one day's air), while real pods show spot/ad
    // segments in the timed window. Ad-Escape (1) = hard evidence only; Hybrid (2) =
    // hard evidence OR >=2 spot/ad segments in the window; Timed (3) = legacy
    // duration-only (documented: will thrash talk); Off (0) never fires (SM-side);
    // Live-Edge (4) never fires HERE - its trigger is drift, in hlsPollMedia.
    // The caller still owns the shared armed/cooldown/pending atomics (also driven by
    // the discontinuity path in hlsPollMedia), so both triggers share one owner.
    if (d.liveStallFired) {
        const int      rm    = repin_mode_.load();
        const uint32_t nowms = port::tickMs();
        const bool     hard  = repinHardEvidence(nowms);
        const int      spots = repinSpotSegsInWindow(nowms);
        const bool     fire  = (rm == 3) || (rm == 1 && hard) || (rm == 2 && (hard || spots >= 2));
        if (fire) {
            hls_repin_pending_.store(true);
            hls_repin_armed_ = false;
            hls_repin_cooldown_until_ = port::tickMs() + 90000;
            slog("updateIHeart: LIVE-floor stall %lus -> requesting live-edge re-pin (mode=%d hard=%d spots=%d)",
                 (unsigned long)(d.liveStallElapsedMs / 1000), rm, hard ? 1 : 0, spots);
        } else {
            slog("updateIHeart: LIVE-floor stall %lus suppressed - no ad evidence (mode=%d hard=%d spots=%d)",
                 (unsigned long)(d.liveStallElapsedMs / 1000), rm, hard ? 1 : 0, spots);
        }
    }

    // Commit publish (caller owns now_playing_ / np_pub_q_ / the mutex).
    if (d.committed) {
        std::string disp = sanitizeForDisplay(d.newDisp);
        std::lock_guard<std::mutex> lk(now_playing_mtx_);
        if (disp != now_playing_) {
            now_playing_ = disp;
            // Hold this label until its audio is heard: delay = buffer depth ahead of
            // the speaker = ring seconds + undecoded pending segments. Self-calibrating,
            // so it tracks rebuffers and collapses to ~0 right after a re-pin flush.
            long ring_ms = (long)(1000.0 * (double)ringAvailable() / (double)(SAMPLE_RATE * CHANNELS));
            long pend_ms = (long)hls_.pending.size() * (long)hls_.target_dur_ms;
            np_pub_q_.push_back({ port::tickMs() + (uint32_t)(ring_ms + pend_ms), disp });
            const char* via = (d.newKind==IHNow::Song) ? "song" : (d.newKind==IHNow::Ad ? "ad" : "live");
            slog("iheart [%s]: %s", via, disp.c_str());
        }
    }
}

// ─── Staging lane (dual-stream smooth re-pin) — Stage A: shadow observer ───────
// Coordinator-only (guarded by !is_lane_ at the call site). Spawns a parallel
// StreamSource on the SAME canonical URL with the digital rendition, prebuffers
// it on a detached thread (never blocking the primary producer), and waits for
// that lane's OWN live edge to clear to music. Stage A stops there: it logs that
// a clean blend point was reached and tears the lane down. The primary's audio
// and existing re-pin are untouched. Stage B replaces the teardown with a swap
// into the lane; Stage C with an equal-power crossfade.
//
// Teardown always happens on a detached thread: StreamSource::close() joins the
// lane's producer, which can take up to a read chunk — doing that inline would
// stall the primary producer (this runs on it) and underrun audio.
void StreamSource::serviceStaging() {
    uint32_t now = port::tickMs();

    auto teardown = [&](const char* why, uint32_t cooldown_ms) {
        slog("staging: %s -> teardown", why);
        if (staging_) {
            StreamSource* s = staging_.release();      // hand ownership to the reaper thread
            std::thread([s]{ s->close(); delete s; }).detach();
        }
        stg_cooldown_until_ = now + cooldown_ms;
        stg_state_ = Stg::Idle;
    };

    switch (stg_state_) {
    case Stg::Idle:
        // Arm while a commercial is airing on the primary (LIVE floor in digital
        // mode) and the cooldown has elapsed. One staging session per break.
        if (digital_active_.load() && ih_sm_.state() == IHNow::Live &&
            (long)(now - stg_cooldown_until_) >= 0 &&
            !stg_opening_.load() && !staging_) {
            staging_ = std::make_unique<StreamSource>(true, *http_);  // is_lane_ = true; same injected transport
            staging_->setPreferDigital(true);
            stg_opening_.store(true);
            stg_state_ = Stg::Opening;
            stg_armed_tick_ = now;                              // for elapsed-since-arm peek logs
            std::string u = url_;
            slog("staging: arming parallel digital session");
            std::thread([this, u]{
                bool ok = staging_ && staging_->open(u);       // blocking handshake off the primary thread
                if (!ok) slog("staging: open FAILED");
                stg_opening_.store(false);
            }).detach();
        }
        break;

    case Stg::Opening:
        if (!stg_opening_.load()) {                             // detached open() finished
            if (staging_ && staging_->isOpen()) {
                stg_state_ = Stg::Arming;
                slog("staging: open OK, prebuffering parallel session");
            } else {
                teardown("open did not establish", 30000);
            }
        }
        break;

    case Stg::Arming:
        if (ih_sm_.state() != IHNow::Live) {                   // primary's break cleared on its own
            teardown("primary break cleared (no blend needed)", 15000);
        } else {
            // Step-1 visibility (logging only, no behavior change): record what the
            // fresh-handshake peek sees at its OWN live edge each poll, with seconds
            // since the peek armed. The primary's edge stays on slate during a
            // break, so this is the earliest place the returning song is visible.
            // Compare "peek cls=music +Ns" against the 40s LIVE-floor timer to size
            // how much sooner a peek-driven re-pin could land us in the song.
            unsigned el = (stg_armed_tick_ ? (now - stg_armed_tick_) / 1000 : 0);
            if (staging_)
                slog("staging: peek cls=%s prebuf=%d +%us",
                     staging_->edgeClsName(), staging_->isPrebuffered() ? 1 : 0, el);
            if (staging_ && staging_->isPrebuffered() && staging_->edgeIsMusic()) {
                slog("staging: READY at music edge +%us — clean blend point reached "
                     "(Stage A observer; B/C will swap/crossfade here)", el);
                teardown("Stage A: observed only", 60000);
            }
        }
        break;
    }
}

// HLS branch of rawRead(): serves bytes from the current segment, advancing to
// the next as it drains and re-polling the media playlist for new segments at the
// target-duration cadence. Recovers a stale token internally by re-resolving the
// master; returns 0 only on stop or genuine death (the producer's reconnect path
// is the backstop). Runs on the producer thread only.
// Decode an ID3v2 text frame body (leading encoding byte + text) to UTF-8.
// Handles ISO-8859-1, UTF-8, and UTF-16 (with/without BOM). Best-effort; the
// result is sanitized to ASCII by the caller per the display invariant.
static std::string decodeId3Text(const uint8_t* d, size_t n) {
    if (n == 0) return {};
    uint8_t enc = d[0];
    const uint8_t* s = d + 1;
    size_t m = (n > 0) ? n - 1 : 0;
    std::string out;
    auto put_cp = [&out](uint32_t u) {
        if (u < 0x80) out += (char)u;
        else if (u < 0x800) { out += (char)(0xC0|(u>>6)); out += (char)(0x80|(u&0x3F)); }
        else { out += (char)(0xE0|(u>>12)); out += (char)(0x80|((u>>6)&0x3F)); out += (char)(0x80|(u&0x3F)); }
    };
    switch (enc) {
        case 0:   // ISO-8859-1 -> UTF-8
            for (size_t i=0;i<m;i++){ uint8_t c=s[i]; if(c==0) break; put_cp(c); }
            break;
        case 3:   // UTF-8 (copy through, stop at NUL)
            for (size_t i=0;i<m;i++){ if(s[i]==0) break; out += (char)s[i]; }
            break;
        case 1: case 2: {   // UTF-16 (BOM or BE) -> UTF-8
            size_t i = 0; bool le = false;
            if (m >= 2 && ((s[0]==0xFF&&s[1]==0xFE)||(s[0]==0xFE&&s[1]==0xFF))) { le=(s[0]==0xFF); i=2; }
            for (; i+1 < m; i+=2){
                uint16_t u = le ? (uint16_t)(s[i] | (s[i+1]<<8)) : (uint16_t)((s[i]<<8) | s[i+1]);
                if (u==0) break;
                put_cp(u);
            }
            break;
        }
        default:
            for (size_t i=0;i<m;i++){ if(s[i]==0) break; out += (char)s[i]; }
            break;
    }
    return out;
}

// Parse a leading ID3v2 tag for TIT2 (title) + TPE1 (artist) and publish them to
// the shared now-playing slot (same slot the ICY path fills), so HLS song info
// flows to the display and scrobbler with no extra plumbing. Defensive: malformed
// frames bail without disturbing the read path. Logs to [stream] for diagnosis.
void StreamSource::hlsParseId3(const uint8_t* tag, size_t len) {
    if (len < 10 || tag[0]!='I' || tag[1]!='D' || tag[2]!='3') return;
    int     ver   = tag[3];                 // major version: 3 (v2.3) or 4 (v2.4)
    uint8_t flags = tag[5];
    uint32_t tagsize = ((uint32_t)(tag[6]&0x7f)<<21) | ((uint32_t)(tag[7]&0x7f)<<14) |
                       ((uint32_t)(tag[8]&0x7f)<< 7) | ((uint32_t)(tag[9]&0x7f));
    if (flags & 0x80)                       // unsynchronisation — rare for timed-ID3
        slog("hls-id3: unsync flag set (parse may be partial)");
    size_t pos = 10;
    size_t end = len < (size_t)10 + tagsize ? len : (size_t)10 + tagsize;
    if (flags & 0x40) {                     // extended header present — skip it
        if (pos + 4 > end) return;
        uint32_t extlen = (ver >= 4)
            ? (((uint32_t)(tag[pos]&0x7f)<<21)|((uint32_t)(tag[pos+1]&0x7f)<<14)|((uint32_t)(tag[pos+2]&0x7f)<<7)|((uint32_t)(tag[pos+3]&0x7f)))
            : (((uint32_t)tag[pos]<<24)|((uint32_t)tag[pos+1]<<16)|((uint32_t)tag[pos+2]<<8)|((uint32_t)tag[pos+3]));
        pos += (ver >= 4) ? extlen : (4 + extlen);   // v2.4 extlen counts itself; v2.3 doesn't
    }
    std::string title, artist;
    while (pos + 10 <= end) {
        char id[5] = { (char)tag[pos], (char)tag[pos+1], (char)tag[pos+2], (char)tag[pos+3], 0 };
        if (id[0] == 0) break;              // hit padding
        uint32_t fsz = (ver >= 4)
            ? (((uint32_t)(tag[pos+4]&0x7f)<<21)|((uint32_t)(tag[pos+5]&0x7f)<<14)|((uint32_t)(tag[pos+6]&0x7f)<<7)|((uint32_t)(tag[pos+7]&0x7f)))
            : (((uint32_t)tag[pos+4]<<24)|((uint32_t)tag[pos+5]<<16)|((uint32_t)tag[pos+6]<<8)|((uint32_t)tag[pos+7]));
        size_t fdata = pos + 10;
        if (fsz == 0 || fdata + fsz > end) break;     // padding or malformed -> stop
        if (!std::strcmp(id,"TIT2"))      title  = decodeId3Text(tag + fdata, fsz);
        else if (!std::strcmp(id,"TPE1")) artist = decodeId3Text(tag + fdata, fsz);
        pos = fdata + fsz;
    }
    if (title.empty() && artist.empty()) {
        slog("hls-id3: tag found (ver=2.%d size=%u) but no TIT2/TPE1", ver, tagsize);
        return;
    }
    std::string combined = artist.empty() ? title
                         : (title.empty() ? artist : artist + " - " + title);
    combined = sanitizeForDisplay(combined);
    {
        std::lock_guard<std::mutex> lk(now_playing_mtx_);
        if (combined == now_playing_) return;         // unchanged
        now_playing_ = combined;
    }
    slog("hls-id3: %s", combined.c_str());
}

uint32_t StreamSource::hlsRawRead(void* dst, uint32_t want) {
    // 1. Serve from the current segment if bytes remain.
    if (hls_.seg_pos < hls_.seg.size()) {
        uint32_t avail = (uint32_t)(hls_.seg.size() - hls_.seg_pos);
        uint32_t n = (want < avail) ? want : avail;
        std::memcpy(dst, hls_.seg.data() + hls_.seg_pos, n);
        hls_.seg_pos += n;
        return n;
    }
    // 2. Current segment drained — make sure we have a pending segment to fetch.
    int repoll_fail = 0;
    while (hls_.pending.empty()) {
        if (stop_.load()) return 0;
        uint32_t since = port::tickMs() - hls_.last_poll;
        if (since < (uint32_t)hls_.target_dur_ms) { port::sleepMs(50); continue; }   // pace polling
        if (!hlsPollMedia()) {
            // Variant GET failed — session token likely expired. Refresh via master.
            if (!hlsResolveMaster() || !hlsPollMedia()) {
                if (++repoll_fail >= 3) { slog("hlsRawRead: giving up after repoll failures"); return 0; }
                if (stop_.load()) return 0;
                port::sleepMs(300);
            }
        }
        // If still empty, the live edge hasn't advanced yet — loop and wait.
    }
    // 3. Fetch the next pending segment.
    std::string next = hls_.pending.front();
    hls_.pending.erase(hls_.pending.begin());
    hls_.seg.clear(); hls_.seg_pos = 0;
    if (!hlsFetchSegment(next, hls_.seg)) return 0;   // producer reconnect is the backstop

    // 4. Leading ID3v2 tag: parse it for timed now-playing metadata (TIT2/TPE1),
    //    then skip past it so the byte stream the decoder sees starts on an ADTS
    //    frame. iHeart .aac is usually bare ADTS, but HLS carries song info as a
    //    timed-ID3 tag in front of segments when the track changes.
    if (hls_.seg.size() >= 10 && hls_.seg[0]=='I' && hls_.seg[1]=='D' && hls_.seg[2]=='3') {
        uint32_t sz = ((uint32_t)(hls_.seg[6] & 0x7f) << 21) |
                      ((uint32_t)(hls_.seg[7] & 0x7f) << 14) |
                      ((uint32_t)(hls_.seg[8] & 0x7f) <<  7) |
                      ((uint32_t)(hls_.seg[9] & 0x7f));
        size_t skip = 10 + (size_t)sz;
        size_t avail = (skip <= hls_.seg.size()) ? skip : hls_.seg.size();
        hlsParseId3(hls_.seg.data(), avail);     // extract now-playing before skipping
        if (skip < hls_.seg.size()) hls_.seg_pos = skip;
    }

    // 5. Recurse once to serve the freshly fetched segment.
    return hlsRawRead(dst, want);
}

bool StreamSource::hlsProbeTest(const std::string& master_url) {
    hls_ = HlsState{};
    hls_.master_url = master_url;
    slog("hlsProbe: start url=%s", master_url.c_str());
    if (!hlsEnsureSession())                 { return false; }
    if (!hlsResolveMaster())                 { disconnect(); return false; }
    if (!hlsPollMedia())                     { disconnect(); return false; }
    if (hls_.pending.empty())                { slog("hlsProbe: no segments queued"); disconnect(); return false; }

    std::vector<uint8_t> seg;
    if (!hlsFetchSegment(hls_.pending.front(), seg)) { disconnect(); return false; }
    bool adts = seg.size() >= 2 && seg[0] == 0xFF && (seg[1] & 0xF6) == 0xF0;  // ADTS sync + layer==00
    slog("hlsProbe: OK first-seg=%zu bytes adts_sync=%d window=%zu",
         seg.size(), adts ? 1 : 0, hls_.pending.size());
    disconnect();
    return true;
}

// ─── ICY metadata de-interleaving ─────────────────────────────────────────────
// Both decoders pull audio through readAudio(); it transparently removes the
// periodic Shoutcast/Icecast metadata blocks and parses the StreamTitle out.

uint32_t StreamSource::rawRead(void* dst, uint32_t want) {
    if (mode_ == Mode::HLS) return hlsRawRead(dst, want);   // segment pump backs the byte stream
#ifdef _WIN32
    // ── THE live audio read loop: raw WinINet by design, permanently outside
    //    the IHttp seam. Baseline, verbatim. Linux twin = Phase 3 slice 3. ──
    if (raw_pos_ >= raw_buf_.size()) {
        if (stop_.load() || hConn_ == nullptr) return 0;
        raw_buf_.assign(8192, 0);
        DWORD got = 0;
        if (!InternetReadFile(hConn_, raw_buf_.data(), (DWORD)raw_buf_.size(), &got) || got == 0) {
            raw_buf_.clear(); raw_pos_ = 0;
            return 0;
        }
        raw_buf_.resize(got);
        raw_pos_ = 0;
    }
    uint32_t avail = (uint32_t)(raw_buf_.size() - raw_pos_);
    uint32_t n = (want < avail) ? want : avail;
    std::memcpy(dst, raw_buf_.data() + raw_pos_, n);
    raw_pos_ += n;
    return n;
#else
    // ── Slice 3: curl_easy_recv in InternetReadFile's refill slot — the same
    // pull shape, buffered layer, and return-0 semantics. curl_easy_recv is
    // NON-blocking (CURLE_AGAIN when dry), so the wait is ours: recv FIRST
    // (TLS may hold decrypted bytes the raw socket doesn't show — polling
    // first would deadlock on data already in hand), then poll in 100 ms
    // slices with stop_ checked per slice. 8 s of dry socket returns 0,
    // mirroring the WinINet RECEIVE_TIMEOUT failure the producer treats as
    // stream-end (reconnect). Net: stop aborts a stalled read in ~100 ms here
    // vs <=8 s baseline — a named accepted-better delta. ────────────────────
    if (raw_pos_ >= raw_buf_.size()) {
        if (stop_.load() || hConn_ == nullptr) return 0;
        CURL* h = static_cast<CURL*>(hConn_);
        raw_buf_.assign(8192, 0);
        size_t got = 0;
        uint32_t start = port::tickMs();
        for (;;) {
            CURLcode rc = curl_easy_recv(h, raw_buf_.data(), raw_buf_.size(), &got);
            if (rc == CURLE_OK && got > 0) break;         // data in hand
            if (rc != CURLE_AGAIN) {                      // orderly close (got==0) or error
                raw_buf_.clear(); raw_pos_ = 0;
                return 0;
            }
            if (stop_.load() || (long)(port::tickMs() - start) >= 8000) {
                raw_buf_.clear(); raw_pos_ = 0;
                return 0;
            }
            icyWaitSocket(h, POLLIN, 100);
        }
        raw_buf_.resize(got);
        raw_pos_ = 0;
    }
    uint32_t avail = (uint32_t)(raw_buf_.size() - raw_pos_);
    uint32_t n = (want < avail) ? want : avail;
    std::memcpy(dst, raw_buf_.data() + raw_pos_, n);
    raw_pos_ += n;
    return n;
#endif
}

bool StreamSource::rawReadExact(void* dst, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n > 0) {
        uint32_t got = rawRead(p, n);
        if (got == 0) return false;   // stream ended mid-block
        p += got; n -= got;
    }
    return true;
}

uint32_t StreamSource::readAudio(void* out, uint32_t want) {
    if (icy_metaint_ <= 0) {
        // No inline metadata — straight passthrough. HLS also lands here
        // (metaint 0): hlsRawRead already served post-ID3-strip bytes, so the
        // tee sees pure elementary stream on every path.
        uint32_t got = rawRead(out, want);
        teeWrite(out, got);            // copy/remux tee: post-transport bytes
        return got;
    }

    uint8_t* dst = static_cast<uint8_t*>(out);
    uint32_t produced = 0;
    while (produced < want) {
        if (icy_counter_ <= 0) {
            // Metadata block: 1 length byte, then (len*16) bytes.
            uint8_t lenb = 0;
            if (!rawReadExact(&lenb, 1)) break;
            int mlen = (int)lenb * 16;
            if (mlen > 0) {
                std::string block((size_t)mlen, '\0');
                if (!rawReadExact(&block[0], (uint32_t)mlen)) break;
                parseIcyMetadata(block);
            }
            icy_counter_ = icy_metaint_;
        }
        uint32_t chunk = want - produced;
        if (chunk > (uint32_t)icy_counter_) chunk = (uint32_t)icy_counter_;
        uint32_t got = rawRead(dst + produced, chunk);
        if (got == 0) break;
        produced     += got;
        icy_counter_ -= (int)got;
    }
    teeWrite(out, produced);           // copy/remux tee: metaint already stripped
    return produced;
}

// ─── copy/remux tee (abi-cluster slice B) ─────────────────────────────────────
// See the header's threading contract. The writer never stalls (drop-oldest);
// the reader detects being lapped and resyncs to the live tail.

void StreamSource::teeWrite(const void* src, uint32_t n) {
    if (n == 0 || !tee_armed_.load(std::memory_order_relaxed)) return;
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint64_t w   = tee_w_.load(std::memory_order_relaxed);
    uint32_t off = (uint32_t)(w & (TEE_RING_BYTES - 1));
    uint32_t first = (n < TEE_RING_BYTES - off) ? n : TEE_RING_BYTES - off;
    std::memcpy(tee_.data() + off, s, first);
    if (n > first) std::memcpy(tee_.data(), s + first, n - first);
    tee_w_.store(w + n, std::memory_order_release);
}

void StreamSource::setEncodedCapture(bool on) {
    if (on) {
        if (tee_.empty()) tee_.resize(TEE_RING_BYTES);   // before arming: the
        // producer only touches tee_ once tee_armed_ is observed true.
        tee_r_ = tee_w_.load(std::memory_order_acquire); // fresh session: live tail
        tee_discont_.store(false, std::memory_order_relaxed);
        tee_armed_.store(true, std::memory_order_release);
    } else {
        tee_armed_.store(false, std::memory_order_release);
    }
}

uint32_t StreamSource::readEncoded(uint8_t* dst, uint32_t cap,
                                   int32_t* codec_out, int32_t* discont_out) {
    int32_t codec = encodedCaps();
    if (codec_out)   *codec_out = codec;
    bool disc = tee_discont_.exchange(false, std::memory_order_acq_rel);
    if (!tee_armed_.load(std::memory_order_acquire) || cap == 0 || !dst) {
        if (discont_out) *discont_out = disc ? 1 : 0;
        return 0;
    }
    uint64_t w = tee_w_.load(std::memory_order_acquire);
    if (w - tee_r_ > TEE_RING_BYTES) {           // writer lapped us while idle
        tee_r_ = w - TEE_RING_BYTES;             // drop-oldest: snap to live tail
        disc = true;
    }
    uint32_t n = (uint32_t)((w - tee_r_ < cap) ? (w - tee_r_) : cap);
    if (n > 0) {
        uint32_t off   = (uint32_t)(tee_r_ & (TEE_RING_BYTES - 1));
        uint32_t first = (n < TEE_RING_BYTES - off) ? n : TEE_RING_BYTES - off;
        std::memcpy(dst, tee_.data() + off, first);
        if (n > first) std::memcpy(dst + first, tee_.data(), n - first);
        // Validate AFTER the copy: if the writer advanced past our read span
        // while we copied, the bytes are torn — discard them, resync to the
        // live tail, and report the gap instead of emitting garbage.
        uint64_t w2 = tee_w_.load(std::memory_order_acquire);
        if (w2 - tee_r_ > TEE_RING_BYTES) {
            tee_r_ = w2 - TEE_RING_BYTES;
            if (discont_out) *discont_out = 1;
            if (disc) tee_discont_.store(true, std::memory_order_relaxed);
            return 0;
        }
        tee_r_ += n;
    }
    if (discont_out) *discont_out = disc ? 1 : 0;
    return n;
}

int32_t StreamSource::encodedCaps() const {
    // 1=REMOCT_CODEC_MP3, 2=REMOCT_CODEC_AAC_ADTS (remoct_plugin.h; the
    // adapter is the ABI boundary — these values are pinned by contract).
    if (!producer_thread_.joinable()) return 0;   // what connect decided, 0 before
    return codec_ == Codec::AAC ? 2 : 1;
}

void StreamSource::parseIcyMetadata(const std::string& block) {
    auto p = block.find("StreamTitle='");
    if (p == std::string::npos) return;
    p += 13;                                  // length of "StreamTitle='"
    auto e = block.find("';", p);
    if (e == std::string::npos) e = block.find('\'', p);
    if (e == std::string::npos) return;
    std::string title = block.substr(p, e - p);
    {
        std::lock_guard<std::mutex> lk(now_playing_mtx_);
        if (title == now_playing_) return;    // unchanged
        now_playing_ = title;
    }
    slog("icy: %s", title.c_str());
}

std::string StreamSource::nowPlaying() const {
    std::lock_guard<std::mutex> lk(now_playing_mtx_);
    uint32_t now = port::tickMs();
    while (!np_pub_q_.empty() && (long)(now - np_pub_q_.front().releaseTick) >= 0) {
        np_published_ = np_pub_q_.front().disp;
        np_pub_q_.pop_front();
    }
    // Before the first label comes due (startup), show the freshest commit so the
    // UI is never blank.
    return np_published_.empty() ? now_playing_ : np_published_;
}

std::string StreamSource::currentArtUrl() const {
    std::lock_guard<std::mutex> lk(now_playing_mtx_);
    return iheart_art_;
}

// ─── Decoder ──────────────────────────────────────────────────────────────────

bool StreamSource::initDecoder() {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, CHANNELS, SAMPLE_RATE);
    // Step 1: MP3 only. Forcing the backend skips format-sniffing, which would
    // otherwise try to seek a non-seekable network source.
    cfg.encodingFormat = ma_encoding_format_mp3;

    ma_result r = ma_decoder_init(&StreamSource::onRead, &StreamSource::onSeek,
                                  this, &cfg, &decoder_);
    if (r != MA_SUCCESS) {
        slog("initDecoder: ma_decoder_init rc=%d", (int)r);
        return false;
    }
    decoder_ready_ = true;
    return true;
}

void StreamSource::uninitDecoder() {
    if (decoder_ready_) { ma_decoder_uninit(&decoder_); decoder_ready_ = false; }
}

// ─── miniaudio callbacks (0.11.x signatures) ──────────────────────────────────
// NOTE: if the vendored miniaudio is 0.10.x, the read proc returns size_t (bytes)
// and the seek proc returns ma_bool32 — adjust these two signatures accordingly.

ma_result StreamSource::onRead(ma_decoder* dec, void* out, size_t toRead, size_t* bytesRead) {
    auto* self = static_cast<StreamSource*>(dec->pUserData);
    *bytesRead = 0;

    if (self->stop_.load() || self->hConn_ == nullptr)
        return MA_AT_END;

    uint32_t got = self->readAudio(out, (uint32_t)toRead);   // ICY metadata stripped here

    *bytesRead = (size_t)got;
    if (got == 0)
        return MA_AT_END;           // server closed the stream — producer reconnects

    return MA_SUCCESS;
}

ma_result StreamSource::onSeek(ma_decoder* /*dec*/, ma_int64 /*offset*/, ma_seek_origin /*origin*/) {
    // A live network stream is not truly seekable, but we only ever read forward.
    // ma_decoder_init performs an internal rewind-to-start during setup; reporting
    // success lets init complete. Any resulting byte offset is harmless because MP3
    // frames are self-synchronising — the decoder resyncs on the next frame header.
    return MA_SUCCESS;
}

// ─── Producer thread ──────────────────────────────────────────────────────────

void StreamSource::producerWorker() {
    if (!initDecoder()) {
        last_error_ = "decoder init failed";
        slog("producer: initDecoder FAILED");
        disconnect();
        playing_.store(false);
        return;
    }
    slog("producer: initDecoder OK");

    constexpr ma_uint32 CHUNK = 4096;                  // frames per decode call
    std::vector<int16_t> pcm(CHUNK * CHANNELS);
    int reconnect_attempts = 0;
    bool logged_first = false;

    while (!stop_.load()) {
        // abi-cluster keep-draining: while a recording is active the network
        // keeps being consumed through a playback pause (the host mutes its
        // output post-tap instead). Inactive: the old freeze, byte-for-byte.
        if (paused_.load() && !record_active_.load()) { port::sleepMs(20); continue; }

        if (hls_repin_pending_.exchange(false)) {
            slog("producer: ad-onset live-edge re-pin -> re-handshake");
            tee_discont_.store(true, std::memory_order_relaxed);   // copy tee resyncs
            uninitDecoder();
            disconnect();
            prebuffered_.store(false);
            ringClear();                      // drop stale buffered audio: jump to live + avoid backpressure deadlock
            if (stop_.load()) break;
            if (!connect() || !initDecoder()) { port::sleepMs(500); continue; }
            continue;
        }

        // Backpressure: mirror CDSource — if the ring is over half full, let the
        // consumer drain before decoding more.
        if (ringAvailable() > RING_SIZE / 2) { port::sleepMs(10); continue; }

        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(&decoder_, pcm.data(), CHUNK, &framesRead);

        if (!logged_first) { slog("producer: first read framesRead=%llu",
                                  (unsigned long long)framesRead); logged_first = true; }

        if (framesRead > 0) {
            ringWrite(pcm.data(), (int)framesRead * CHANNELS);
            playing_.store(true);
            if (!prebuffered_.load() && ringAvailable() >= PREBUFFER_SAMPLES) {
                prebuffered_.store(true);
                slog("producer: prebuffered (ring=%d)", ringAvailable());
            }
            reconnect_attempts = 0;
            continue;
        }

        // framesRead == 0 → stream ended or dropped. Tear down and reconnect.
        slog("producer: framesRead=0 -> reconnect (attempt %d)", reconnect_attempts + 1);
        if (stop_.load()) break;
        tee_discont_.store(true, std::memory_order_relaxed);   // copy tee resyncs
        uninitDecoder();
        disconnect();

        if (++reconnect_attempts > 10) {
            last_error_ = "stream lost (max reconnect attempts)";
            playing_.store(false);
            break;
        }
        prebuffered_.store(false);                     // re-buffer after the gap
        port::sleepMs(500 * reconnect_attempts);               // linear backoff
        if (stop_.load()) break;
        if (!connect() || !initDecoder()) continue;    // keep retrying until cap
    }

    uninitDecoder();
    disconnect();
    playing_.store(false);
}

// ─── AAC producer (FDK-AAC, ADTS transport) ───────────────────────────────────

void StreamSource::producerWorkerAAC() {
    aac_dec_ = aacDecoder_Open(TT_MP4_ADTS, 1);
    if (!aac_dec_) {
        last_error_ = "aacDecoder_Open failed";
        slog("producerAAC: aacDecoder_Open FAILED");
        disconnect();
        playing_.store(false);
        return;
    }
    slog("producerAAC: decoder open OK");

    static constexpr int NETBUF  = 8192;
    static constexpr int OUT_MAX = 8 * 2048 * 2;     // >> one HE-AAC frame (2048) * 2ch
    std::vector<uint8_t> net(NETBUF);
    std::vector<INT_PCM> out(OUT_MAX);
    UINT bytes_in_buf       = 0;
    int  reconnect_attempts = 0;
    bool logged_first       = false;

    while (!stop_.load()) {
        // abi-cluster keep-draining: see the producer loop above — same gate.
        if (paused_.load() && !record_active_.load()) { port::sleepMs(20); continue; }
        if (hls_repin_pending_.exchange(false)) {
            slog("producerAAC: ad-onset live-edge re-pin -> re-handshake");
            tee_discont_.store(true, std::memory_order_relaxed);   // copy tee resyncs
            disconnect();
            prebuffered_.store(false);
            ringClear();                      // drop stale buffered audio: jump to live + avoid backpressure deadlock
            if (stop_.load()) break;
            hls_repin_active_ = true;         // this connect() is a re-pin: allow prime-to-music-boundary
            const bool ok = connect();
            hls_repin_active_ = false;
            if (!ok) { port::sleepMs(500); continue; }
            bytes_in_buf = 0;                 // fresh session — drop stale partial frame
            continue;
        }
        if (ringAvailable() > RING_SIZE / 2) { port::sleepMs(10); continue; }   // backpressure

        // Top up the network buffer (append after any carried-over leftover).
        if (bytes_in_buf < NETBUF) {
            uint32_t got = readAudio(net.data() + bytes_in_buf, NETBUF - bytes_in_buf);
            if (got == 0) {
                slog("producerAAC: read ended -> reconnect (attempt %d)", reconnect_attempts + 1);
                if (stop_.load()) break;
                tee_discont_.store(true, std::memory_order_relaxed);   // copy tee resyncs
                disconnect();
                if (++reconnect_attempts > 10) {
                    last_error_ = "stream lost (max reconnect attempts)";
                    playing_.store(false);
                    break;
                }
                prebuffered_.store(false);
                port::sleepMs(500 * reconnect_attempts);
                if (stop_.load()) break;
                if (!connect()) continue;
                bytes_in_buf = 0;            // fresh connection — drop stale partial frame
                continue;
            }
            bytes_in_buf += (UINT)got;
            reconnect_attempts = 0;
        }

        // Feed FDK; it copies what it can and reports the unconsumed remainder.
        UCHAR* p     = net.data();
        UINT   size  = bytes_in_buf;
        UINT   valid = bytes_in_buf;
        aacDecoder_Fill(aac_dec_, &p, &size, &valid);
        UINT consumed = bytes_in_buf - valid;
        if (consumed > 0 && valid > 0)
            std::memmove(net.data(), net.data() + consumed, valid);   // carry leftover
        bytes_in_buf = valid;

        // Decode every complete frame currently buffered.
        for (;;) {
            AAC_DECODER_ERROR e = aacDecoder_DecodeFrame(aac_dec_, out.data(), OUT_MAX, 0);
            if (e == AAC_DEC_NOT_ENOUGH_BITS) break;          // need more network bytes
            if (e != AAC_DEC_OK) continue;                    // skip a bad frame; ADTS resyncs

            CStreamInfo* si = aacDecoder_GetStreamInfo(aac_dec_);
            if (!si || si->numChannels <= 0 || si->frameSize <= 0) continue;

            if (!logged_first) {
                slog("producerAAC: first frame sr=%d ch=%d frameSize=%d",
                     si->sampleRate, si->numChannels, si->frameSize);
                logged_first = true;
            }

            if (si->sampleRate == SAMPLE_RATE && si->numChannels == CHANNELS) {
                // Common HE-AAC case: already 44100/stereo — copy straight in.
                ringWrite(reinterpret_cast<int16_t*>(out.data()), si->frameSize * CHANNELS);
            } else {
                // Off-rate / mono station — convert to 44100/stereo via miniaudio.
                if (!conv_ready_ || conv_in_rate_ != si->sampleRate
                                 || conv_in_ch_ != si->numChannels) {
                    if (conv_ready_) { ma_data_converter_uninit(&conv_, nullptr); conv_ready_ = false; }
                    ma_data_converter_config c = ma_data_converter_config_init(
                        ma_format_s16, ma_format_s16,
                        (ma_uint32)si->numChannels, (ma_uint32)CHANNELS,
                        (ma_uint32)si->sampleRate,  (ma_uint32)SAMPLE_RATE);
                    if (ma_data_converter_init(&c, nullptr, &conv_) == MA_SUCCESS) {
                        conv_ready_   = true;
                        conv_in_rate_ = si->sampleRate;
                        conv_in_ch_   = si->numChannels;
                        slog("producerAAC: resampler %d/%dch -> %d/%dch",
                             si->sampleRate, si->numChannels, SAMPLE_RATE, CHANNELS);
                    }
                }
                if (conv_ready_) {
                    ma_uint64 inFrames  = (ma_uint64)si->frameSize;
                    ma_uint64 outCap    = inFrames * SAMPLE_RATE / (ma_uint64)si->sampleRate + 32;
                    std::vector<int16_t> tmp((size_t)outCap * CHANNELS);
                    ma_uint64 outFrames = outCap;
                    if (ma_data_converter_process_pcm_frames(
                            &conv_, out.data(), &inFrames, tmp.data(), &outFrames) == MA_SUCCESS)
                        ringWrite(tmp.data(), (int)outFrames * CHANNELS);
                }
            }

            playing_.store(true);
            if (!prebuffered_.load() && ringAvailable() >= PREBUFFER_SAMPLES) {
                prebuffered_.store(true);
                slog("producerAAC: prebuffered (ring=%d)", ringAvailable());
            }
        }
    }

    if (conv_ready_) { ma_data_converter_uninit(&conv_, nullptr); conv_ready_ = false; }
    if (aac_dec_)    { aacDecoder_Close(aac_dec_); aac_dec_ = nullptr; }
    disconnect();
    playing_.store(false);
}

// ─── Audio-callback contract (identical shape to CDSource::readFrames) ─────────

uint32_t StreamSource::readFrames(float* dst, uint32_t frame_count) {
    int samples_needed = (int)frame_count * CHANNELS;

    // abi-cluster keep-draining: while a recording is active, a playback pause
    // does NOT silence this read — the ring drains REAL frames (the host mutes
    // its playback output AFTER the recorder tap; one truth, two views). The
    // !prebuffered_ arm is untouched — buffering still silence-pads.
    if ((paused_.load() && !record_active_.load()) || !prebuffered_.load()) {
        std::memset(dst, 0, samples_needed * sizeof(float));
        return frame_count;
    }

    std::vector<int16_t> tmp(samples_needed, 0);
    int got = ringRead(tmp.data(), samples_needed);

    // Underrun: ring couldn't satisfy the request. Drop back to buffering so we
    // refill instead of dribbling broken audio; the unfilled tail stays silent.
    if (got < samples_needed)
        prebuffered_.store(false);

    for (int i = 0; i < samples_needed; ++i)
        dst[i] = (float)tmp[i] / 32768.0f;

    uint64_t drained = frames_drained_.fetch_add(frame_count, std::memory_order_relaxed)
                       + frame_count;
    position_sec_.store((int)(drained / SAMPLE_RATE), std::memory_order_relaxed);
    return frame_count;
}

// ─── int16 SPSC ring (mirrors CDSource exactly) ───────────────────────────────

int StreamSource::ringAvailable() const {
    int w = ring_write_.load(std::memory_order_acquire);
    int r = ring_read_.load(std::memory_order_acquire);
    return (w >= r) ? (w - r) : (RING_SIZE - r + w);
}

void StreamSource::ringWrite(const int16_t* data, int samples) {
    // Producer thread only — single writer.
    int w = ring_write_.load(std::memory_order_relaxed);
    for (int i = 0; i < samples; ++i) {
        ring_[w] = data[i];
        w = (w + 1) % RING_SIZE;
    }
    ring_write_.store(w, std::memory_order_release);
}

int StreamSource::ringRead(int16_t* dst, int samples) {
    // Audio callback only — single reader, lock-free.
    int avail = ringAvailable();
    int n = std::min(samples, avail);
    int r = ring_read_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        dst[i] = ring_[r];
        r = (r + 1) % RING_SIZE;
    }
    ring_read_.store(r, std::memory_order_release);
    return n;
}

void StreamSource::ringClear() {
    // Producer-side flush, used on a live-edge re-pin to discard buffered audio so
    // we restart at the live edge rather than replaying the stale (pre-ad) buffer.
    // Safe here: the consumer is gated off (prebuffered_=false) during the re-pin,
    // so it isn't touching ring_read_ concurrently. Snapping read up to write makes
    // the ring read as empty, which clears the producer's backpressure wait and
    // avoids the full-ring / not-prebuffered deadlock that silenced playback.
    ring_read_.store(ring_write_.load(std::memory_order_acquire), std::memory_order_release);
    // The buffer just jumped to the live edge; drop labels scheduled against the
    // discarded buffer and snap the published label to the current one so the
    // display follows the jump instead of lagging ~a buffer behind.
    std::lock_guard<std::mutex> lk(now_playing_mtx_);
    np_pub_q_.clear();
    np_published_ = now_playing_;
}

