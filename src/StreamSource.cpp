#ifdef _WIN32

#include "StreamSource.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "wininet.lib")

// ─── Diagnostic log (temp: %TEMP%\remoct_stream.log) ──────────────────────────
static void slog(const char* fmt, ...) {
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string p = (n > 0 && n < MAX_PATH) ? std::string(tmp) + "remoct_stream.log"
                                            : std::string("remoct_stream.log");
    FILE* f = std::fopen(p.c_str(), "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
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
    { std::lock_guard<std::mutex> lk(now_playing_mtx_); now_playing_.clear(); }

    // Codec choice by URL (content-type sniffing is a later refinement).
    std::string lower = url;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    codec_ = (lower.find(".aac") != std::string::npos || lower.find("aac") != std::string::npos)
             ? Codec::AAC : Codec::MP3;

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
    hInet_ = InternetOpenA("RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)",
                           INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet_) return false;

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

// ─── ICY metadata de-interleaving ─────────────────────────────────────────────
// Both decoders pull audio through readAudio(); it transparently removes the
// periodic Shoutcast/Icecast metadata blocks and parses the StreamTitle out.

DWORD StreamSource::rawRead(void* dst, DWORD want) {
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
    return now_playing_;
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

#endif // _WIN32
