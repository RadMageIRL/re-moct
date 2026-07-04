// icy_pipeline_test — Phase 3 slice 3: the ICY/continuous path, replayed
// headlessly against a REAL localhost ICY server fixture on BOTH platforms.
//
// This closes the slice-0 honest limit ("the ICY path cannot be replayed
// headlessly — live gates only"): the fixture is a real TCP server speaking the
// ICY protocol (ICY 200 OK status, icy-metaint interleaving, StreamTitle
// blocks), so the REAL StreamSource Continuous pipeline runs end-to-end —
// open() -> connect (raw WinINet on Windows / the slice-3 curl twin on Linux)
// -> producerWorkerAAC -> readAudio de-interleave -> FDK decode -> ring ->
// readFrames. Nothing in StreamSource is faked or injected.
//
// Baseline-first (the slice-B pattern): this test was run against the
// untouched Windows WinINet path BEFORE the Linux twin existed, so it pins the
// baseline semantics the twin must reproduce:
//   1. connect sends Icy-MetaData: 1; icy-metaint honored from the response
//   2. audio flows (prebuffer -> real decoded energy through readFrames)
//   3. THE offset-0 invariant: StreamTitle blocks parse EXACTLY at metaint
//      boundaries across two title transitions — a single-byte slip garbles
//      both the ADTS decode and the titles, failing 2+3 deterministically
//   4. server-side drop -> producer reconnects and self-heals (fixture sees a
//      second connection, audio + fresh metadata resume)
//   5. close() during a silent (blocked-read) stream returns bounded — <=8s
//      receive-timeout class on Windows, ~100ms stop_-poll class on Linux
//   6. a response without icy-metaint -> passthrough (no de-interleave)
//
// BASELINE FINDING (this test's first run, 2026-07-03): WinINet does NOT
// surface `icy-metaint` from a response whose status line is "ICY 200 OK"
// (HttpQueryInfoA finds nothing -> icy_metaint_=0) — on a true SHOUTcast v1
// server the Windows baseline plays audio with the metadata blocks bleeding
// into the decoder (ADTS resync swallows them) and titles never parse. Every
// live-verified Windows station speaks "HTTP/1.0 200 OK" (the wild probe found
// 11/12 doing so). Scenario A therefore uses the HTTP/1.0 status shape (both
// platforms de-interleave identically); scenario C pins the ICY-status shape
// per-platform: Windows = plays-without-titles (today's reality, byte-verbatim
// sacred), Linux twin = parses it (a NAMED accepted-better delta, same class
// as the stop_-poll latency).
//
// The AAC codec is used (URL carries ".aac") because the fixture reuses the
// runtime FDK-encoded ADTS generator (no binary fixtures, the hls_pipeline_test
// precedent). The transport + de-interleave under test sit BELOW the codec
// split — the MP3 continuous decoder stays covered by the live gates.

#define MINIAUDIO_IMPLEMENTATION            // StreamSource.cpp holds decls only
#include "miniaudio.h"

#include "StreamSource.h"
#include "PortUtil.h"   // sleepMs/tickMs (baseline Sleep/GetTickCount on Windows)

#include <fdk-aac/aacenc_lib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── platform shim (the http_cancel_test shape) ───────────────────────────────
#ifdef _WIN32
using SockT = SOCKET;
static constexpr SockT kBadSock = INVALID_SOCKET;
static void closeSock(SockT s)  { closesocket(s); }
static bool initNet()           { WSADATA w; return WSAStartup(MAKEWORD(2,2), &w) == 0; }
static void cleanupNet()        { WSACleanup(); }
static int  sendFlags()         { return 0; }
using SockLenT = int;
static void capSocketWaits(SockT c, unsigned ms) {
    DWORD v = ms;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
}
#else
using SockT = int;
static constexpr SockT kBadSock = -1;
static void closeSock(SockT s)  { ::close(s); }
static bool initNet()           { signal(SIGPIPE, SIG_IGN); return true; }
static void cleanupNet()        {}
static int  sendFlags()         { return MSG_NOSIGNAL; }
using SockLenT = socklen_t;
static void capSocketWaits(SockT c, unsigned ms) {
    struct timeval tv { (time_t)(ms / 1000u), (suseconds_t)((ms % 1000u) * 1000u) };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

// Stop a LISTENING socket so a thread blocked in accept() wakes (POSIX close()
// alone does NOT wake it — the http_cancel_test lesson: shutdown first).
static void stopListener(SockT s) {
#ifndef _WIN32
    ::shutdown(s, SHUT_RDWR);
#endif
    closeSock(s);
}

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ─── Fixture audio: real ADTS (FDK-encoded stereo 44100 sine) ─────────────────
// Verbatim generator shape from hls_pipeline_test — no binary fixtures.

static std::vector<uint8_t> encodeAdtsSine(double secs, double freq, double amp) {
    std::vector<uint8_t> out;
    HANDLE_AACENCODER enc = nullptr;
    if (aacEncOpen(&enc, 0, 2) != AACENC_OK) return out;
    aacEncoder_SetParam(enc, AACENC_AOT, 2);              // AAC-LC
    aacEncoder_SetParam(enc, AACENC_SAMPLERATE, 44100);
    aacEncoder_SetParam(enc, AACENC_CHANNELMODE, MODE_2);
    aacEncoder_SetParam(enc, AACENC_BITRATE, 128000);
    aacEncoder_SetParam(enc, AACENC_TRANSMUX, TT_MP4_ADTS);
    if (aacEncEncode(enc, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
        aacEncClose(&enc); return out;
    }
    AACENC_InfoStruct info{};
    aacEncInfo(enc, &info);
    const int frame = (int)info.frameLength;              // 1024 for LC
    const int total = (int)(secs * 44100.0);

    std::vector<INT_PCM> pcm((size_t)frame * 2);
    std::vector<uint8_t> obuf(8192);
    int pos = 0;
    while (pos < total) {
        for (int i = 0; i < frame; ++i) {
            double t = (double)(pos + i) / 44100.0;
            INT_PCM s = (INT_PCM)(amp * 32767.0 * std::sin(2.0 * 3.14159265358979 * freq * t));
            pcm[(size_t)i * 2]     = s;
            pcm[(size_t)i * 2 + 1] = s;
        }
        void* inPtr  = pcm.data();
        INT   inId   = IN_AUDIO_DATA, inSize = (INT)(pcm.size() * sizeof(INT_PCM)), inEl = sizeof(INT_PCM);
        void* outPtr = obuf.data();
        INT   outId  = OUT_BITSTREAM_DATA, outSize = (INT)obuf.size(), outEl = 1;
        AACENC_BufDesc inDesc{};  inDesc.numBufs = 1;  inDesc.bufs = &inPtr;
        inDesc.bufferIdentifiers = &inId;   inDesc.bufSizes = &inSize;  inDesc.bufElSizes = &inEl;
        AACENC_BufDesc outDesc{}; outDesc.numBufs = 1; outDesc.bufs = &outPtr;
        outDesc.bufferIdentifiers = &outId; outDesc.bufSizes = &outSize; outDesc.bufElSizes = &outEl;
        AACENC_InArgs  inArgs{};  inArgs.numInSamples = frame * 2;
        AACENC_OutArgs outArgs{};
        if (aacEncEncode(enc, &inDesc, &outDesc, &inArgs, &outArgs) != AACENC_OK) break;
        out.insert(out.end(), obuf.data(), obuf.data() + outArgs.numOutBytes);
        pos += frame;
    }
    aacEncClose(&enc);
    return out;
}

// ─── The ICY fixture server ───────────────────────────────────────────────────
// A real TCP listener speaking SHOUTcast: ICY (or HTTP/1.0) status line,
// icy-metaint header, then interleaved [metaint audio bytes][metadata block]
// forever. The audio is the ADTS buffer looped (frame-aligned end, so the
// splice is a legal ADTS stream). Titles are sent in the metadata slot on
// change, zero-length blocks otherwise — the real-station shape.

struct IcyServer {
    // config (set before start())
    std::string status  = "ICY 200 OK";
    int         metaint = 4096;               // 0 = omit the header (passthrough case)
    int         pace_ms = 100;                // per audio block (~2.5x realtime @128kbps)

    // live controls
    std::atomic<bool> stall { false };        // stop sending, keep the connection open
    std::atomic<bool> drop  { false };        // one-shot: close the current connection
    std::atomic<bool> quit  { false };
    std::atomic<int>  connections { 0 };

    std::mutex  mtx;
    std::string title;                        // current StreamTitle (sent on change)
    std::string first_request;                // captured client request headers

    std::vector<uint8_t> audio;               // looped ADTS payload
    size_t apos = 0;

    SockT       lsock = kBadSock;
    int         port  = 0;
    std::thread th;

    bool start() {
        lsock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lsock == kBadSock) return false;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::bind(lsock, (sockaddr*)&a, sizeof(a)) != 0) return false;
        if (::listen(lsock, 4) != 0) return false;
        SockLenT alen = sizeof(a);
        if (getsockname(lsock, (sockaddr*)&a, &alen) != 0) return false;
        port = ntohs(a.sin_port);
        th = std::thread([this] { run(); });
        return true;
    }
    void stop() {
        quit.store(true);
        if (lsock != kBadSock) { stopListener(lsock); lsock = kBadSock; }
        if (th.joinable()) th.join();
    }
    ~IcyServer() { stop(); }

    bool sendAll(SockT c, const void* p, size_t len) {
        const char* q = (const char*)p;
        while (len > 0) {
            int n = (int)::send(c, q, (int)len, sendFlags());
            if (n <= 0) return false;
            q += n; len -= (size_t)n;
        }
        return true;
    }

    bool serveAudio(SockT c, size_t want) {    // `want` bytes of the looped ADTS
        while (want > 0) {
            if (apos >= audio.size()) apos = 0;
            size_t n = std::min(want, audio.size() - apos);
            if (!sendAll(c, audio.data() + apos, n)) return false;
            apos += n; want -= n;
        }
        return true;
    }

    void run() {
        while (!quit.load()) {
            SockT c = ::accept(lsock, nullptr, nullptr);
            if (c == kBadSock) break;                    // listener stopped
            ++connections;
            capSocketWaits(c, 10000);

            // Request headers (loose parse; capture the first for asserts).
            std::string req; char b[1024];
            while (req.find("\r\n\r\n") == std::string::npos) {
                int n = (int)::recv(c, b, sizeof(b), 0);
                if (n <= 0) break;
                req.append(b, n);
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (first_request.empty()) first_request = req;
            }

            std::string hdr = status + "\r\n";
            if (metaint > 0) hdr += "icy-metaint:" + std::to_string(metaint) + "\r\n";
            hdr += "icy-name:ICY Fixture\r\ncontent-type:audio/aacp\r\n\r\n";
            if (!sendAll(c, hdr.data(), hdr.size())) { closeSock(c); continue; }

            std::string last_sent;                       // per-connection, as real servers
            bool alive = true;
            while (alive && !quit.load() && !drop.load()) {
                if (stall.load()) { port::sleepMs(20); continue; }
                alive = serveAudio(c, (size_t)(metaint > 0 ? metaint : 4096));
                if (!alive || metaint <= 0) { if (alive) port::sleepMs((unsigned)pace_ms); continue; }
                std::string t;
                { std::lock_guard<std::mutex> lk(mtx); t = title; }
                if (!t.empty() && t != last_sent) {
                    std::string m = "StreamTitle='" + t + "';";
                    m.append((16 - m.size() % 16) % 16, '\0');
                    uint8_t len = (uint8_t)(m.size() / 16);
                    alive = sendAll(c, &len, 1) && sendAll(c, m.data(), m.size());
                    if (alive) last_sent = t;
                } else {
                    uint8_t z = 0;
                    alive = sendAll(c, &z, 1);
                }
                port::sleepMs((unsigned)pace_ms);
            }
            closeSock(c);
            drop.store(false);                           // one-shot consumed
        }
    }

    void setTitle(const std::string& t) { std::lock_guard<std::mutex> lk(mtx); title = t; }
};

// ─── Consumer helpers ─────────────────────────────────────────────────────────

static double rmsOf(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double acc = 0.0;
    for (float s : v) acc += (double)s * s;
    return std::sqrt(acc / (double)v.size());
}

static std::vector<float> drainFrames(StreamSource& ss, int frames) {
    std::vector<float> all;
    all.reserve((size_t)frames * 2);
    float buf[512 * 2];
    int left = frames;
    while (left > 0) {
        int n = left < 512 ? left : 512;
        ss.readFrames(buf, (uint32_t)n);
        all.insert(all.end(), buf, buf + (size_t)n * 2);
        left -= n;
        port::sleepMs(1);
    }
    return all;
}

template <typename Pred>
static bool waitFor(int timeout_ms, Pred pred) {
    uint32_t t0 = port::tickMs();
    while ((long)(port::tickMs() - t0) < timeout_ms) {
        if (pred()) return true;
        port::sleepMs(10);
    }
    return false;
}

// Wait for a condition while KEEPING THE RING DRAINED — the producer only
// touches the network while the ring has room (backpressure at half), so any
// wait that needs network progress (metadata blocks, noticing a server drop)
// must drain, exactly as the real audio callback always does.
template <typename Pred>
static bool drainWaitFor(StreamSource& ss, int timeout_ms, Pred pred) {
    uint32_t t0 = port::tickMs();
    float buf[512 * 2];
    while ((long)(port::tickMs() - t0) < timeout_ms) {
        ss.readFrames(buf, 512);
        if (pred()) return true;
        port::sleepMs(5);
    }
    return false;
}

static bool waitTitle(StreamSource& ss, const std::string& want, int timeout_ms) {
    return drainWaitFor(ss, timeout_ms, [&] { return ss.nowPlaying() == want; });
}

int main() {
    if (!initNet()) { std::printf("FAIL initNet\n"); return 1; }

    std::vector<uint8_t> adts = encodeAdtsSine(4.0, 440.0, 0.5);
    CHECK(adts.size() > 20000);                          // encoder actually produced ADTS
    CHECK(adts.size() >= 2 && adts[0] == 0xFF && (adts[1] & 0xF6) == 0xF0);

    // ── Scenario A: the full ICY contract (HTTP/1.0 status, metaint 4096) ────
    // The status shape every live-verified station uses — both platforms parse
    // icy-metaint and run the de-interleave identically here.
    {
        IcyServer srv;
        srv.audio  = adts;
        srv.status = "HTTP/1.0 200 OK";
        srv.setTitle("Alpha One");
        CHECK(srv.start());

        StreamSource ss(core::http());   // slice c: HTTP injected (unused on the raw ICY path)
        std::string url = "http://127.0.0.1:" + std::to_string(srv.port) + "/fixture.aac";

        // 1+2. open -> Continuous mode -> prebuffer -> real audio
        CHECK(ss.open(url));
        CHECK(ss.isOpen());
        CHECK(waitFor(15000, [&] { return ss.isPrebuffered(); }));
        auto pcm = drainFrames(ss, 50 * 512);
        CHECK(rmsOf(pcm) > 0.15);                        // decoded sine, not silence

        // The transport actually asked for metadata.
        {
            std::lock_guard<std::mutex> lk(srv.mtx);
            CHECK(srv.first_request.find("Icy-MetaData: 1") != std::string::npos);
        }

        // 3. THE alignment gate: two title transitions parse exactly.
        CHECK(waitTitle(ss, "Alpha One", 20000));
        srv.setTitle("Beta Two");
        CHECK(waitTitle(ss, "Beta Two", 20000));
        srv.setTitle("Gamma Three");
        CHECK(waitTitle(ss, "Gamma Three", 20000));
        // Position advances only on real (non-silence) frames — drain until a
        // full second has actually been consumed rather than asserting on a
        // platform-speed-dependent snapshot.
        CHECK(drainWaitFor(ss, 20000, [&] { return ss.positionSec() >= 1; }));

        // 4. server drop -> reconnect self-heal (backoff 500ms, fresh request).
        // Must keep draining: the producer only revisits the network (and thus
        // notices the drop) while the ring has room.
        srv.drop.store(true);
        CHECK(drainWaitFor(ss, 15000, [&] { return srv.connections.load() >= 2; }));
        srv.setTitle("Delta Four");
        CHECK(waitTitle(ss, "Delta Four", 25000));       // metadata resumed
        // Titles parse producer-side and can land BEFORE the post-reconnect
        // prebuffer refills — wait for prebuffer before asserting audible audio.
        CHECK(waitFor(15000, [&] { return ss.isPrebuffered(); }));
        auto healed = drainFrames(ss, 20 * 512);
        CHECK(rmsOf(healed) > 0.15);
        std::printf("  reconnect: healed on connection %d\n", srv.connections.load());

        // 5. prompt close on a SILENT stream — drain to underrun first so the
        // producer is genuinely blocked in the network read, not backpressure.
        srv.stall.store(true);
        {
            uint32_t t0 = port::tickMs();
            float buf[512 * 2];
            while ((long)(port::tickMs() - t0) < 20000 && !ss.buffering())
                ss.readFrames(buf, 512);                 // drain flat out, no pacing
            CHECK(ss.buffering());                       // underrun reached
        }
        port::sleepMs(300);                              // producer now at the blocked read
        uint32_t t0 = port::tickMs();
        ss.close();
        uint32_t dt = port::tickMs() - t0;
        std::printf("  close() on a silent stream: %u ms\n", dt);
#ifdef _WIN32
        CHECK(dt < 12000);   // receive-timeout class (8s) — the baseline bound
#else
        CHECK(dt < 2000);    // stop_-poll class (~100ms slices) — the twin bound
#endif
        CHECK(!ss.isOpen());

        srv.stop();
    }

    // ── Scenario B: no icy-metaint -> passthrough (no de-interleave) ──────────
    {
        IcyServer srv;
        srv.audio   = adts;
        srv.status  = "HTTP/1.0 200 OK";                 // the other accepted status shape
        srv.metaint = 0;
        CHECK(srv.start());

        StreamSource ss(core::http());   // slice c: HTTP injected (unused on the raw ICY path)
        std::string url = "http://127.0.0.1:" + std::to_string(srv.port) + "/fixture.aac";
        CHECK(ss.open(url));
        CHECK(waitFor(15000, [&] { return ss.isPrebuffered(); }));
        auto pcm = drainFrames(ss, 30 * 512);
        CHECK(rmsOf(pcm) > 0.15);
        CHECK(ss.nowPlaying().empty());                  // no metadata channel at all
        ss.close();
        srv.stop();
    }

    // ── Scenario C: the "ICY 200 OK" status shape (true SHOUTcast v1) ─────────
    // Pins the per-platform reality (the baseline finding above): Windows/
    // WinINet accepts the stream but yields icy_metaint=0 — audio plays via
    // ADTS resync, titles never parse (byte-verbatim sacred, unchanged). The
    // Linux twin parses the ICY status + metaint itself — a NAMED
    // accepted-better delta.
    {
        IcyServer srv;
        srv.audio  = adts;
        srv.status = "ICY 200 OK";
        srv.setTitle("Icy Epsilon");
        CHECK(srv.start());

        StreamSource ss(core::http());   // slice c: HTTP injected (unused on the raw ICY path)
        std::string url = "http://127.0.0.1:" + std::to_string(srv.port) + "/fixture.aac";
        CHECK(ss.open(url));
        CHECK(waitFor(15000, [&] { return ss.isPrebuffered(); }));
        auto pcm = drainFrames(ss, 30 * 512);
        CHECK(rmsOf(pcm) > 0.10);                        // audio flows on both platforms
#ifdef _WIN32
        // Baseline reality: metaint unseen -> passthrough, metadata swallowed
        // by ADTS resync; titles never appear.
        CHECK(ss.nowPlaying().empty());
#else
        CHECK(waitTitle(ss, "Icy Epsilon", 20000));      // the twin parses ICY status
#endif
        ss.close();
        srv.stop();
    }

    cleanupNet();
    if (g_fail == 0) { std::printf("icy_pipeline_test: ALL PASS\n"); return 0; }
    std::printf("icy_pipeline_test: %d FAILURE(S)\n", g_fail);
    return 1;
}
