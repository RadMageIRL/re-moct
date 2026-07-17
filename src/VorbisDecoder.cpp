// VorbisDecoder.cpp — miniaudio custom decoding backend backed by
// libvorbisfile. See VorbisDecoder.h for the contract. Decodes Ogg Vorbis
// (.ogg). Output is interleaved 32-bit float PCM at the file's native rate.
//
// Design: the whole encoded file is slurped into memory on init (the
// AacDecoder.cpp pattern) and served to libvorbisfile through ov_open_callbacks
// over that buffer — vorbisfile has no op_open_memory equivalent, so the
// fread-shaped callbacks are hand-written. Like op_open_memory, the callbacks
// read lazily for the life of the OggVorbis_File: the slurped buffer is
// pinned in the backend struct, never resized after open, and ov_clear runs
// BEFORE the struct (and buffer) are deleted — a freed-buffer read would be a
// use-after-free that only surfaces on seek or near EOF.
//
// ov_read_float is PLANAR (float** per channel, pointing into decoder-internal
// buffers), unlike op_read_float's interleaved output — the interleave loop
// below is the one place in the codec backends that needs it.
//
// The include keeps the explicit vorbis/ prefix: TagLib ships its own
// (unrelated C++) vorbisfile.h on this target's include path, and the CI
// grep gate forbids the bare form.

#include "VorbisDecoder.h"

#include <vorbis/vorbisfile.h>

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

// ── ov_callbacks over an in-memory buffer ─────────────────────────────────────
struct MemStream {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos  = 0;       // may run past size (fseek-past-EOF semantics); reads then return 0
};

size_t vs_read(void* ptr, size_t size, size_t nmemb, void* ds) {
    MemStream* m = (MemStream*)ds;
    if (size == 0 || nmemb == 0 || m->pos >= m->size) return 0;
    size_t items = std::min(nmemb, (m->size - m->pos) / size);
    if (items == 0) return 0;                     // less than one whole item left
    std::memcpy(ptr, m->data + m->pos, items * size);
    m->pos += items * size;
    return items;
}
int vs_seek(void* ds, ogg_int64_t offset, int whence) {
    MemStream* m = (MemStream*)ds;
    ogg_int64_t base = (whence == SEEK_CUR) ? (ogg_int64_t)m->pos
                     : (whence == SEEK_END) ? (ogg_int64_t)m->size : 0;
    ogg_int64_t np = base + offset;
    if (np < 0) return -1;
    m->pos = (size_t)np;
    return 0;
}
// NOTE: the ov_callbacks tell signature returns long — 32-bit on 64-bit
// Windows, so a >2 GB .ogg would misreport there. A vorbisfile API limit,
// not ours; multi-GB Vorbis files are effectively nonexistent.
long vs_tell(void* ds) { return (long)((MemStream*)ds)->pos; }

const ov_callbacks g_mem_callbacks = {
    vs_read, vs_seek, nullptr /* close: buffer owned by the backend */, vs_tell
};

// ── the backend data source ──────────────────────────────────────────────────
struct VorbisBackend {
    ma_data_source_base ds;        // MUST be first

    std::vector<uint8_t> file;     // the callbacks read from this in place —
                                   // it must outlive vf and never reallocate
    MemStream mem;
    OggVorbis_File vf {};
    bool opened = false;           // ov_clear only on a successful open

    uint32_t channels = 0, rate = 0;
    uint64_t cursor_frames = 0;
};

// ── ma_data_source vtable ─────────────────────────────────────────────────────
ma_result vorbis_ds_read(ma_data_source* pDS, void* out, ma_uint64 frameCount, ma_uint64* pRead) {
    VorbisBackend* b = (VorbisBackend*)pDS;
    float* dst = (float*)out;
    ma_uint64 served = 0;
    int bitstream = 0;
    while (served < frameCount) {
        int want = (int)std::min<ma_uint64>(frameCount - served, 4096);
        float** pcm = nullptr;
        long got = ov_read_float(&b->vf, &pcm, want, &bitstream);
        if (got == OV_HOLE) continue;          // corrupt page: skip it; vorbisfile resyncs
        if (got <= 0 || !pcm) break;           // 0 = EOF, negative = hard error
        // Planar -> interleaved: pcm[c] points into decoder-internal buffers
        // that are only valid until the next ov_read_float call. Chained
        // streams may switch channel count per link — clamp to the CURRENT
        // link's count so pcm[] is never indexed past its planar array.
        vorbis_info* vi = ov_info(&b->vf, bitstream);
        uint32_t src_ch = (vi && vi->channels > 0) ? (uint32_t)vi->channels : b->channels;
        for (long i = 0; i < got; ++i)
            for (uint32_t c = 0; c < b->channels; ++c)
                dst[((size_t)i * b->channels) + c] = pcm[std::min(c, src_ch - 1)][i];
        dst    += (size_t)got * b->channels;
        served += (ma_uint64)got;
    }
    b->cursor_frames += served;
    if (pRead) *pRead = served;
    return (served > 0) ? MA_SUCCESS : MA_AT_END;
}

ma_result vorbis_ds_seek(ma_data_source* pDS, ma_uint64 frameIndex) {
    VorbisBackend* b = (VorbisBackend*)pDS;
    if (ov_pcm_seek(&b->vf, (ogg_int64_t)frameIndex) != 0) return MA_ERROR;
    b->cursor_frames = frameIndex;
    return MA_SUCCESS;
}

ma_result vorbis_ds_get_format(ma_data_source* pDS, ma_format* fmt, ma_uint32* ch,
                               ma_uint32* sr, ma_channel* chMap, size_t chMapCap) {
    VorbisBackend* b = (VorbisBackend*)pDS;
    if (fmt) *fmt = ma_format_f32;
    if (ch)  *ch  = b->channels ? b->channels : 2;
    if (sr)  *sr  = b->rate ? b->rate : 44100;   // native rate; the converter resamples
    if (chMap && chMapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, chMap, chMapCap, b->channels ? b->channels : 2);
    return MA_SUCCESS;
}

ma_result vorbis_ds_get_cursor(ma_data_source* pDS, ma_uint64* pCursor) {
    VorbisBackend* b = (VorbisBackend*)pDS;
    if (pCursor) *pCursor = b->cursor_frames;
    return MA_SUCCESS;
}

ma_result vorbis_ds_get_length(ma_data_source* pDS, ma_uint64* pLength) {
    VorbisBackend* b = (VorbisBackend*)pDS;
    ogg_int64_t total = ov_pcm_total(&b->vf, -1);
    if (total < 0) return MA_NOT_IMPLEMENTED;    // unseekable/undetermined stream
    if (pLength) *pLength = (ma_uint64)total;
    return MA_SUCCESS;
}

ma_data_source_vtable g_vorbis_ds_vtable = {
    vorbis_ds_read, vorbis_ds_seek, vorbis_ds_get_format, vorbis_ds_get_cursor, vorbis_ds_get_length, nullptr, 0
};

// ── sniff + construct ─────────────────────────────────────────────────────────
bool looksOgg(const uint8_t* d, size_t n) { return n >= 4 && std::memcmp(d, "OggS", 4) == 0; }

ma_result vorbis_construct(VorbisBackend* b) {
    ma_data_source_config dsc = ma_data_source_config_init();
    dsc.vtable = &g_vorbis_ds_vtable;
    ma_result r = ma_data_source_init(&dsc, &b->ds);
    if (r != MA_SUCCESS) return r;

    if (!looksOgg(b->file.data(), b->file.size())) { ma_data_source_uninit(&b->ds); return MA_NO_BACKEND; }

    // libvorbisfile's own parser is the Vorbis-vs-Opus discriminator: an Opus
    // (or FLAC/Theora/...) Ogg fails here with OV_ENOTVORBIS -> MA_NO_BACKEND.
    // On failure vf must NOT be ov_clear'd (vorbisfile contract).
    b->mem = { b->file.data(), b->file.size(), 0 };
    if (ov_open_callbacks(&b->mem, &b->vf, nullptr, 0, g_mem_callbacks) != 0) {
        ma_data_source_uninit(&b->ds);
        return MA_NO_BACKEND;
    }
    b->opened = true;

    vorbis_info* vi = ov_info(&b->vf, -1);
    if (!vi || vi->channels <= 0 || vi->rate <= 0) {
        ov_clear(&b->vf); b->opened = false;
        ma_data_source_uninit(&b->ds);
        return MA_NO_BACKEND;
    }
    b->channels = (uint32_t)vi->channels;
    b->rate     = (uint32_t)vi->rate;
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
    VorbisBackend* b = new (std::nothrow) VorbisBackend();
    if (!b) return MA_OUT_OF_MEMORY;
    b->file.assign((const uint8_t*)pData, (const uint8_t*)pData + dataSize);
    ma_result r = vorbis_construct(b);
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
    VorbisBackend* b = (VorbisBackend*)pBackend;
    if (b->opened) ov_clear(&b->vf);   // BEFORE delete: vf reads b->file via the callbacks
    ma_data_source_uninit(&b->ds);
    delete b;
}

ma_decoding_backend_vtable g_vorbis_backend_vtable = {
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

extern "C" ma_decoding_backend_vtable* ma_vorbis_backend_vtable(void) {
    return &g_vorbis_backend_vtable;
}
