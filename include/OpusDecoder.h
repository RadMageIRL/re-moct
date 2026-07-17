// OpusDecoder.h — miniaudio custom decoding backend for Opus files (libopusfile).
//
// Handles:
//   * .opus — Ogg Opus (RFC 7845)
//
// Registered through CustomBackends.h alongside the FDK-AAC and WavPack
// backends. The vtable sniffs for the Ogg capture pattern and then lets
// libopusfile's own parser reject non-Opus Ogg (e.g. Vorbis), failing fast
// (MA_NO_BACKEND) so the other decoders proceed unchanged.
//
// Output is 32-bit float PCM at 48 kHz — Opus's native rate; libopusfile
// always decodes to 48 kHz — via op_read_float (native-float lossy decode,
// the same output rule as the Vorbis backend; no s16 quantize round trip).
// miniaudio's converter resamples to the device format. libopusfile applies
// the OpusHead output-gain field itself (OP_HEADER_GAIN default, on every
// op_read* entry point), so the R128 ReplayGain tags read in
// populate_track_info are relative to the PCM this backend emits, exactly the
// RFC 7845 convention.
#pragma once

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the singleton Opus decoding-backend vtable (registered via
// remoct_custom_backends() — see CustomBackends.h).
ma_decoding_backend_vtable* ma_opus_backend_vtable(void);

#ifdef __cplusplus
}
#endif
