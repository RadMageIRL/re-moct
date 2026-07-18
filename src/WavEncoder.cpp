// WavEncoder.cpp — see WavEncoder.h. Header fields are serialized explicitly
// little-endian byte-by-byte: both targets are LE (so this is moot today) but
// the file format is LE by spec and the code shouldn't rely on host order.
#include "WavEncoder.h"

#include "PortUtil.h"   // port::fopenUtf8 — same open call the sibling encoders use

#include <cstdint>
#include <cstring>

namespace {
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint16_t CHANNELS    = 2;
constexpr uint16_t BITS        = 16;
constexpr uint16_t BLOCK_ALIGN = CHANNELS * (BITS / 8);          // 4
constexpr uint32_t BYTE_RATE   = SAMPLE_RATE * BLOCK_ALIGN;      // 176400

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// The canonical 44-byte header for a given data size.
void buildHeader(uint8_t h[44], uint32_t data_bytes) {
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    std::memcpy(h + 8,  "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);           // fmt chunk size
    put16(h + 20, 1);            // PCM
    put16(h + 22, CHANNELS);
    put32(h + 24, SAMPLE_RATE);
    put32(h + 28, BYTE_RATE);
    put16(h + 32, BLOCK_ALIGN);
    put16(h + 34, BITS);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}
} // namespace

bool WavEncoder::open(const std::string& path, uint64_t total_frames) {
    f_ = port::fopenUtf8(path, "wb");
    if (!f_) return false;
    declared_frames_ = total_frames;
    written_frames_  = 0;
    uint8_t h[44];
    buildHeader(h, (uint32_t)(total_frames * BLOCK_ALIGN));
    if (fwrite(h, 1, sizeof(h), f_) != sizeof(h)) {
        fclose(f_); f_ = nullptr;
        return false;
    }
    return true;
}

bool WavEncoder::writeFrames(const int16_t* interleaved, size_t frames) {
    if (!f_) return false;
    // STRICT: a short write is a hard failure (see header comment) — the
    // track aborts and the caller removes the partial file.
    if (fwrite(interleaved, BLOCK_ALIGN, frames, f_) != frames) return false;
    written_frames_ += frames;
    return true;
}

bool WavEncoder::finalize(bool ok) {
    if (!f_) return true;
    // Insurance for non-exact callers only: the CD path's frame count is
    // TOC-exact (kept files always match their header), and a short WRITE
    // already failed the track via the strict writeFrames above.
    if (ok && written_frames_ != declared_frames_) {
        uint8_t h[44];
        buildHeader(h, (uint32_t)(written_frames_ * BLOCK_ALIGN));
        fseek(f_, 0, SEEK_SET);
        fwrite(h, 1, sizeof(h), f_);
    }
    // Strict writeFrames guarantees fwrite accepted every byte — but a final
    // sub-buffer tail can still sit in stdio (the ENOSPC chain-test lesson
    // from the WavPack slice); flush and check before declaring completion.
    bool completed = !ok || fflush(f_) == 0;
    fclose(f_);
    f_ = nullptr;
    return completed;
}
