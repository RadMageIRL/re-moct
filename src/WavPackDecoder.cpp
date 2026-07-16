// WavPackDecoder.cpp — miniaudio custom decoding backend backed by libwavpack.
// See WavPackDecoder.h for the contract. Decodes plain .wv (16/24/32-bit int
// and float, lossless or hybrid-lossy without the .wvc sidecar). Output is
// interleaved 32-bit float PCM at the file's native rate.
//
// Design: the whole encoded file is slurped into memory on init (the
// AacDecoder.cpp pattern) and served to libwavpack through a
// WavpackStreamReader64 over that buffer — so libwavpack never sees a file
// path (Windows wide paths need nothing beyond the slurp), and passing a null
// wvc_id defers .wvc correction files structurally: a memory stream has no
// filename to derive the sidecar from. libwavpack owns block parsing, seek
// (WavpackSeekSample64 is sample-exact), and unpacking.
//
// WavpackUnpackSamples always yields int32 slots whatever the source depth:
// integer samples are left-justified to their native bit depth (scaled here by
// 1/2^(bps-1)), and MODE_FLOAT files carry IEEE float bit patterns in the same
// slots (OPEN_NORMALIZE pins them to ±1.0), copied out bit-for-bit.

#include "WavPackDecoder.h"

#include <wavpack/wavpack.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

// 64-bit file seek/tell so files >2 GB are sized correctly rather than rejected
// by a 32-bit ftell overflow. Same convention as AacDecoder.cpp/Mp4Chapters.cpp.
#ifdef _WIN32
#  define FSEEK64 _fseeki64
#  define FTELL64 _ftelli64
#else
#  define FSEEK64 fseeko
#  define FTELL64 ftello
#endif

namespace {

// ── WavpackStreamReader64 over an in-memory buffer ────────────────────────────
struct MemStream {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos  = 0;       // may run past size (fseek-past-EOF semantics); reads then return 0
};

int32_t ws_read(void* id, void* dst, int32_t bcount) {
    MemStream* m = (MemStream*)id;
    if (bcount <= 0 || m->pos >= m->size) return 0;
    size_t n = std::min((size_t)bcount, m->size - m->pos);
    std::memcpy(dst, m->data + m->pos, n);
    m->pos += n;
    return (int32_t)n;
}
int32_t ws_write(void* id, void* dst, int32_t bcount) {   // decode-only: never edits tags
    (void)id; (void)dst; (void)bcount;
    return 0;
}
int64_t ws_get_pos(void* id) { return (int64_t)((MemStream*)id)->pos; }
int ws_set_pos_abs(void* id, int64_t pos) {
    if (pos < 0) return -1;
    ((MemStream*)id)->pos = (size_t)pos;
    return 0;
}
int ws_set_pos_rel(void* id, int64_t delta, int mode) {
    MemStream* m = (MemStream*)id;
    int64_t base = (mode == SEEK_CUR) ? (int64_t)m->pos
                 : (mode == SEEK_END) ? (int64_t)m->size : 0;
    int64_t np = base + delta;
    if (np < 0) return -1;
    m->pos = (size_t)np;
    return 0;
}
int ws_push_back_byte(void* id, int c) {
    MemStream* m = (MemStream*)id;
    if (m->pos == 0) return EOF;
    --m->pos;              // the buffer IS the file, so the byte is already there
    return c;
}
int64_t ws_get_length(void* id) { return (int64_t)((MemStream*)id)->size; }
int ws_can_seek(void* id) { (void)id; return 1; }
int ws_truncate_here(void* id) { (void)id; return -1; }   // decode-only
int ws_close(void* id) { (void)id; return 0; }            // buffer owned by the backend

WavpackStreamReader64 g_mem_reader = {
    ws_read, ws_write, ws_get_pos, ws_set_pos_abs, ws_set_pos_rel,
    ws_push_back_byte, ws_get_length, ws_can_seek, ws_truncate_here, ws_close
};

// ── the backend data source ──────────────────────────────────────────────────
struct WvBackend {
    ma_data_source_base ds;        // MUST be first

    std::vector<uint8_t> file;
    MemStream mem;
    WavpackContext* ctx = nullptr;

    uint32_t channels = 0, rate = 0;
    bool  is_float = false;
    float scale = 1.0f / 32768.0f;   // int → f32 factor; 1.0 (unused) for float mode

    std::vector<int32_t> tmp;        // WavpackUnpackSamples staging
    uint64_t cursor_frames = 0;
};

// ── ma_data_source vtable ─────────────────────────────────────────────────────
ma_result wv_ds_read(ma_data_source* pDS, void* out, ma_uint64 frameCount, ma_uint64* pRead) {
    WvBackend* b = (WvBackend*)pDS;
    float* dst = (float*)out;
    ma_uint64 served = 0;
    while (served < frameCount) {
        uint32_t want = (uint32_t)std::min<ma_uint64>(frameCount - served, 4096);
        b->tmp.resize((size_t)want * b->channels);
        uint32_t got = WavpackUnpackSamples(b->ctx, b->tmp.data(), want);
        if (got == 0) break;                             // EOF or decode error
        size_t n = (size_t)got * b->channels;
        if (b->is_float) {
            std::memcpy(dst, b->tmp.data(), n * sizeof(float));
        } else {
            for (size_t i = 0; i < n; ++i)
                dst[i] = (float)b->tmp[i] * b->scale;
        }
        dst    += n;
        served += got;
    }
    b->cursor_frames += served;
    if (pRead) *pRead = served;
    return (served > 0) ? MA_SUCCESS : MA_AT_END;
}

ma_result wv_ds_seek(ma_data_source* pDS, ma_uint64 frameIndex) {
    WvBackend* b = (WvBackend*)pDS;
    if (!WavpackSeekSample64(b->ctx, (int64_t)frameIndex)) return MA_ERROR;
    b->cursor_frames = frameIndex;
    return MA_SUCCESS;
}

ma_result wv_ds_get_format(ma_data_source* pDS, ma_format* fmt, ma_uint32* ch,
                           ma_uint32* sr, ma_channel* chMap, size_t chMapCap) {
    WvBackend* b = (WvBackend*)pDS;
    if (fmt) *fmt = ma_format_f32;
    if (ch)  *ch  = b->channels ? b->channels : 2;
    if (sr)  *sr  = b->rate ? b->rate : 44100;
    if (chMap && chMapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, chMap, chMapCap, b->channels ? b->channels : 2);
    return MA_SUCCESS;
}

ma_result wv_ds_get_cursor(ma_data_source* pDS, ma_uint64* pCursor) {
    WvBackend* b = (WvBackend*)pDS;
    if (pCursor) *pCursor = b->cursor_frames;
    return MA_SUCCESS;
}

ma_result wv_ds_get_length(ma_data_source* pDS, ma_uint64* pLength) {
    WvBackend* b = (WvBackend*)pDS;
    int64_t total = WavpackGetNumSamples64(b->ctx);
    if (total < 0) return MA_NOT_IMPLEMENTED;            // unknown length
    if (pLength) *pLength = (ma_uint64)total;
    return MA_SUCCESS;
}

ma_data_source_vtable g_wv_ds_vtable = {
    wv_ds_read, wv_ds_seek, wv_ds_get_format, wv_ds_get_cursor, wv_ds_get_length, nullptr, 0
};

// ── sniff + construct ─────────────────────────────────────────────────────────
bool looksWv(const uint8_t* d, size_t n) { return n >= 4 && std::memcmp(d, "wvpk", 4) == 0; }

ma_result wv_construct(WvBackend* b) {
    ma_data_source_config dsc = ma_data_source_config_init();
    dsc.vtable = &g_wv_ds_vtable;
    ma_result r = ma_data_source_init(&dsc, &b->ds);
    if (r != MA_SUCCESS) return r;

    if (!looksWv(b->file.data(), b->file.size())) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }

    b->mem = { b->file.data(), b->file.size(), 0 };
    char err[80] = {};
    // wvc_id null = no correction stream (hybrid plays lossy). OPEN_NORMALIZE
    // pins MODE_FLOAT data to ±1.0 so the bit-copy below needs no scaling.
    b->ctx = WavpackOpenFileInputEx64(&g_mem_reader, &b->mem, nullptr, err, OPEN_NORMALIZE, 0);
    if (!b->ctx) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }

    b->channels = (uint32_t)WavpackGetNumChannels(b->ctx);
    b->rate     = WavpackGetSampleRate(b->ctx);
    int bps     = WavpackGetBitsPerSample(b->ctx);
    b->is_float = (WavpackGetMode(b->ctx) & MODE_FLOAT) != 0;
    if (b->channels == 0 || b->rate == 0 || bps < 1 || bps > 32) {
        b->ctx = WavpackCloseFile(b->ctx);
        ma_data_source_uninit(&b->ds);
        return MA_NO_BACKEND;
    }
    b->scale = b->is_float ? 1.0f : 1.0f / (float)(1u << (bps - 1));
    return MA_SUCCESS;
}

#ifdef _WIN32
bool slurpW(const wchar_t* path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    unsigned char hdr[4]; size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (!looksWv(hdr, got)) { fclose(f); return false; }
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
    unsigned char hdr[4]; size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (!looksWv(hdr, got)) { fclose(f); return false; }
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
    WvBackend* b = new (std::nothrow) WvBackend();
    if (!b) return MA_OUT_OF_MEMORY;
    b->file.assign((const uint8_t*)pData, (const uint8_t*)pData + dataSize);
    ma_result r = wv_construct(b);
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
    if (!looksWv(buf.data(), buf.size())) return MA_NO_BACKEND;
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
    WvBackend* b = (WvBackend*)pBackend;
    if (b->ctx) WavpackCloseFile(b->ctx);
    ma_data_source_uninit(&b->ds);
    delete b;
}

ma_decoding_backend_vtable g_wv_backend_vtable = {
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

extern "C" ma_decoding_backend_vtable* ma_wavpack_backend_vtable(void) {
    return &g_wv_backend_vtable;
}
