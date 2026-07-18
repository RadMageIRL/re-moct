// WavPackEncoder.cpp — see WavPackEncoder.h.
#include "WavPackEncoder.h"

#include "PortUtil.h"   // port::fopenUtf8 — the wide-path open; no char* path
                        // ever crosses libwavpack (block callback over OUR FILE*)

#include <wavpack/wavpack.h>

#include <cstring>

namespace {
// Mode index -> libwavpack config flags (normal = no flag).
int modeFlags(int mode) {
    switch (mode) {
        case 0: return CONFIG_FAST_FLAG;
        case 2: return CONFIG_HIGH_FLAG;
        case 3: return CONFIG_VERY_HIGH_FLAG;
        default: return 0;   // 1 = normal
    }
}
} // namespace

// STRICT block output (the fold-in): a short fwrite returns failure to
// libwavpack AND sets the sticky flag — the pair makes a lying success
// unrepresentable no matter how the library propagates the error.
int WavPackEncoder::blockOut(void* id, void* data, int32_t bcount) {
    auto* self = (WavPackEncoder*)id;
    if (!self->first_block_seen_) {
        // Keep a copy of the FIRST block for the non-exact-caller insurance
        // in finalize (WavpackUpdateNumSamples rewrites it). Dead path on the
        // CD rip (totals are TOC-exact); tiny, one block.
        self->first_block_.assign((unsigned char*)data,
                                  (unsigned char*)data + bcount);
        self->first_block_seen_ = true;
    }
    size_t w = fwrite(data, 1, (size_t)bcount, self->f_);
    if (w != (size_t)bcount) { self->write_failed_ = true; return 0; }
    return 1;
}

bool WavPackEncoder::open(const std::string& path, uint64_t total_frames) {
    f_ = port::fopenUtf8(path, "wb");
    if (!f_) return false;
    declared_frames_  = total_frames;
    write_failed_     = false;
    first_block_seen_ = false;
    first_block_.clear();

    wpc_ = WavpackOpenFileOutput(&WavPackEncoder::blockOut, this, nullptr /* no .wvc: pure lossless */);
    if (!wpc_) { fclose(f_); f_ = nullptr; return false; }

    WavpackConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.bits_per_sample  = 16;      // exactly CD-DA — the round-trip test
    cfg.bytes_per_sample = 2;       // pins these (wrong values can't survive it)
    cfg.num_channels     = 2;
    cfg.channel_mask     = 3;       // stereo L+R
    cfg.sample_rate      = 44100;
    cfg.flags            = modeFlags(mode_);
    if (!WavpackSetConfiguration64(wpc_, &cfg, (int64_t)total_frames, nullptr) ||
        !WavpackPackInit(wpc_)) {
        WavpackCloseFile(wpc_); wpc_ = nullptr;
        fclose(f_); f_ = nullptr;
        return false;
    }
    return true;
}

bool WavPackEncoder::writeFrames(const int16_t* interleaved, size_t frames) {
    if (!wpc_ || write_failed_) return false;
    stage_.resize(frames * 2);
    for (size_t i = 0; i < frames * 2; ++i)      // sign-extending widen
        stage_[i] = interleaved[i];
    // Probe-proven: a failed block callback surfaces here as 0 — and the
    // sticky flag covers any propagation gap.
    if (!WavpackPackSamples(wpc_, stage_.data(), (uint32_t)frames)) return false;
    return !write_failed_;
}

bool WavPackEncoder::finalize(bool ok) {
    bool completed = true;
    if (wpc_) {
        if (ok) {
            // The final block flushes HERE — a disk-full on it must fail the
            // track (the reason IEncoder::finalize returns bool).
            if (!WavpackFlushSamples(wpc_)) completed = false;
            if (write_failed_)              completed = false;
            // Third layer of the trap (found by the ENOSPC chain test): small
            // compressed blocks can sit in STDIO'S buffer — fwrite "succeeds",
            // the library flush "succeeds", and the real ENOSPC would only
            // surface at an unchecked fclose. Flush the FILE* and check it.
            if (completed && fflush(f_) != 0) completed = false;
            // Insurance for a legitimate non-exact caller only (never the CD
            // path): with completed==true every byte is on disk, so a sample
            // count mismatch can only mean the caller declared a different
            // total — patch the first block's header copy and rewrite it.
            if (completed && !first_block_.empty() &&
                (uint64_t)WavpackGetSampleIndex64(wpc_) != declared_frames_ &&
                WavpackGetSampleIndex64(wpc_) >= 0) {
                WavpackUpdateNumSamples(wpc_, first_block_.data());
                fseek(f_, 0, SEEK_SET);
                if (fwrite(first_block_.data(), 1, first_block_.size(), f_)
                        != first_block_.size())
                    completed = false;
            }
        }
        WavpackCloseFile(wpc_); wpc_ = nullptr;
    }
    if (f_) { fclose(f_); f_ = nullptr; }
    return completed;
}
