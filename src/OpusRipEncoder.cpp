// OpusRipEncoder.cpp — see OpusRipEncoder.h.
//
// The include keeps the explicit opus/ prefix (the decode-slice rule; the CI
// grep gate guards opusfile/vorbisfile — opusenc has no TagLib collision but
// follows the same convention).
#include "OpusRipEncoder.h"

#include "PortUtil.h"   // port::fopenUtf8 — the wide-path open, ours not the lib's

#include <opus/opusenc.h>

namespace {
// fwrite-shaped sink callbacks: libopusenc writes through OUR FILE*.
int ope_write_cb(void* ud, const unsigned char* p, opus_int32 len) {
    return fwrite(p, 1, (size_t)len, (FILE*)ud) != (size_t)len;   // 0 = success
}
int ope_close_cb(void* ud) {
    (void)ud;   // the encoder owns the FILE* lifetime; closed in finalize
    return 0;
}
const OpusEncCallbacks g_file_callbacks = { ope_write_cb, ope_close_cb };
} // namespace

bool OpusRipEncoder::open(const std::string& path, uint64_t total_frames) {
    (void)total_frames;   // no header size concept (like Mp3Encoder)
    f_ = port::fopenUtf8(path, "wb");
    if (!f_) return false;
    cmts_ = ope_comments_create();
    if (!cmts_) { fclose(f_); f_ = nullptr; return false; }
    // Real tags (Xiph fields, art, R128 gains) come from the post-loop tagFile
    // pass; only the encoder stamp is known now.
    ope_comments_add(cmts_, "ENCODER", "RE-MOCT libopusenc");

    int err = 0;
    // 44100 input; libopusenc resamples to 48k internally (probe-proven).
    enc_ = ope_encoder_create_callbacks(&g_file_callbacks, f_, cmts_, 44100, 2, 0, &err);
    if (!enc_) {
        ope_comments_destroy(cmts_); cmts_ = nullptr;
        fclose(f_); f_ = nullptr;
        return false;
    }
    ope_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_));   // VBR is libopusenc's default
    // encoder-bitrate-mode: enforce the mode explicitly. vbr_=true reasserts the
    // libopusenc default (output unchanged); vbr_=false is constrained CBR.
    ope_encoder_ctl(enc_, OPUS_SET_VBR(vbr_ ? 1 : 0));
    // Header output gain 0: the R128 tags are the ONLY gain (RFC 7845; the
    // decode side applies OP_HEADER_GAIN and defines the tags relative to it).
    ope_encoder_ctl(enc_, OPE_SET_HEADER_GAIN(0));
    return true;
}

bool OpusRipEncoder::writeFrames(const int16_t* interleaved, size_t frames) {
    if (!enc_) return false;
    // STRICT: any libopusenc error fails the track (the IEncoder contract);
    // the caller removes the partial file.
    return ope_encoder_write(enc_, interleaved, (int)frames) == OPE_OK;
}

bool OpusRipEncoder::finalize(bool ok) {
    bool completed = true;
    if (enc_) {
        // drain flushes the final pages through the write callback — the same
        // buffering-encoder trap as WavPack's flush: a disk-full here must
        // fail the track, not ship a truncated file marked success.
        if (ok) completed = ope_encoder_drain(enc_) == OPE_OK;
        ope_encoder_destroy(enc_);
        enc_ = nullptr;
    }
    if (cmts_) { ope_comments_destroy(cmts_); cmts_ = nullptr; }
    if (f_) {
        // Same stdio-buffer layer as WavPack: the drained pages may still sit
        // in the FILE* buffer — flush and check before declaring completion.
        if (completed && fflush(f_) != 0) completed = false;
        fclose(f_); f_ = nullptr;
    }
    return completed;
}
