// EncoderFactory.cpp - the one place a RipFormat becomes a concrete IEncoder.
//
// Lifted verbatim out of CDRipper.cpp (was file-static there) so a second
// caller - the convert-core engine - can link it without pulling in the entire
// rip translation unit (CD I/O, HTTP, MusicBrainz, ...). The switch body and the
// encoder constructor arguments are unchanged, so rip output stays byte-
// identical (pinned by rip_encoder_seam_test, which constructs the encoders
// directly and never calls this factory).
#include "EncoderFactory.h"

#include "FlacEncoder.h"
#include "Mp3Encoder.h"
#include "WavEncoder.h"
#include "OpusRipEncoder.h"
#include "WavPackEncoder.h"

#include <memory>

std::unique_ptr<IEncoder> makeEncoder(RipFormat f, const RipOptions& opt) {
    switch (f) {
        case RipFormat::Flac: return std::make_unique<FlacEncoder>(opt.flac_level);
        case RipFormat::Mp3:  return std::make_unique<Mp3Encoder>(opt.mp3_vbr_q, opt.mp3_cbr, opt.mp3_cbr_bitrate);
        case RipFormat::Wav:  return std::make_unique<WavEncoder>();
        case RipFormat::Opus: return std::make_unique<OpusRipEncoder>(opt.opus_bitrate, opt.opus_vbr);
        case RipFormat::WavPack: return std::make_unique<WavPackEncoder>(opt.wavpack_mode);
    }
    return nullptr;
}
