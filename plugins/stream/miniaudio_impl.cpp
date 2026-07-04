// miniaudio_impl.cpp — the streaming plugin's OWN miniaudio implementation TU
// (Phase 4 slice c).
//
// StreamSource decodes MP3 (ma_decoder / dr_mp3) and resamples (ma_data_converter).
// The host compiles MINIAUDIO_IMPLEMENTATION in AudioManager.cpp, but that lives in
// the HOST binary; once StreamSource is a loaded .so/.dll it cannot resolve those
// symbols across the module boundary, so the plugin carries its own copy.
//
// MA_NO_DEVICE_IO: the plugin NEVER opens an audio device — the host owns the
// device and calls read_frames into its buffer. Trimming the device/backend layer
// keeps PulseAudio/WASAPI out of the plugin and makes device ownership a build-time
// invariant. Decoding + data conversion (all StreamSource uses) remain available.
//
// Two miniaudio implementations in one process is safe: they are separate binaries,
// and the loader uses dlopen(RTLD_LOCAL) on POSIX / per-module symbol space on
// Windows, so the plugin's miniaudio symbols never collide with the host's. Same
// reasoning as the dual Log.cpp (D1).
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING          // decode-only: the plugin never encodes
#include "miniaudio.h"
