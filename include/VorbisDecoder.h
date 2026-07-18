// VorbisDecoder.h — miniaudio custom decoding backend for Ogg Vorbis
// (libvorbisfile).
//
// Handles:
//   * .ogg — Ogg Vorbis
//
// Registered through CustomBackends.h alongside the FDK-AAC, Opus, and
// WavPack backends. The vtable sniffs for the Ogg capture pattern and then
// lets libvorbisfile's own parser reject non-Vorbis Ogg (e.g. Opus), failing
// fast (MA_NO_BACKEND) so the other decoders proceed unchanged — the mirror
// image of OpusDecoder's discrimination, so the two OggS backends cannot
// capture each other's files.
//
// Output is 32-bit float PCM at the file's native rate: Vorbis decodes to
// float natively (MDCT), so ov_read_float hands it over in one hop with no
// s16 quantize-then-rewiden round trip, and transient peaks that overshoot
// ±1.0 survive to the ReplayGain stage instead of clipping at decode.
// miniaudio's converter handles rate/channels to the device format.
#pragma once

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the singleton Vorbis decoding-backend vtable (registered via
// remoct_custom_backends() — see CustomBackends.h).
ma_decoding_backend_vtable* ma_vorbis_backend_vtable(void);

#ifdef __cplusplus
}
#endif
