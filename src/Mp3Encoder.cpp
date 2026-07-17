// Mp3Encoder.cpp — see Mp3Encoder.h. The bodies below are the inline LAME
// blocks from CDRipper::ripTrack moved verbatim (setup / encode_buffer+fwrite /
// flush + Xing rewrite + close); only the buffer names changed. Do not
// "improve" values or ordering here — byte-identity with the pre-seam rip
// output is the contract, and rip_encoder_seam_test diffs this class against
// a frozen copy of the original inline sequence.
#include "Mp3Encoder.h"

#include "PortUtil.h"   // port::fopenUtf8 — same open call the inline code used

namespace {
constexpr int SAMPLE_RATE = 44100;
constexpr int CHANNELS    = 2;
} // namespace

bool Mp3Encoder::open(const std::string& path, uint64_t total_frames) {
    (void)total_frames;   // LAME derives duration itself (Xing frame count)
    lame_ = lame_init();
    if (!lame_) return false;
    lame_set_in_samplerate(lame_, SAMPLE_RATE);
    lame_set_num_channels(lame_, CHANNELS);
    lame_set_VBR(lame_, vbr_mtrh);
    lame_set_VBR_q(lame_, vbr_q_);
    lame_set_quality(lame_, 2);
    lame_set_mode(lame_, STEREO);
    lame_set_write_id3tag_automatic(lame_, 0);
    lame_set_bWriteVbrTag(lame_, 1);   // emit Xing/LAME info header (VBR duration)
    if (lame_init_params(lame_) < 0) {
        lame_close(lame_); lame_ = nullptr;
        return false;
    }
    file_ = port::fopenUtf8(path, "wb");
    if (!file_) {
        lame_close(lame_); lame_ = nullptr;
        return false;
    }
    return true;
}

bool Mp3Encoder::writeFrames(const int16_t* interleaved, size_t frames) {
    if (!lame_ || !file_) return false;
    left_.resize(frames);
    right_.resize(frames);
    for (size_t i = 0; i < frames; ++i) {          // the ripTrack deinterleave, verbatim
        left_[i]  = interleaved[i*2];
        right_[i] = interleaved[i*2 + 1];
    }
    // Same worst-case sizing rule as the inline mp3_out buffer: samples*2 + 7200.
    out_.resize(frames * 2 + 7200);
    int mp3b = lame_encode_buffer(lame_, left_.data(), right_.data(),
                                  (int)frames, out_.data(), (int)out_.size());
    if (mp3b < 0) return false;
    if (mp3b > 0) fwrite(out_.data(), 1, (size_t)mp3b, file_);
    return true;
}

bool Mp3Encoder::finalize(bool ok) {
    if (!lame_ && !file_) return true;
    if (ok && lame_ && file_) {
        out_.resize(out_.size() > 7200 ? out_.size() : 7200);
        int fb = lame_encode_flush(lame_, out_.data(), (int)out_.size());
        if (fb > 0) fwrite(out_.data(), 1, (size_t)fb, file_);
        // Overwrite the reserved first frame with the real Xing/LAME info tag so
        // VBR MP3s report the correct duration (frame count) to every player.
        unsigned char vbr_tag[2880];
        size_t tag_n = lame_get_lametag_frame(lame_, vbr_tag, sizeof(vbr_tag));
        if (tag_n > 0 && tag_n <= sizeof(vbr_tag)) {
            fseek(file_, 0, SEEK_SET);
            fwrite(vbr_tag, 1, tag_n, file_);
        }
    }
    if (file_) { fclose(file_); file_ = nullptr; }
    if (lame_) { lame_close(lame_); lame_ = nullptr; }
    return true;   // best-effort, exactly the pre-seam inline behavior
}
