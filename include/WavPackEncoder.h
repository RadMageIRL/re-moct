// WavPackEncoder.h — IEncoder for WavPack rip output (rip-wavpack-encoder
// slice, the final encoder). libwavpack — the SAME library the decode slice
// already links, both directions, so this slice adds zero dependencies.
// PURE LOSSLESS only: wvc_id = NULL, no hybrid correction file (matching the
// decode side's structural .wvc deferral); the row's * master marker is
// backed by the bit-exact round-trip test through the shipping decoder.
//
// The write path is a BLOCK CALLBACK — libwavpack buffers and writes whole
// blocks, so the disk-full trap sits one level deeper than WAV's: the
// callback must be strict (short fwrite -> failure to libwavpack) or a
// truncated .wv ships marked success, and the finalize insurance CANNOT
// catch it (WavpackGetSampleIndex64 counts samples SUBMITTED, not bytes
// written). Probe-proven propagation: a failed callback returns 0 through
// BOTH WavpackPackSamples and WavpackFlushSamples (note WavpackGetNumErrors
// stays 0 — it counts decode CRC errors, never write failures; do not rely
// on it). A sticky write_failed_ flag backs the propagation up, and
// finalize(true) checks WavpackFlushSamples — the final block flushes there,
// which is why IEncoder::finalize returns bool.
#pragma once

#include "IEncoder.h"

#include <cstdio>
#include <cstdint>
#include <vector>

// Compression mode: 0 fast, 1 normal, 2 high, 3 very high. Named default so
// a drift is visible in review; the config default maps to the same constant.
inline constexpr int kWavPackDefaultMode = 1;   // normal

typedef struct WavpackContext WavpackContext;   // matches wavpack.h's typedef

class WavPackEncoder : public IEncoder {
public:
    explicit WavPackEncoder(int mode = kWavPackDefaultMode) : mode_(mode) {}
    ~WavPackEncoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    bool finalize(bool ok) override;

private:
    static int blockOut(void* id, void* data, int32_t bcount);   // strict

    int             mode_ = kWavPackDefaultMode;
    FILE*           f_    = nullptr;
    WavpackContext* wpc_  = nullptr;
    bool            write_failed_    = false;  // sticky: any short block write
    bool            first_block_seen_ = false;
    std::vector<unsigned char> first_block_;   // for the non-exact-caller insurance
    uint64_t        declared_frames_ = 0;
    std::vector<int32_t> stage_;               // s16 -> int32 widening buffer
};
