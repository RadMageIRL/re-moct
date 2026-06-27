#ifdef _WIN32

#include "StreamSource.h"
#include "Log.h"
#include "StringUtils.h"
#include "IHeartDeepLog.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>


// ── Tunables ─────────────────────────────────────────────────────────────────
// How long the now-playing reconciler may sit on the LIVE floor (digital mode)
// before we assume it has stalled (the dual-blind) and force a live-edge re-pin.
// Logs show healthy ad breaks clear the floor in ≤~30s; a real stall ran 7 min.
// 45s sits just above the healthy ceiling so normal breaks self-recover untouched
// while a genuine stall heals fast. Tune here.
static constexpr DWORD LIVE_STALL_MS = 35000;

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
    stop_.store(false);
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
    stop_.store(true);
    // The producer owns the WinINet handles and decoder; it exits within one
    // read chunk because onRead() returns MA_AT_END as soon as stop_ is set,
    // then tears its own resources down. (Promptly interrupting a fully stalled
    // connection is a later hardening item — see note in producerWorker.)
    if (producer_thread_.joinable()) producer_thread_.join();
    playing_.store(false);
    prebuffered_.store(false);
}

// ─── Connection ───────────────────────────────────────────────────────────────

bool StreamSource::connect() {
    if (mode_ == Mode::HLS) return hlsConnect();   // segment-pump setup (no persistent ICY conn)

    hInet_ = InternetOpenA("RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)",
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
}

void StreamSource::disconnect() {
    if (hConn_) { InternetCloseHandle(hConn_); hConn_ = nullptr; }
    if (hInet_) { InternetCloseHandle(hInet_); hInet_ = nullptr; }
}

// ─── HLS increment 1: resolve / poll / fetch (standalone, no audio path) ──────
// Walks the same chain VLC does: master -> variant (media playlist) -> .aac
// segments. Relative refs resolve against each response's post-redirect URL so
// the rj-tok / 50_<session> token propagates. All output goes to slog ([stream],
// %TEMP%\remoct-*.log). None of the continuous-stream path is touched.

bool StreamSource::hlsEnsureSession() {
    if (hInet_) return true;
    hInet_ = InternetOpenA("RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)",
                           INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet_) { slog("hlsEnsureSession: InternetOpenA FAILED err=%lu", GetLastError()); return false; }
    DWORD to = 8000;  // mirror connect() timeouts
    InternetSetOptionA(hInet_, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet_, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet_, INTERNET_OPTION_SEND_TIMEOUT,    &to, sizeof(to));
    return true;
}

// Short-lived GET over the shared session. Captures body (text and/or bytes)
// and, optionally, the post-redirect URL.
bool StreamSource::hlsHttpGet(const std::string& url, std::string* out_text,
                              std::vector<uint8_t>* out_bytes, std::string* out_final_url) {
    if (!hInet_) return false;
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION;
    HINTERNET h = InternetOpenUrlA(hInet_, url.c_str(), nullptr, 0, flags, 0);
    if (!h) { slog("hlsGet: open FAILED err=%lu url=%s", GetLastError(), url.c_str()); return false; }

    if (out_final_url) {                       // URL after redirects (token lives here)
        char fbuf[2048]; DWORD flen = sizeof(fbuf);
        if (InternetQueryOptionA(h, INTERNET_OPTION_URL, fbuf, &flen))
            out_final_url->assign(fbuf, flen);
        else
            *out_final_url = url;
    }

    DWORD status = 0, slen = sizeof(status), sidx = 0;   // non-2xx => session expiry/dead
    if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &slen, &sidx)
        && status >= 400) {
        slog("hlsGet: HTTP %lu url=%s", status, url.c_str());
        InternetCloseHandle(h);
        return false;
    }

    std::vector<uint8_t> buf;
    uint8_t chunk[8192];
    DWORD got = 0;
    for (;;) {
        if (stop_.load()) { InternetCloseHandle(h); return false; }
        if (!InternetReadFile(h, chunk, sizeof(chunk), &got)) {
            slog("hlsGet: read err=%lu url=%s", GetLastError(), url.c_str());
            InternetCloseHandle(h);
            return false;
        }
        if (got == 0) break;                   // EOF
        buf.insert(buf.end(), chunk, chunk + got);
        if (buf.size() > 8u * 1024u * 1024u) { slog("hlsGet: oversized body, abort"); break; }
    }
    InternetCloseHandle(h);

    if (out_text)  out_text->assign(reinterpret_cast<char*>(buf.data()), buf.size());
    if (out_bytes) *out_bytes = std::move(buf);
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
    char out[2048]; DWORD len = sizeof(out);
    if (InternetCombineUrlA(base.c_str(), ref.c_str(), out, &len, ICU_NO_ENCODE))
        return std::string(out, len);
    return ref;                                // fallback: hand back as-is
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
        if (disc && hls_repin_armed_) {
            hls_repin_pending_.store(true);
            hls_repin_armed_ = false;
            hls_repin_cooldown_until_ = GetTickCount() + 90000;   // ~one ad pod
            slog("hlsPollMedia: AD-ONSET discontinuity -> requesting live-edge re-pin");
        } else if (!hls_repin_armed_ &&
                   (long)(GetTickCount() - hls_repin_cooldown_until_) >= 0) {
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
    hls_.last_poll = GetTickCount();
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
std::string StreamSource::hlsBuildDigitalUrl(const std::string& base) {
    if (base.find('?') != std::string::npos) return base;     // already parameterized
    std::string id;
    size_t p = base.find("/zc");
    if (p != std::string::npos) {
        p += 3;
        while (p < base.size() && std::isdigit((unsigned char)base[p])) id += base[p++];
    }
    if (id.empty()) return base;

    static bool seeded = false;
    if (!seeded) { std::srand(GetTickCount()); seeded = true; }
    static const char* H = "0123456789abcdef";
    std::string lid; lid.reserve(32);
    for (int i = 0; i < 32; ++i) lid += H[std::rand() & 15];

    return base + "?streamid=" + id +
        "&zip=&aw_0_1st.playerid=iHeartRadioWebPlayer&clientType=web&companionAds=false"
        "&deviceName=web-mobile&dist=iheart&host=webapp.US&listenerId=" + lid +
        "&playedFrom=157&pname=live_profile&stationid=" + id +
        "&terminalId=159&territory=US&us_privacy=1-N-";
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
    hls_.master_url = want_digital ? hlsBuildDigitalUrl(master) : master;
    // iHeart now-playing: primary source is the variant manifest's EXTINF tags
    // (freeze-proof), trackHistory module as fallback. Set this up BEFORE the initial
    // poll so hlsPollMedia parses the manifest on connect — otherwise the producer's
    // first trackHistory fallback can clobber the (not-yet-set) manifest result on a
    // station switch. resolve() is url-aware, so switching re-resolves cleanly.
    is_iheart_          = IHeartRadio::isIHeartUrl(url_);
    last_iheart_poll_   = 0;
    iheart_th_cache_.clear();
    iheart_th_ended_    = -1;
    ih_state_ = ih_pending_ = IHNow::Live;
    ih_state_disp_.clear(); ih_pending_disp_.clear(); ih_streak_ = 0;
    if (is_iheart_) {
        iheart_.setLogger([](const std::string& s){ slog("%s", s.c_str()); });
        iheart_.resolve(url_);   // sidecar-first; cheap; gives us stationName() for the ad label
    }
    // Resolve + initial poll. If a requested DIGITAL attempt fails at any step, fall
    // back to the raw broadcast URL once, so a changed/blocked handshake degrades to
    // exactly today's behavior (never worse).
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
    // Prime near the live edge: keep only the last ~2 queued segments so we start
    // close to live instead of ~a full window behind. last_seq is already at the
    // window top from the full poll, so subsequent polls only pick up newer ones.
    if (hls_.pending.size() > 2)
        hls_.pending.erase(hls_.pending.begin(), hls_.pending.end() - 2);
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
enum class IHeartMfCls { None, Song, Ad };

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
void StreamSource::updateIHeartNowPlaying(const std::string& body) {
    std::string mfArtist, mfTitle;
    IHeartMfCls cls   = classifyIHeartManifest(body, mfArtist, mfTitle);
    std::string mfSong = (cls == IHeartMfCls::Song) ? (mfArtist + " - " + mfTitle) : std::string();

    // trackHistory snapshot (throttled ~9s; caches song + staleness between polls).
    DWORD nowtick = GetTickCount();
    if (last_iheart_poll_ == 0 || nowtick - last_iheart_poll_ >= 9000) {
        last_iheart_poll_ = nowtick;
        long ended = -1;
        iheart_th_cache_ = iheart_.resolve(url_) ? iheart_.pollNowPlaying(&ended) : std::string();
        iheart_th_ended_ = ended;
        // currentTrackMeta: the web player's own now-playing source. Polled while the
        // deep log is on (probe), OR in digital mode — there ctm rides the same timeline
        // as the audio and carries the album-art URL we surface to Discord.
        if (IHeartDeepLog::enabled() || digital_active_.load())
            iheart_.pollCurrentTrackMeta(&iheart_ctm_);
    }
    // Album art for Discord RP: in DIGITAL mode, currentTrackMeta rides the same
    // timeline as the audio and carries a cover URL. Publish it only when a SONG is the
    // committed state AND ctm agrees with it (title match), so ad/boundary skew never
    // paints a wrong cover. The UI falls back to the logo whenever this is empty.
    {
        std::string art;
        if (digital_active_.load() && ih_state_ == IHNow::Song &&
            iheart_ctm_.ok && !iheart_ctm_.imagePath.empty()) {
            auto squash = [](const std::string& s){
                std::string o; for (unsigned char c : s)
                    if (std::isalnum(c)) o += (char)std::tolower(c);
                return o;
            };
            std::string t = squash(iheart_ctm_.title);
            if (!t.empty() && squash(ih_state_disp_).find(t) != std::string::npos) {
                art = iheart_ctm_.imagePath;
                if (art.rfind("http://", 0) == 0) art = "https://" + art.substr(7); // Discord media proxy prefers https
            }
        }
        std::lock_guard<std::mutex> lk(now_playing_mtx_);
        iheart_art_ = art;
    }

    // CUR is compared against (serverNow - endTime), but our listener sits ~20s behind
    // the live edge (we prime 2x10s segments on connect). So a song still audible at the
    // speaker has, on the server clock, already passed endTime by roughly the buffer depth.
    // CUR=20 expired the song before the audio did -> mid-song flip to "LIVE". Widen to 60
    // to cover buffer (~20s) + normal song tail. Regression-safe: a genuine trackHistory
    // freeze still climbs past 60 within a minute, dropping thCurrent and flooring to LIVE
    // (or Ad) as before — just ~40s later. A real new track commits in 1 tick and overrides
    // any just-finished-song overhang immediately.
    const long CUR = 60;
    bool thCurrent = !iheart_th_cache_.empty() && iheart_th_ended_ <= CUR;                       // playing now

    // Confidence-ordered target for this tick.
    std::string st = iheart_.stationName();
    IHNow tgtKind; std::string tgtDisp;
    if (!mfSong.empty())             { tgtKind = IHNow::Song; tgtDisp = mfSong; }            // manifest song (Breeze) — highest confidence
    else if (cls == IHeartMfCls::Ad) {                                                       // active in-band ad: "Spot Block" w/ real length, or song_spot="T".
        tgtKind = IHNow::Ad;                                                                 // A commercial is airing NOW, so OVERRIDE the schedule-based
        tgtDisp = st.empty() ? std::string("Commercial break")                               // trackHistory song (which can run ahead of the broadcast and
                             : (st + " - Commercial break");                                 // otherwise paints a song over a live ad).
    }
    else if (thCurrent)              { tgtKind = IHNow::Song; tgtDisp = iheart_th_cache_; }  // trackHistory current song (Z100, when no live ad signal)
    else                             { tgtKind = IHNow::Live;                                // murk / no signal -> honest LIVE
                                       tgtDisp = st.empty() ? std::string("LIVE") : (st + " - LIVE"); }

    // ── Deep-analysis capture (opt-in, Ctrl+A; no-op unless enabled). Records the
    //    full tick — inputs, target, debounce state, and live audio position —
    //    BEFORE the early-return debounce, so a metadata freeze still emits ticks. ──
    {
        IHeartDeepLog::Record dr;
        dr.stationId = iheart_.stationId();
        dr.station   = st;
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
                    // =, optional backslash, opening quote — not the whole quote run, or an
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
            }
        }
        dr.th        = iheart_th_cache_;
        dr.thEnded   = iheart_th_ended_;
        dr.thCurrent = thCurrent;
        dr.tgtKind   = (tgtKind == IHNow::Song) ? "Song" : (tgtKind == IHNow::Ad ? "Ad" : "Live");
        dr.tgtDisp   = tgtDisp;
        dr.stState   = (ih_state_ == IHNow::Song) ? "Song" : (ih_state_ == IHNow::Ad ? "Ad" : "Live");
        dr.stDisp    = ih_state_disp_;
        dr.pendKind  = (ih_pending_ == IHNow::Song) ? "Song" : (ih_pending_ == IHNow::Ad ? "Ad" : "Live");
        dr.pendDisp  = ih_pending_disp_;
        dr.streak    = ih_streak_;
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
        if (!is_lane_) IHeartDeepLog::emit(dr);
    }

    // Live-floor stall self-heal (digital only) — the second re-pin trigger,
    // feeding the same flag/arm/cooldown as the discontinuity path. Most ad breaks
    // drop us onto the LIVE floor briefly and the reconciler re-acquires the next
    // song on its own; occasionally it stalls there indefinitely (the dual-blind)
    // and only a re-handshake recovers it. So once we've held the floor past
    // LIVE_STALL_MS, request the live-edge re-pin. Placed BEFORE the early-return
    // below because while stalled the state never changes (that return fires every
    // tick), so this is the only spot that runs during the stall.
    if (digital_active_.load() && ih_state_ == IHNow::Live) {
        if (ih_live_since_ == 0) {
            ih_live_since_ = GetTickCount();
        } else if (hls_repin_armed_ &&
                   (GetTickCount() - ih_live_since_) >= LIVE_STALL_MS) {
            hls_repin_pending_.store(true);
            hls_repin_armed_ = false;
            hls_repin_cooldown_until_ = GetTickCount() + 90000;
            slog("updateIHeart: LIVE-floor stall %lus -> requesting live-edge re-pin",
                 (unsigned long)((GetTickCount() - ih_live_since_) / 1000));
            ih_live_since_ = 0;                 // restart floor timer after acting
        }
    } else {
        ih_live_since_ = 0;                     // off the floor (or raw) -> reset timer
    }

    // Dual-stream staging lane (coordinator only). Runs every tick BEFORE the
    // debounce early-returns below, same rationale as the LIVE-floor block: while
    // stalled the committed state never changes, so this is the only spot that
    // runs during an ad. Stage A: shadow observer (arm/watch/log/teardown).
    if (!is_lane_) serviceStaging();

    // Asymmetric debounce: Song needs 1 tick (instant), Ad 3, Live 2.
    if (tgtKind == ih_state_ && tgtDisp == ih_state_disp_) { ih_streak_ = 0; return; }  // already committed
    if (tgtKind == ih_pending_ && tgtDisp == ih_pending_disp_) ih_streak_++;
    else { ih_pending_ = tgtKind; ih_pending_disp_ = tgtDisp; ih_streak_ = 1; }
    int need = (tgtKind == IHNow::Song) ? 1 : (tgtKind == IHNow::Ad ? 3 : 2);
    if (ih_streak_ < need) return;                                                      // hold current display

    ih_state_ = tgtKind; ih_state_disp_ = tgtDisp; ih_streak_ = 0;                      // commit
    std::string disp = sanitizeForDisplay(tgtDisp);
    std::lock_guard<std::mutex> lk(now_playing_mtx_);
    if (disp != now_playing_) {
        now_playing_ = disp;
        // Hold this label until its audio is heard: delay = buffer depth ahead of
        // the speaker = ring seconds + undecoded pending segments. Self-calibrating,
        // so it tracks rebuffers and collapses to ~0 right after a re-pin flush.
        long ring_ms = (long)(1000.0 * (double)ringAvailable() / (double)(SAMPLE_RATE * CHANNELS));
        long pend_ms = (long)hls_.pending.size() * (long)hls_.target_dur_ms;
        np_pub_q_.push_back({ GetTickCount() + (DWORD)(ring_ms + pend_ms), disp });
        const char* via = (tgtKind==IHNow::Song) ? "song" : (tgtKind==IHNow::Ad ? "ad" : "live");
        slog("iheart [%s]: %s", via, disp.c_str());
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
    DWORD now = GetTickCount();

    auto teardown = [&](const char* why, DWORD cooldown_ms) {
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
        if (digital_active_.load() && ih_state_ == IHNow::Live &&
            (long)(now - stg_cooldown_until_) >= 0 &&
            !stg_opening_.load() && !staging_) {
            staging_ = std::make_unique<StreamSource>(true);   // is_lane_ = true
            staging_->setPreferDigital(true);
            stg_opening_.store(true);
            stg_state_ = Stg::Opening;
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
        if (ih_state_ != IHNow::Live) {                        // primary's break cleared on its own
            teardown("primary break cleared (no blend needed)", 15000);
        } else if (staging_ && staging_->isPrebuffered() && staging_->edgeIsMusic()) {
            slog("staging: READY at music edge — clean blend point reached "
                 "(Stage A observer; B/C will swap/crossfade here)");
            teardown("Stage A: observed only", 60000);
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

DWORD StreamSource::hlsRawRead(void* dst, DWORD want) {
    // 1. Serve from the current segment if bytes remain.
    if (hls_.seg_pos < hls_.seg.size()) {
        DWORD avail = (DWORD)(hls_.seg.size() - hls_.seg_pos);
        DWORD n = (want < avail) ? want : avail;
        std::memcpy(dst, hls_.seg.data() + hls_.seg_pos, n);
        hls_.seg_pos += n;
        return n;
    }
    // 2. Current segment drained — make sure we have a pending segment to fetch.
    int repoll_fail = 0;
    while (hls_.pending.empty()) {
        if (stop_.load()) return 0;
        DWORD since = GetTickCount() - hls_.last_poll;
        if (since < (DWORD)hls_.target_dur_ms) { Sleep(50); continue; }   // pace polling
        if (!hlsPollMedia()) {
            // Variant GET failed — session token likely expired. Refresh via master.
            if (!hlsResolveMaster() || !hlsPollMedia()) {
                if (++repoll_fail >= 3) { slog("hlsRawRead: giving up after repoll failures"); return 0; }
                if (stop_.load()) return 0;
                Sleep(300);
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

DWORD StreamSource::rawRead(void* dst, DWORD want) {
    if (mode_ == Mode::HLS) return hlsRawRead(dst, want);   // segment pump backs the byte stream
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
    DWORD avail = (DWORD)(raw_buf_.size() - raw_pos_);
    DWORD n = (want < avail) ? want : avail;
    std::memcpy(dst, raw_buf_.data() + raw_pos_, n);
    raw_pos_ += n;
    return n;
}

bool StreamSource::rawReadExact(void* dst, DWORD n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n > 0) {
        DWORD got = rawRead(p, n);
        if (got == 0) return false;   // stream ended mid-block
        p += got; n -= got;
    }
    return true;
}

DWORD StreamSource::readAudio(void* out, DWORD want) {
    if (icy_metaint_ <= 0)
        return rawRead(out, want);     // no inline metadata — straight passthrough

    uint8_t* dst = static_cast<uint8_t*>(out);
    DWORD produced = 0;
    while (produced < want) {
        if (icy_counter_ <= 0) {
            // Metadata block: 1 length byte, then (len*16) bytes.
            uint8_t lenb = 0;
            if (!rawReadExact(&lenb, 1)) break;
            int mlen = (int)lenb * 16;
            if (mlen > 0) {
                std::string block((size_t)mlen, '\0');
                if (!rawReadExact(&block[0], (DWORD)mlen)) break;
                parseIcyMetadata(block);
            }
            icy_counter_ = icy_metaint_;
        }
        DWORD chunk = want - produced;
        if (chunk > (DWORD)icy_counter_) chunk = (DWORD)icy_counter_;
        DWORD got = rawRead(dst + produced, chunk);
        if (got == 0) break;
        produced     += got;
        icy_counter_ -= (int)got;
    }
    return produced;
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
    DWORD now = GetTickCount();
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

    DWORD got = self->readAudio(out, (DWORD)toRead);   // ICY metadata stripped here

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
        if (paused_.load()) { Sleep(20); continue; }

        if (hls_repin_pending_.exchange(false)) {
            slog("producer: ad-onset live-edge re-pin -> re-handshake");
            uninitDecoder();
            disconnect();
            prebuffered_.store(false);
            ringClear();                      // drop stale buffered audio: jump to live + avoid backpressure deadlock
            if (stop_.load()) break;
            if (!connect() || !initDecoder()) { Sleep(500); continue; }
            continue;
        }

        // Backpressure: mirror CDSource — if the ring is over half full, let the
        // consumer drain before decoding more.
        if (ringAvailable() > RING_SIZE / 2) { Sleep(10); continue; }

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
        uninitDecoder();
        disconnect();

        if (++reconnect_attempts > 10) {
            last_error_ = "stream lost (max reconnect attempts)";
            playing_.store(false);
            break;
        }
        prebuffered_.store(false);                     // re-buffer after the gap
        Sleep(500 * reconnect_attempts);               // linear backoff
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
        if (paused_.load()) { Sleep(20); continue; }
        if (hls_repin_pending_.exchange(false)) {
            slog("producerAAC: ad-onset live-edge re-pin -> re-handshake");
            disconnect();
            prebuffered_.store(false);
            ringClear();                      // drop stale buffered audio: jump to live + avoid backpressure deadlock
            if (stop_.load()) break;
            if (!connect()) { Sleep(500); continue; }
            bytes_in_buf = 0;                 // fresh session — drop stale partial frame
            continue;
        }
        if (ringAvailable() > RING_SIZE / 2) { Sleep(10); continue; }   // backpressure

        // Top up the network buffer (append after any carried-over leftover).
        if (bytes_in_buf < NETBUF) {
            DWORD got = readAudio(net.data() + bytes_in_buf, NETBUF - bytes_in_buf);
            if (got == 0) {
                slog("producerAAC: read ended -> reconnect (attempt %d)", reconnect_attempts + 1);
                if (stop_.load()) break;
                disconnect();
                if (++reconnect_attempts > 10) {
                    last_error_ = "stream lost (max reconnect attempts)";
                    playing_.store(false);
                    break;
                }
                prebuffered_.store(false);
                Sleep(500 * reconnect_attempts);
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

    if (paused_.load() || !prebuffered_.load()) {
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

#endif // _WIN32
