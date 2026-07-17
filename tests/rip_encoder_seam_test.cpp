// rip_encoder_seam_test — the rip-encoder-seam byte-identity oracle.
//
// The reference arm below is the FLAC/LAME encode sequence that lived INLINE
// in CDRipper::ripTrack before the seam, copied VERBATIM (setup :787-830,
// per-block conversion+encode :1037-1074, offset-tail :1159-1165, finalize
// :1172-1187 at the pre-seam tree) and frozen here. The seam arm drives the
// same fixed PCM fixture through FlacEncoder/Mp3Encoder. The test asserts the
// two .flac outputs and the two .mp3 outputs are BYTE-IDENTICAL — the
// transliteration-fidelity guarantee, machine-enforced on both platforms,
// no disc required. It complements the Joan Osborne hardware gate, never
// replaces it.
//
// The fixture mimics the real call shape: 27-sector blocks, a short final
// block, then a small odd-length "tail" call (the corr_sub_skip borrow).

#include "FlacEncoder.h"
#include "Mp3Encoder.h"

#include <FLAC/stream_encoder.h>
#include <lame/lame.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int SECTOR_SAMPLES = 588;
constexpr int SECTORS_PER_READ = 27;
constexpr int SAMPLE_RATE = 44100;
constexpr int CHANNELS = 2;
constexpr int BIT_DEPTH = 16;
constexpr int FLAC_COMPRESSION = 5;

// Deterministic PCM: seeded LCG, no time/no rand — identical on every run
// and platform. ~100 sectors + a 137-frame tail.
std::vector<int16_t> makeFixture(size_t frames) {
    std::vector<int16_t> pcm(frames * 2);
    uint32_t x = 0x2D5C1E7Bu;
    for (auto& s : pcm) {
        x = x * 1103515245u + 12345u;
        s = (int16_t)(x >> 16);
    }
    return pcm;
}

// The block schedule the real loop produces for a 100-sector track:
// 27,27,27,19 sectors, then the odd tail.
struct Block { const int16_t* p; int samples; };
std::vector<Block> schedule(const std::vector<int16_t>& pcm, int tail_frames) {
    std::vector<Block> b;
    int total = (int)(pcm.size() / 2) - tail_frames;
    const int16_t* p = pcm.data();
    int left = total;
    while (left > 0) {
        int n = std::min(left, SECTORS_PER_READ * SECTOR_SAMPLES);
        b.push_back({ p, n });
        p    += (size_t)n * 2;
        left -= n;
    }
    b.push_back({ p, tail_frames });   // the corr_sub_skip-style tail call
    return b;
}

// ── REFERENCE ARM — the pre-seam inline sequence, VERBATIM (frozen) ─────────
bool encodeInline(const std::string& flac_path, const std::string& mp3_path,
                  const std::vector<Block>& blocks, uint64_t total_frames) {
    // ── FLAC setup ──────────────────────────────────────────────── (:787)
    FLAC__StreamEncoder* flac_enc = FLAC__stream_encoder_new();
    if (!flac_enc) return false;
    FLAC__stream_encoder_set_verify(flac_enc, false);
    FLAC__stream_encoder_set_compression_level(flac_enc, FLAC_COMPRESSION);
    FLAC__stream_encoder_set_channels(flac_enc, CHANNELS);
    FLAC__stream_encoder_set_bits_per_sample(flac_enc, BIT_DEPTH);
    FLAC__stream_encoder_set_sample_rate(flac_enc, SAMPLE_RATE);
    FLAC__stream_encoder_set_total_samples_estimate(flac_enc, (FLAC__uint64)total_frames);
    FILE* flac_file = fopen(flac_path.c_str(), "wb");
    if (!flac_file) { FLAC__stream_encoder_delete(flac_enc); return false; }
    if (FLAC__stream_encoder_init_FILE(flac_enc, flac_file, nullptr, nullptr)
            != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        fclose(flac_file); FLAC__stream_encoder_delete(flac_enc); return false;
    }
    // ── LAME setup ──────────────────────────────────────────────── (:804)
    lame_global_flags* lame = lame_init();
    if (!lame) { FLAC__stream_encoder_finish(flac_enc); FLAC__stream_encoder_delete(flac_enc); return false; }
    lame_set_in_samplerate(lame, SAMPLE_RATE);
    lame_set_num_channels(lame, CHANNELS);
    lame_set_VBR(lame, vbr_mtrh);
    lame_set_VBR_q(lame, 0);
    lame_set_quality(lame, 2);
    lame_set_mode(lame, STEREO);
    lame_set_write_id3tag_automatic(lame, 0);
    lame_set_bWriteVbrTag(lame, 1);
    if (lame_init_params(lame) < 0) {
        lame_close(lame);
        FLAC__stream_encoder_finish(flac_enc); FLAC__stream_encoder_delete(flac_enc);
        return false;
    }
    FILE* mp3_file = fopen(mp3_path.c_str(), "wb");
    if (!mp3_file) {
        lame_close(lame);
        FLAC__stream_encoder_finish(flac_enc); FLAC__stream_encoder_delete(flac_enc);
        return false;
    }

    // Per-block buffers, sized as the real BUF_SECTORS ones (:905-909)
    std::vector<FLAC__int32> flac_buf((size_t)SECTORS_PER_READ * SECTOR_SAMPLES * 2);
    std::vector<int16_t> lame_left((size_t)SECTORS_PER_READ * SECTOR_SAMPLES);
    std::vector<int16_t> lame_right((size_t)SECTORS_PER_READ * SECTOR_SAMPLES);
    std::vector<uint8_t> mp3_out((size_t)SECTORS_PER_READ * SECTOR_SAMPLES * 2 + 7200);

    bool ok = true;
    for (auto& blk : blocks) {
        const int16_t* src = blk.p;
        int samples = blk.samples;
        for (int i = 0; i < samples; ++i) {                     // (:1037-1042)
            int16_t l = src[i*2], r = src[i*2+1];
            flac_buf[(size_t)i*2]     = (FLAC__int32)l;
            flac_buf[(size_t)i*2 + 1] = (FLAC__int32)r;
            lame_left[(size_t)i]      = l;
            lame_right[(size_t)i]     = r;
        }
        // FLAC encode                                            (:1067)
        if (!FLAC__stream_encoder_process_interleaved(
                flac_enc, flac_buf.data(), (unsigned)samples)) { ok = false; break; }
        // MP3 encode                                             (:1071)
        int mp3b = lame_encode_buffer(lame, lame_left.data(), lame_right.data(),
                                      samples, mp3_out.data(), (int)mp3_out.size());
        if (mp3b < 0) { ok = false; break; }
        if (mp3b > 0) fwrite(mp3_out.data(), 1, (size_t)mp3b, mp3_file);
    }

    // ── Flush encoders ──────────────────────────────────────────── (:1172)
    FLAC__stream_encoder_finish(flac_enc);
    FLAC__stream_encoder_delete(flac_enc);
    if (ok) {
        int fb = lame_encode_flush(lame, mp3_out.data(), (int)mp3_out.size());
        if (fb > 0) fwrite(mp3_out.data(), 1, (size_t)fb, mp3_file);
        unsigned char vbr_tag[2880];
        size_t tag_n = lame_get_lametag_frame(lame, vbr_tag, sizeof(vbr_tag));
        if (tag_n > 0 && tag_n <= sizeof(vbr_tag)) {
            fseek(mp3_file, 0, SEEK_SET);
            fwrite(vbr_tag, 1, tag_n, mp3_file);
        }
    }
    fclose(mp3_file);
    lame_close(lame);
    return ok;
}

// ── SEAM ARM ────────────────────────────────────────────────────────────────
bool encodeSeam(const std::string& flac_path, const std::string& mp3_path,
                const std::vector<Block>& blocks, uint64_t total_frames) {
    FlacEncoder flac;
    Mp3Encoder  mp3;
    if (!flac.open(flac_path, total_frames)) return false;
    if (!mp3.open(mp3_path, total_frames))  { flac.finalize(false); return false; }
    bool ok = true;
    for (auto& blk : blocks) {
        if (!flac.writeFrames(blk.p, (size_t)blk.samples)) { ok = false; break; }
        if (!mp3.writeFrames(blk.p, (size_t)blk.samples))  { ok = false; break; }
    }
    flac.finalize(ok);
    mp3.finalize(ok);
    return ok;
}

std::vector<uint8_t> slurp(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    if (sz > 0 && fread(out.data(), 1, (size_t)sz, f) != (size_t)sz) out.clear();
    fclose(f);
    return out;
}

int failures = 0;
void check(bool cond, const char* what) {
    std::printf("%-52s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) ++failures;
}

} // namespace

int main() {
    const size_t kTail   = 137;                              // odd, sub-sector
    const size_t kFrames = (size_t)100 * SECTOR_SAMPLES + kTail;
    auto pcm    = makeFixture(kFrames);
    auto blocks = schedule(pcm, (int)kTail);

    check(blocks.size() == 5, "block schedule 27+27+27+19 sectors + tail");

    bool ref_ok  = encodeInline("seam_ref.flac",  "seam_ref.mp3",  blocks, kFrames);
    bool seam_ok = encodeSeam("seam_new.flac",  "seam_new.mp3",  blocks, kFrames);
    check(ref_ok,  "reference (frozen inline) arm encoded");
    check(seam_ok, "seam (FlacEncoder/Mp3Encoder) arm encoded");

    auto rf = slurp("seam_ref.flac"), nf = slurp("seam_new.flac");
    auto rm = slurp("seam_ref.mp3"),  nm = slurp("seam_new.mp3");
    std::printf("  flac: ref=%zu bytes, seam=%zu bytes\n", rf.size(), nf.size());
    std::printf("  mp3:  ref=%zu bytes, seam=%zu bytes\n", rm.size(), nm.size());
    check(!rf.empty() && rf == nf, "FLAC output BYTE-IDENTICAL to inline");
    check(!rm.empty() && rm == nm, "MP3 output BYTE-IDENTICAL to inline");

    std::remove("seam_ref.flac"); std::remove("seam_new.flac");
    std::remove("seam_ref.mp3");  std::remove("seam_new.mp3");

    if (failures) { std::printf("%d FAILURE(S)\n", failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
