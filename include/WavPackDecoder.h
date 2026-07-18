// WavPackDecoder.h — miniaudio custom decoding backend for WavPack (libwavpack).
//
// Handles:
//   * .wv — plain WavPack (lossless or hybrid-lossy; 16/24/32-bit int, float)
//
// Registered through CustomBackends.h alongside the FDK-AAC and Opus backends.
// The vtable sniffs for the "wvpk" block magic and fails fast (MA_NO_BACKEND)
// on anything else, so the other decoders proceed unchanged.
//
// Output is 32-bit float PCM at the file's native rate — a deliberate
// departure from the AAC backend's s16: WavPack sources are commonly 24-bit,
// the playback chain is f32 anyway, and s16 here would truncate 8 bits for
// nothing. miniaudio's converter handles rate/channels to the device format.
//
// Hybrid .wvc correction sidecars are NOT read (deferred by design): the
// backend decodes from memory with no correction stream, so a hybrid .wv
// plays at its lossy quality and a lossless .wv is bit-exact.
#pragma once

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the singleton WavPack decoding-backend vtable (registered via
// remoct_custom_backends() — see CustomBackends.h).
ma_decoding_backend_vtable* ma_wavpack_backend_vtable(void);

#ifdef __cplusplus
}
#endif
