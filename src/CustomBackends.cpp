// CustomBackends.cpp — see CustomBackends.h. Add new codec backends HERE (and
// only here); every registration site picks them up through the accessor.
#include "CustomBackends.h"

#include "AacDecoder.h"
#include "OpusDecoder.h"
#include "WavPackDecoder.h"

extern "C" ma_decoding_backend_vtable** remoct_custom_backends(size_t* count) {
    // Function-local static: initialized on first call (thread-safe since
    // C++11), so there is no static-init-order coupling to the singletons.
    // Ordering is not load-bearing — the sniffs are disjoint on 4-byte magic
    // (FF Fx/ftyp, OggS, wvpk) and each fails fast with MA_NO_BACKEND.
    static ma_decoding_backend_vtable* k_backends[] = {
        ma_aac_backend_vtable(),
        ma_opus_backend_vtable(),
        ma_wavpack_backend_vtable(),
    };
    if (count) *count = sizeof(k_backends) / sizeof(k_backends[0]);
    return k_backends;
}
