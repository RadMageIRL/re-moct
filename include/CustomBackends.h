// CustomBackends.h — the process-wide list of miniaudio custom decoding
// backends (FDK-AAC, Opus, WavPack).
//
// Every ma_decoder that should understand the custom formats registers THIS
// array, so the playback path (LocalFileSource's open_decoder) and the
// BPM-analysis path (AudioManager::detectBpm) cannot drift: adding a codec is
// a one-line edit in CustomBackends.cpp that every consumer inherits. The
// consumers' ma_decoder_configs legitimately differ (forced 44100/2 playback
// vs. native-rate mono analysis) — only the backend array is shared.
#pragma once

#include "miniaudio.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns the singleton custom decoding-backend vtable array. Register like:
//   size_t n = 0;
//   ma_decoding_backend_vtable** vts = remoct_custom_backends(&n);
//   cfg.ppCustomBackendVTables = vts;
//   cfg.customBackendCount     = (ma_uint32)n;
//   cfg.pCustomBackendUserData = NULL;
ma_decoding_backend_vtable** remoct_custom_backends(size_t* count);

#ifdef __cplusplus
}
#endif
