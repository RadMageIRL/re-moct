// AacDecoder.cpp — miniaudio custom decoding backend backed by FDK-AAC.
// See AacDecoder.h for the contract. Decodes raw ADTS (.aac) and AAC inside an
// MP4/ISO-BMFF container (.m4a/.mp4). Output is interleaved signed-16 PCM.
//
// Design: the whole encoded file is slurped into memory on init (songs are a few
// MB; this removes a class of streaming-I/O bugs). Both transports are reduced to
// a flat "sample table" (file offset + size per access unit): for MP4 from the box
// tree, for ADTS by walking frame headers. FDK is then fed one access unit per
// frame from that table, which makes seeking exact and identical for both paths.

#include "AacDecoder.h"

#include <fdk-aac/aacdecoder_lib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// 64-bit file seek/tell so files >2 GB are sized correctly rather than rejected
// by a 32-bit ftell overflow. Same convention as Mp4Chapters.cpp.
#ifdef _WIN32
#  define FSEEK64 _fseeki64
#  define FTELL64 _ftelli64
#else
#  define FSEEK64 fseeko
#  define FTELL64 ftello
#endif

namespace {

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint64_t be64(const uint8_t* p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

struct AacSample { uint64_t offset; uint32_t size; };

// ── minimal MP4/ISO-BMFF parser (first AAC audio track) ──────────────────────
struct Mp4Aac {
    std::vector<uint8_t>  asc;         // AudioSpecificConfig (DecoderSpecificInfo)
    std::vector<AacSample> samples;    // flat per-access-unit table
    uint32_t timescale = 0;
    uint64_t duration  = 0;            // in timescale units
    bool ok = false;

    static uint32_t descLen(const uint8_t*& p, const uint8_t* end) {
        uint32_t len = 0; int cnt = 0;
        while (p < end && cnt < 4) { uint8_t b = *p++; len = (len << 7) | (b & 0x7F); ++cnt; if (!(b & 0x80)) break; }
        return len;
    }

    void parseEsds(const uint8_t* p, const uint8_t* end) {
        if (p + 4 > end) return;
        p += 4;                                  // FullBox version+flags
        if (p >= end || *p != 0x03) return;      // ES_DescrTag
        ++p; descLen(p, end);
        if (p + 3 > end) return;
        p += 2;                                  // ES_ID
        uint8_t flags = *p++;
        if (flags & 0x80) p += 2;
        if (flags & 0x40) { if (p < end) { uint8_t l = *p++; p += l; } }
        if (flags & 0x20) p += 2;
        if (p >= end || *p != 0x04) return;      // DecoderConfigDescrTag
        ++p; descLen(p, end);
        p += 13;
        if (p >= end || *p != 0x05) return;      // DecSpecificInfoTag
        ++p; uint32_t n = descLen(p, end);
        if (p + n > end) n = (uint32_t)(end - p);
        asc.assign(p, p + n);
    }

    static bool findBox(const uint8_t* p, const uint8_t* end, const char* type,
                        const uint8_t** outBeg, const uint8_t** outEnd) {
        while (p + 8 <= end) {
            uint64_t sz = be32(p);
            const uint8_t* hdr = p;
            const uint8_t* body = p + 8;
            if (sz == 1) { if (p + 16 > end) break; sz = be64(p + 8); body = p + 16; }
            else if (sz == 0) sz = (uint64_t)(end - p);
            if (sz < (uint64_t)(body - hdr) || sz > (uint64_t)(end - hdr)) break;  // size-compare avoids hdr+sz pointer overflow on crafted 64-bit boxes
            if (std::memcmp(hdr + 4, type, 4) == 0) { *outBeg = body; *outEnd = hdr + sz; return true; }
            p = hdr + sz;
        }
        return false;
    }

    void parseStbl(const uint8_t* b, const uint8_t* e) {
        const uint8_t *sb, *se;
        if (findBox(b, e, "stsd", &sb, &se)) {
            const uint8_t *mb, *me;
            if (sb + 8 <= se && findBox(sb + 8, se, "mp4a", &mb, &me)) {
                const uint8_t *eb, *ee;
                if (mb + 28 <= me && findBox(mb + 28, me, "esds", &eb, &ee)) parseEsds(eb, ee);
            }
        }

        std::vector<uint32_t> sizes;
        if (findBox(b, e, "stsz", &sb, &se) && sb + 12 <= se) {
            uint32_t uniform = be32(sb + 4);
            uint32_t count   = be32(sb + 8);
            if (uniform != 0) sizes.assign(count, uniform);
            else { const uint8_t* q = sb + 12; for (uint32_t i = 0; i < count && q + 4 <= se; ++i, q += 4) sizes.push_back(be32(q)); }
        }

        struct Stsc { uint32_t first, perChunk; };
        std::vector<Stsc> stsc;
        if (findBox(b, e, "stsc", &sb, &se) && sb + 8 <= se) {
            uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8;
            for (uint32_t i = 0; i < n && q + 12 <= se; ++i, q += 12) stsc.push_back({ be32(q), be32(q + 4) });
        }

        std::vector<uint64_t> chunks;
        if (findBox(b, e, "stco", &sb, &se) && sb + 8 <= se) {
            uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8;
            for (uint32_t i = 0; i < n && q + 4 <= se; ++i, q += 4) chunks.push_back(be32(q));
        } else if (findBox(b, e, "co64", &sb, &se) && sb + 8 <= se) {
            uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8;
            for (uint32_t i = 0; i < n && q + 8 <= se; ++i, q += 8) chunks.push_back(be64(q));
        }

        if (findBox(b, e, "stts", &sb, &se) && sb + 8 <= se) {
            uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8; uint64_t total = 0;
            for (uint32_t i = 0; i < n && q + 8 <= se; ++i, q += 8) total += (uint64_t)be32(q) * be32(q + 4);
            if (duration == 0) duration = total;
        }

        if (sizes.empty() || stsc.empty() || chunks.empty()) return;
        uint32_t sIdx = 0;
        for (uint32_t c = 0; c < chunks.size(); ++c) {
            uint32_t per = stsc.back().perChunk;
            for (size_t k = 0; k < stsc.size(); ++k) {
                uint32_t firstNext = (k + 1 < stsc.size()) ? stsc[k + 1].first : 0xFFFFFFFFu;
                if ((c + 1) >= stsc[k].first && (c + 1) < firstNext) { per = stsc[k].perChunk; break; }
            }
            uint64_t off = chunks[c];
            for (uint32_t s = 0; s < per && sIdx < sizes.size(); ++s, ++sIdx) {
                samples.push_back({ off, sizes[sIdx] });
                off += sizes[sIdx];
            }
        }
        if (!samples.empty() && !asc.empty()) ok = true;
    }

    bool parse(const uint8_t* data, size_t len) {
        const uint8_t* end = data + len;
        const uint8_t *mb, *me;
        if (!findBox(data, end, "moov", &mb, &me)) return false;
        const uint8_t* p = mb;
        const uint8_t *tb, *te;
        while (findBox(p, me, "trak", &tb, &te)) {
            const uint8_t *mdb, *mde;
            if (findBox(tb, te, "mdia", &mdb, &mde)) {
                const uint8_t *hb, *he;
                if (findBox(mdb, mde, "mdhd", &hb, &he) && hb + 4 <= he) {
                    uint8_t ver = hb[0];
                    if (ver == 1 && hb + 28 <= he) { timescale = be32(hb + 20); duration = be64(hb + 24); }
                    else if (hb + 20 <= he)        { timescale = be32(hb + 12); duration = be32(hb + 16); }
                }
                const uint8_t *nb, *ne;
                if (findBox(mdb, mde, "minf", &nb, &ne)) {
                    const uint8_t *xb, *xe;
                    if (findBox(nb, ne, "stbl", &xb, &xe)) parseStbl(xb, xe);
                }
            }
            if (ok) break;
            p = te;
        }
        return ok;
    }
};

// Walk ADTS frame headers, building a sample table (each frame carries its length).
void buildAdts(const std::vector<uint8_t>& f, std::vector<AacSample>& out) {
    size_t i = 0;
    while (i + 7 <= f.size()) {
        if (!(f[i] == 0xFF && (f[i + 1] & 0xF6) == 0xF0)) { ++i; continue; }   // resync
        uint32_t len = (((uint32_t)(f[i + 3] & 0x03)) << 11) | ((uint32_t)f[i + 4] << 3) | (f[i + 5] >> 5);
        if (len < 7 || i + len > f.size()) break;
        out.push_back({ (uint64_t)i, len });
        i += len;
    }
}

// ── the backend data source ──────────────────────────────────────────────────
enum class AacMode { Adts, Mp4 };

struct AacBackend {
    ma_data_source_base ds;            // MUST be first

    std::vector<uint8_t>   file;
    std::vector<AacSample> samples;    // shared by both modes
    std::vector<uint8_t>   asc;        // MP4 only
    AacMode mode = AacMode::Adts;

    HANDLE_AACDECODER dec = nullptr;
    uint32_t mp4_timescale = 0;
    uint64_t mp4_duration  = 0;

    size_t cur_sample = 0;
    std::vector<int16_t> pcm;
    size_t pcm_pos = 0;

    uint32_t channels = 0, rate = 0, frame_size = 0;
    uint64_t cursor_frames = 0;
};

bool openFdk(AacBackend* b) {
    if (b->mode == AacMode::Mp4) {
        b->dec = aacDecoder_Open(TT_MP4_RAW, 1);
        if (!b->dec) return false;
        UCHAR* asc = b->asc.data();
        UINT   ascLen = (UINT)b->asc.size();
        if (aacDecoder_ConfigRaw(b->dec, &asc, &ascLen) != AAC_DEC_OK) { aacDecoder_Close(b->dec); b->dec = nullptr; return false; }
    } else {
        b->dec = aacDecoder_Open(TT_MP4_ADTS, 1);
        if (!b->dec) return false;
    }
    return true;
}

bool decodeNext(AacBackend* b) {
    static constexpr int OUT_MAX = 8 * 2048 * 2;
    INT_PCM out[OUT_MAX];
    while (b->cur_sample < b->samples.size()) {
        const AacSample& s = b->samples[b->cur_sample++];
        if (s.offset > b->file.size() || s.size > b->file.size() - s.offset) return false;  // overflow-safe bounds check
        UCHAR* inBuf = b->file.data() + s.offset;
        UINT   inSize = s.size, valid = s.size;
        aacDecoder_Fill(b->dec, &inBuf, &inSize, &valid);
        AAC_DECODER_ERROR e = aacDecoder_DecodeFrame(b->dec, out, OUT_MAX, 0);
        if (e != AAC_DEC_OK) continue;                       // skip a bad AU; FDK resyncs
        CStreamInfo* si = aacDecoder_GetStreamInfo(b->dec);
        if (!si || si->frameSize <= 0 || si->numChannels <= 0) continue;
        b->channels = (uint32_t)si->numChannels;
        b->rate     = (uint32_t)si->sampleRate;
        b->frame_size = (uint32_t)si->frameSize;
        size_t n = (size_t)si->frameSize * si->numChannels;
        b->pcm.assign(out, out + n);
        b->pcm_pos = 0;
        return true;
    }
    return false;
}

bool reopenFdk(AacBackend* b) {
    if (b->dec) { aacDecoder_Close(b->dec); b->dec = nullptr; }
    b->pcm.clear(); b->pcm_pos = 0;
    return openFdk(b);
}

// ── ma_data_source vtable ─────────────────────────────────────────────────────
ma_result aac_ds_read(ma_data_source* pDS, void* out, ma_uint64 frameCount, ma_uint64* pRead) {
    AacBackend* b = (AacBackend*)pDS;
    int16_t* dst = (int16_t*)out;
    ma_uint64 served = 0;
    while (served < frameCount) {
        if (b->pcm_pos >= b->pcm.size()) { if (!decodeNext(b)) break; }
        if (b->channels == 0) break;
        ma_uint64 availFrames = (b->pcm.size() - b->pcm_pos) / b->channels;
        ma_uint64 want = frameCount - served;
        if (want > availFrames) want = availFrames;
        std::memcpy(dst, &b->pcm[b->pcm_pos], (size_t)want * b->channels * sizeof(int16_t));
        b->pcm_pos += (size_t)want * b->channels;
        dst        += want * b->channels;
        served     += want;
    }
    b->cursor_frames += served;
    if (pRead) *pRead = served;
    return (served > 0) ? MA_SUCCESS : MA_AT_END;
}

ma_result aac_ds_seek(ma_data_source* pDS, ma_uint64 frameIndex) {
    AacBackend* b = (AacBackend*)pDS;
    if (!reopenFdk(b)) return MA_ERROR;
    uint32_t fs = b->frame_size ? b->frame_size : 1024;
    size_t idx = (size_t)(frameIndex / fs);
    if (idx > b->samples.size()) idx = b->samples.size();
    b->cur_sample    = idx;
    b->cursor_frames = (uint64_t)idx * fs;
    return MA_SUCCESS;
}

ma_result aac_ds_get_format(ma_data_source* pDS, ma_format* fmt, ma_uint32* ch,
                            ma_uint32* sr, ma_channel* chMap, size_t chMapCap) {
    AacBackend* b = (AacBackend*)pDS;
    if (fmt) *fmt = ma_format_s16;
    if (ch)  *ch  = b->channels ? b->channels : 2;
    if (sr)  *sr  = b->rate ? b->rate : 44100;
    if (chMap && chMapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, chMap, chMapCap, b->channels ? b->channels : 2);
    return MA_SUCCESS;
}

ma_result aac_ds_get_cursor(ma_data_source* pDS, ma_uint64* pCursor) {
    AacBackend* b = (AacBackend*)pDS;
    if (pCursor) *pCursor = b->cursor_frames;
    return MA_SUCCESS;
}

ma_result aac_ds_get_length(ma_data_source* pDS, ma_uint64* pLength) {
    AacBackend* b = (AacBackend*)pDS;
    if (b->mode == AacMode::Mp4 && b->mp4_timescale && b->mp4_duration && b->rate) {
        double sec = (double)b->mp4_duration / (double)b->mp4_timescale;
        if (pLength) *pLength = (ma_uint64)(sec * b->rate + 0.5);
        return MA_SUCCESS;
    }
    uint32_t fs = b->frame_size ? b->frame_size : 1024;
    if (!b->samples.empty()) { if (pLength) *pLength = (ma_uint64)b->samples.size() * fs; return MA_SUCCESS; }
    return MA_NOT_IMPLEMENTED;
}

ma_data_source_vtable g_aac_ds_vtable = {
    aac_ds_read, aac_ds_seek, aac_ds_get_format, aac_ds_get_cursor, aac_ds_get_length, nullptr, 0
};

// ── sniff + construct ─────────────────────────────────────────────────────────
bool looksAdts(const uint8_t* d, size_t n) { return n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0; }
bool looksMp4 (const uint8_t* d, size_t n) { return n >= 12 && std::memcmp(d + 4, "ftyp", 4) == 0; }

ma_result aac_construct(AacBackend* b) {
    ma_data_source_config dsc = ma_data_source_config_init();
    dsc.vtable = &g_aac_ds_vtable;
    ma_result r = ma_data_source_init(&dsc, &b->ds);
    if (r != MA_SUCCESS) return r;

    if (looksMp4(b->file.data(), b->file.size())) {
        Mp4Aac mp4;
        if (!mp4.parse(b->file.data(), b->file.size())) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }
        b->mode = AacMode::Mp4;
        b->samples       = std::move(mp4.samples);
        b->asc           = std::move(mp4.asc);
        b->mp4_timescale = mp4.timescale;
        b->mp4_duration  = mp4.duration;
    } else if (looksAdts(b->file.data(), b->file.size())) {
        b->mode = AacMode::Adts;
        buildAdts(b->file, b->samples);
        if (b->samples.empty()) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }
    } else {
        ma_data_source_uninit(&b->ds); return MA_NO_BACKEND;
    }

    if (!openFdk(b)) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }
    if (!decodeNext(b)) { if (b->dec) aacDecoder_Close(b->dec); b->dec = nullptr; ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }
    return MA_SUCCESS;   // first frame primed in b->pcm with pcm_pos = 0
}

#ifdef _WIN32
bool slurpW(const wchar_t* path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    unsigned char hdr[12]; size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (!looksAdts(hdr, got) && !looksMp4(hdr, got)) { fclose(f); return false; }
    FSEEK64(f, 0, SEEK_END); long long sz = FTELL64(f); FSEEK64(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t rd = fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return rd == (size_t)sz;
}
#endif
bool slurpA(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    unsigned char hdr[12]; size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (!looksAdts(hdr, got) && !looksMp4(hdr, got)) { fclose(f); return false; }
    FSEEK64(f, 0, SEEK_END); long long sz = FTELL64(f); FSEEK64(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t rd = fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return rd == (size_t)sz;
}

// ── ma_decoding_backend vtable ────────────────────────────────────────────────
ma_result be_init_memory(void* pUserData, const void* pData, size_t dataSize,
                         const ma_decoding_backend_config* pConfig,
                         const ma_allocation_callbacks* pAlloc, ma_data_source** ppBackend) {
    (void)pUserData; (void)pConfig; (void)pAlloc;
    AacBackend* b = new (std::nothrow) AacBackend();
    if (!b) return MA_OUT_OF_MEMORY;
    b->file.assign((const uint8_t*)pData, (const uint8_t*)pData + dataSize);
    ma_result r = aac_construct(b);
    if (r != MA_SUCCESS) { delete b; return r; }
    *ppBackend = &b->ds;
    return MA_SUCCESS;
}

ma_result be_init(void* pUserData, ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell,
                  void* pRST, const ma_decoding_backend_config* pConfig,
                  const ma_allocation_callbacks* pAlloc, ma_data_source** ppBackend) {
    (void)onTell;
    std::vector<uint8_t> buf;
    unsigned char tmp[16384];
    if (onSeek) onSeek(pRST, 0, ma_seek_origin_start);
    for (;;) {
        size_t got = 0;
        ma_result r = onRead(pRST, tmp, sizeof(tmp), &got);
        if (got > 0) buf.insert(buf.end(), tmp, tmp + got);
        if (r != MA_SUCCESS || got == 0) break;
    }
    if (!looksAdts(buf.data(), buf.size()) && !looksMp4(buf.data(), buf.size())) return MA_NO_BACKEND;
    return be_init_memory(pUserData, buf.data(), buf.size(), pConfig, pAlloc, ppBackend);
}

ma_result be_init_file(void* pUserData, const char* pFilePath,
                       const ma_decoding_backend_config* pConfig,
                       const ma_allocation_callbacks* pAlloc, ma_data_source** ppBackend) {
    std::vector<uint8_t> buf;
    if (!slurpA(pFilePath, buf)) return MA_NO_BACKEND;
    return be_init_memory(pUserData, buf.data(), buf.size(), pConfig, pAlloc, ppBackend);
}

#ifdef _WIN32
ma_result be_init_file_w(void* pUserData, const wchar_t* pFilePath,
                         const ma_decoding_backend_config* pConfig,
                         const ma_allocation_callbacks* pAlloc, ma_data_source** ppBackend) {
    std::vector<uint8_t> buf;
    if (!slurpW(pFilePath, buf)) return MA_NO_BACKEND;
    return be_init_memory(pUserData, buf.data(), buf.size(), pConfig, pAlloc, ppBackend);
}
#endif

void be_uninit(void* pUserData, ma_data_source* pBackend, const ma_allocation_callbacks* pAlloc) {
    (void)pUserData; (void)pAlloc;
    if (!pBackend) return;
    AacBackend* b = (AacBackend*)pBackend;
    if (b->dec) aacDecoder_Close(b->dec);
    ma_data_source_uninit(&b->ds);
    delete b;
}

ma_decoding_backend_vtable g_aac_backend_vtable = {
    be_init,
    be_init_file,
#ifdef _WIN32
    be_init_file_w,
#else
    nullptr,
#endif
    be_init_memory,
    be_uninit
};

} // namespace

extern "C" ma_decoding_backend_vtable* ma_aac_backend_vtable(void) {
    return &g_aac_backend_vtable;
}
