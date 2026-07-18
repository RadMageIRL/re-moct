// M4aEncoder.h — IEncoder for AAC-LC in an MP4 (.m4a) container (aac-m4a-encoder
// slice). FDK-AAC encoder (the SAME libfdk-aac already linked for decode - no new
// dependency, no new DLL) emitting raw access units, muxed by Mp4Mux.h.
//
// STRUCTURALLY UNLIKE the other IEncoders: MP4's moov (sample table) needs the
// COMPLETE access-unit list before a single byte of the container can be written,
// so this encoder BUFFERS. writeFrames stages PCM to FDK in whole 1024-sample
// frames and collects the encoded AUs in memory (a whole track's AAC is a few MB
// - the same order AacDecoder already slurps per file); finalize flushes FDK's
// tail, assembles ftyp + mdat + moov via Mp4Mux, and does the single file write.
// finalize therefore returns whether that write COMPLETED (a buffering encoder
// only learns of disk-full at flush - the WavPack/ENOSPC lesson, IEncoder.h).
//
// AAC-LC only (AOT_AAC_LC); no HE-AAC. VBR = AACENC_BITRATEMODE 1-5 (5 best);
// CBR = mode 0 + AACENC_BITRATE. Tags/art/ReplayGain are the tagFile pass's job
// (CDRipper::tagFile / StreamRecorder::tagCut / ArtEmbed), not this interface.
#pragma once

#include "IEncoder.h"

#include <cstdint>
#include <string>
#include <vector>

// FDK opaque encoder handle (HANDLE_AACENCODER = struct AACENCODER*), forward-
// declared so this header stays free of the heavy aacenc_lib.h.
struct AACENCODER;

// The default AAC VBR quality level, named so a drift is visible in review even
// without a byte-oracle (lossy output has none). Config aac_vbr_level defaults here.
inline constexpr int kAacDefaultVbrLevel = 4;
inline constexpr int kAacDefaultBitrate  = 128000;

class M4aEncoder : public IEncoder {
public:
    // vbr FIRST so a default (or arg-free) construction is the sane default:
    // VBR level 4. CBR is the additive [M]-toggle alternative.
    explicit M4aEncoder(bool vbr = true, int vbr_level = kAacDefaultVbrLevel,
                        int cbr_bitrate_bps = kAacDefaultBitrate)
        : vbr_(vbr), vbr_level_(vbr_level), cbr_bitrate_bps_(cbr_bitrate_bps) {}
    ~M4aEncoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    bool finalize(bool ok) override;

private:
    bool feedFrame(const int16_t* interleaved, int frames);   // whole-frame push to FDK
    bool drainTail();                                         // flush FDK at EOF

    bool        vbr_ = true;
    int         vbr_level_ = kAacDefaultVbrLevel;
    int         cbr_bitrate_bps_ = kAacDefaultBitrate;

    AACENCODER* enc_ = nullptr;
    std::string path_;
    bool        opened_ = false;

    static constexpr uint32_t kSampleRate  = 44100;
    static constexpr uint32_t kChannels    = 2;
    uint32_t    frame_length_ = 1024;              // AAC-LC; set from aacEncInfo

    std::vector<uint8_t>              asc_;         // AudioSpecificConfig (esds)
    std::vector<std::vector<uint8_t>> aus_;         // one raw AAC AU per frame
    std::vector<int16_t>              residual_;    // <1 frame of staged PCM
    uint64_t                          total_bytes_ = 0;  // for the esds bitrate hint
};
