// OpusRipEncoder.h — IEncoder for Opus rip output (rip-opus-encoder slice).
// libopusenc over an fwrite-callback sink: the file opens via port::fopenUtf8
// and the library only ever sees callbacks — no char* path crosses it (the
// house wide-path rule; ope_encoder_create_file's path is ANSI on Windows).
//
// Named OpusRipEncoder, not OpusEncoder: libopus typedefs a C `OpusEncoder`
// in opus.h, and this encoder's TU includes both that and this header — the
// one rip encoder that can't share its format's bare name.
//
// 44.1 kHz s16 goes in directly: libopusenc resamples to Opus's native 48 kHz
// internally (probe-proven sample-exact at plan stage — 88200 frames in,
// exactly 96000 x 48k out). Header output gain is pinned to 0 so the R128_*
// tags written later by tagFile are the ONLY gain — the same RFC 7845
// convention the decode-side OpusDecoder assumes.
//
// writeFrames is strict (the IEncoder contract): any non-OPE_OK result fails
// the track and the caller removes the partial. Tags/art/R128 are NOT
// written here — album gain only exists after the last track (tagFile pass).
#pragma once

#include "IEncoder.h"

#include <cstdio>

// The default bitrate, named so a drift is visible in review even without a
// byte-oracle (lossy output has none). config opus_bitrate defaults to this.
inline constexpr int kOpusDefaultBitrate = 128000;

struct OggOpusEnc;        // libopusenc opaque types — header stays light
struct OggOpusComments;

class OpusRipEncoder : public IEncoder {
public:
    explicit OpusRipEncoder(int bitrate = kOpusDefaultBitrate) : bitrate_(bitrate) {}
    ~OpusRipEncoder() override { finalize(false); }

    bool open(const std::string& path, uint64_t total_frames) override;
    bool writeFrames(const int16_t* interleaved, size_t frames) override;
    void finalize(bool ok) override;

private:
    int              bitrate_ = kOpusDefaultBitrate;
    FILE*            f_    = nullptr;
    OggOpusEnc*      enc_  = nullptr;
    OggOpusComments* cmts_ = nullptr;
};
