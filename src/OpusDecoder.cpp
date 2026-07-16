// OpusDecoder.cpp — miniaudio custom decoding backend backed by libopusfile.
// See OpusDecoder.h for the contract. Decodes Ogg Opus (.opus). Output is
// interleaved signed-16 PCM at 48 kHz.
//
// Design: the whole encoded file is slurped into memory on init (the
// AacDecoder.cpp pattern — songs are a few MB; this removes a class of
// streaming-I/O bugs) and handed to op_open_memory, so libopusfile never sees
// a file path and Windows wide paths need nothing beyond the slurp itself.
// libopusfile owns demux, seek, and decode; op_pcm_seek is sample-exact, so
// there is no AacDecoder-style sample-table machinery here.
//
// The include below MUST keep the explicit opus/ prefix: TagLib ships its own
// (unrelated C++) opusfile.h and its include directory is on this target's
// include path — a bare <opusfile.h> can resolve to the wrong header.

#include "OpusDecoder.h"

#include <opus/opusfile.h>

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

// ── the backend data source ──────────────────────────────────────────────────
struct OpusBackend {
    ma_data_source_base ds;        // MUST be first

    std::vector<uint8_t> file;     // op_open_memory reads from this in place —
                                   // it must outlive `of` and never reallocate
    OggOpusFile* of = nullptr;

    uint32_t channels = 0;
    uint64_t cursor_frames = 0;
};

// ── ma_data_source vtable ─────────────────────────────────────────────────────
ma_result opus_ds_read(ma_data_source* pDS, void* out, ma_uint64 frameCount, ma_uint64* pRead) {
    OpusBackend* b = (OpusBackend*)pDS;
    opus_int16* dst = (opus_int16*)out;
    ma_uint64 served = 0;
    while (served < frameCount) {
        // op_read's buffer size is a value count (samples across channels) in an
        // int; it copies out of its internal packet buffer, so small caps are fine.
        int cap = (int)std::min<ma_uint64>((frameCount - served) * b->channels, 1 << 20);
        int got = op_read(b->of, dst, cap, nullptr);
        if (got == OP_HOLE) continue;          // corrupt page: skip it; opusfile resyncs
        if (got <= 0) break;                   // 0 = EOF, negative = hard error
        dst    += (size_t)got * b->channels;
        served += (ma_uint64)got;
    }
    b->cursor_frames += served;
    if (pRead) *pRead = served;
    return (served > 0) ? MA_SUCCESS : MA_AT_END;
}

ma_result opus_ds_seek(ma_data_source* pDS, ma_uint64 frameIndex) {
    OpusBackend* b = (OpusBackend*)pDS;
    if (op_pcm_seek(b->of, (ogg_int64_t)frameIndex) != 0) return MA_ERROR;
    b->cursor_frames = frameIndex;
    return MA_SUCCESS;
}

ma_result opus_ds_get_format(ma_data_source* pDS, ma_format* fmt, ma_uint32* ch,
                             ma_uint32* sr, ma_channel* chMap, size_t chMapCap) {
    OpusBackend* b = (OpusBackend*)pDS;
    if (fmt) *fmt = ma_format_s16;
    if (ch)  *ch  = b->channels ? b->channels : 2;
    if (sr)  *sr  = 48000;                     // Opus is always 48 kHz internally
    if (chMap && chMapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, chMap, chMapCap, b->channels ? b->channels : 2);
    return MA_SUCCESS;
}

ma_result opus_ds_get_cursor(ma_data_source* pDS, ma_uint64* pCursor) {
    OpusBackend* b = (OpusBackend*)pDS;
    if (pCursor) *pCursor = b->cursor_frames;
    return MA_SUCCESS;
}

ma_result opus_ds_get_length(ma_data_source* pDS, ma_uint64* pLength) {
    OpusBackend* b = (OpusBackend*)pDS;
    ogg_int64_t total = op_pcm_total(b->of, -1);
    if (total < 0) return MA_NOT_IMPLEMENTED;  // unseekable/undetermined stream
    if (pLength) *pLength = (ma_uint64)total;
    return MA_SUCCESS;
}

ma_data_source_vtable g_opus_ds_vtable = {
    opus_ds_read, opus_ds_seek, opus_ds_get_format, opus_ds_get_cursor, opus_ds_get_length, nullptr, 0
};

// ── sniff + construct ─────────────────────────────────────────────────────────
bool looksOgg(const uint8_t* d, size_t n) { return n >= 4 && std::memcmp(d, "OggS", 4) == 0; }

ma_result opus_construct(OpusBackend* b) {
    ma_data_source_config dsc = ma_data_source_config_init();
    dsc.vtable = &g_opus_ds_vtable;
    ma_result r = ma_data_source_init(&dsc, &b->ds);
    if (r != MA_SUCCESS) return r;

    if (!looksOgg(b->file.data(), b->file.size())) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }

    // libopusfile's own parser is the Opus-vs-Vorbis discriminator: a Vorbis
    // (or FLAC/Theora/...) Ogg fails here with OP_ENOTFORMAT → MA_NO_BACKEND.
    int err = 0;
    b->of = op_open_memory(b->file.data(), b->file.size(), &err);
    if (!b->of) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }

    int ch = op_channel_count(b->of, -1);
    b->channels = (ch > 0) ? (uint32_t)ch : 2;
    return MA_SUCCESS;
}

#ifdef _WIN32
bool slurpW(const wchar_t* path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    unsigned char hdr[4]; size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (!looksOgg(hdr, got)) { fclose(f); return false; }
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
    if (!looksOgg(hdr, got)) { fclose(f); return false; }
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
    OpusBackend* b = new (std::nothrow) OpusBackend();
    if (!b) return MA_OUT_OF_MEMORY;
    b->file.assign((const uint8_t*)pData, (const uint8_t*)pData + dataSize);
    ma_result r = opus_construct(b);
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
    if (!looksOgg(buf.data(), buf.size())) return MA_NO_BACKEND;
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
    OpusBackend* b = (OpusBackend*)pBackend;
    if (b->of) op_free(b->of);
    ma_data_source_uninit(&b->ds);
    delete b;
}

ma_decoding_backend_vtable g_opus_backend_vtable = {
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

extern "C" ma_decoding_backend_vtable* ma_opus_backend_vtable(void) {
    return &g_opus_backend_vtable;
}
