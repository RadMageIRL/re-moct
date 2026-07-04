// AacDecoder.h — miniaudio custom decoding backend for AAC files (FDK-AAC).
//
// Handles:
//   * .aac  — raw ADTS AAC (same transport as the internet-radio path)
//   * .m4a / .mp4 — AAC-LC / HE-AAC inside an ISO-BMFF (MP4) container
//
// The backend is registered in a ma_decoder_config via ppCustomBackendVTables.
// miniaudio tries custom backends BEFORE its built-ins, so this vtable sniffs the
// file and fails fast (MA_NO_BACKEND) for anything that isn't AAC — letting the
// built-in FLAC / MP3 / WAV / Vorbis decoders take over unchanged.
//
// Output is signed-16 PCM; miniaudio converts/resamples to the device format, so
// every downstream feature (seek, ReplayGain, BPM, visualizer) works as-is.
#pragma once

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the singleton AAC decoding-backend vtable. Register it like:
//   ma_decoding_backend_vtable* vt[] = { ma_aac_backend_vtable() };
//   cfg.ppCustomBackendVTables = vt;
//   cfg.customBackendCount     = 1;
//   cfg.pCustomBackendUserData = NULL;
ma_decoding_backend_vtable* ma_aac_backend_vtable(void);

#ifdef __cplusplus
}
#endif
