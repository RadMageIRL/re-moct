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

// MINIAUDIO_IMPLEMENTATION: the WavPack round-trip decodes through the
// SHIPPING WavPackDecoder (a miniaudio custom backend), so this test carries
// the miniaudio implementation. No other test TU does; no clash.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "FlacEncoder.h"
#include "Mp3Encoder.h"
#include "WavEncoder.h"
#include "WavPackEncoder.h"
#include "WavPackDecoder.h"
#include "R128Gain.h"

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
// Argument-free construction is deliberate and load-bearing: since the
// rip-format-select slice the quality values are ctor parameters, and this
// arm pins their DEFAULTS (level 5, V0) against the frozen inline reference.
// A drifted default fails this test. (encodeSeamExplicit below documents that
// explicitly passing the default values is the same thing.)
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

    // Explicit-default arm: FlacEncoder(5)/Mp3Encoder(0) must equal the
    // argument-free construction — documents the config-default pin.
    {
        FlacEncoder flac(5);
        Mp3Encoder  mp3(0);
        bool ok = flac.open("seam_exp.flac", kFrames) && mp3.open("seam_exp.mp3", kFrames);
        for (auto& blk : blocks) {
            if (!ok) break;
            ok = flac.writeFrames(blk.p, (size_t)blk.samples) &&
                 mp3.writeFrames(blk.p, (size_t)blk.samples);
        }
        flac.finalize(ok);
        mp3.finalize(ok);
        check(ok, "explicit-default arm (level 5 / V0) encoded");
    }

    auto rf = slurp("seam_ref.flac"), nf = slurp("seam_new.flac");
    auto rm = slurp("seam_ref.mp3"),  nm = slurp("seam_new.mp3");
    auto ef = slurp("seam_exp.flac"), em = slurp("seam_exp.mp3");
    std::printf("  flac: ref=%zu bytes, seam=%zu bytes\n", rf.size(), nf.size());
    std::printf("  mp3:  ref=%zu bytes, seam=%zu bytes\n", rm.size(), nm.size());
    check(!rf.empty() && rf == nf, "FLAC output BYTE-IDENTICAL to inline");
    check(!rm.empty() && rm == nm, "MP3 output BYTE-IDENTICAL to inline");
    check(ef == rf, "FLAC explicit level 5 == default == inline");
    check(em == rm, "MP3 explicit V0 == default == inline");

    std::remove("seam_ref.flac"); std::remove("seam_new.flac"); std::remove("seam_exp.flac");
    std::remove("seam_ref.mp3");  std::remove("seam_new.mp3");  std::remove("seam_exp.mp3");

    // ── MP3 CBR branch (encoder-bitrate-mode) ──────────────────────────────
    // CBR is strictly ADDITIVE: the VBR arms above stay byte-identical. There
    // is no byte-oracle for a lossy CBR stream, so this asserts the structural
    // facts that distinguish the branch: it encodes, it differs from the VBR
    // output, it starts on an MPEG frame sync, and it carries NO Xing/Info
    // duration frame (CBR does not set bWriteVbrTag, and finalize skips the tag
    // rewrite - a Xing/Info frame would be a VBR leak into the CBR path).
    {
        Mp3Encoder mp3(0, /*cbr=*/true, /*cbr_bitrate_bps=*/256000);
        bool ok = mp3.open("seam_cbr.mp3", kFrames);
        for (auto& blk : blocks) {
            if (!ok) break;
            ok = mp3.writeFrames(blk.p, (size_t)blk.samples);
        }
        mp3.finalize(ok);
        check(ok, "MP3 CBR arm encoded");
        auto cbr = slurp("seam_cbr.mp3");
        check(!cbr.empty(), "MP3 CBR output non-empty");
        check(!rm.empty() && cbr != rm, "MP3 CBR output differs from VBR");
        auto has_magic = [&](const char* m) {
            size_t n = cbr.size() < 512 ? cbr.size() : 512;
            for (size_t i = 0; i + 4 <= n; ++i)
                if (std::memcmp(cbr.data() + i, m, 4) == 0) return true;
            return false;
        };
        check(!has_magic("Xing") && !has_magic("Info"),
              "MP3 CBR head carries no Xing/Info duration frame");
        check(cbr.size() >= 2 && cbr[0] == 0xFF && (cbr[1] & 0xE0) == 0xE0,
              "MP3 CBR starts with an MPEG frame sync");
        std::remove("seam_cbr.mp3");
    }

    // ── WAV bit-exact oracle (rip-wav-encoder) ─────────────────────────────
    // WAV is lossless AND uncompressed, so the whole output is machine-
    // checkable: canonical header fields + data chunk == input PCM verbatim.
    {
        WavEncoder wav;
        bool ok = wav.open("seam_wav.wav", kFrames);
        for (auto& blk : blocks) {
            if (!ok) break;
            ok = wav.writeFrames(blk.p, (size_t)blk.samples);
        }
        wav.finalize(ok);
        check(ok, "WAV arm encoded");

        auto wf = slurp("seam_wav.wav");
        const uint32_t data_bytes = (uint32_t)(kFrames * 4);
        check(wf.size() == 44 + (size_t)data_bytes, "WAV file size == 44 + data");
        auto u16 = [&](size_t o) { return (uint32_t)wf[o] | ((uint32_t)wf[o+1] << 8); };
        auto u32 = [&](size_t o) { return u16(o) | (u16(o+2) << 16); };
        bool hdr =
            std::memcmp(wf.data(),      "RIFF", 4) == 0 &&
            u32(4)  == 36 + data_bytes &&
            std::memcmp(wf.data() + 8,  "WAVE", 4) == 0 &&
            std::memcmp(wf.data() + 12, "fmt ", 4) == 0 &&
            u32(16) == 16 && u16(20) == 1 && u16(22) == 2 &&
            u32(24) == 44100 && u32(28) == 176400 &&
            u16(32) == 4 && u16(34) == 16 &&
            std::memcmp(wf.data() + 36, "data", 4) == 0 &&
            u32(40) == data_bytes;
        check(hdr, "WAV header canonical (all 11 fields)");
        check(wf.size() >= 44 &&
              std::memcmp(wf.data() + 44, pcm.data(), (size_t)data_bytes) == 0,
              "WAV data chunk == input PCM byte-for-byte");
        std::remove("seam_wav.wav");
    }

#ifndef _WIN32
    // ── WAV strict-short-write contract (Linux: /dev/full forces ENOSPC) ──
    // A lenient write + the finalize back-patch would turn disk-full into a
    // valid-looking truncated file marked success; strict writeFrames makes
    // the track abort so the caller's cleanup removes the partial instead.
    {
        WavEncoder wav;
        bool opened = wav.open("/dev/full", 588);
        if (opened) {
            int16_t buf[588 * 2] = {};
            // fwrite may buffer; the failure must surface by the time the
            // stdio buffer flushes — write enough to force it through.
            bool w = true;
            for (int i = 0; i < 64 && w; ++i) w = wav.writeFrames(buf, 588);
            check(!w, "WAV short write returns false (ENOSPC surfaces)");
            wav.finalize(false);
        } else {
            check(true, "WAV /dev/full open refused (also acceptable)");
        }
    }
#endif

    // ── WavPack bit-exact round-trip (rip-wavpack-encoder) ─────────────────
    // WavPack COMPRESSES, so "lossless" must be demonstrated, not assumed:
    // encode the fixture, decode through the SHIPPING WavPackDecoder, and
    // require bit-identity. This also pins the 16-bit CD-DA config — a wrong
    // bits/bytes value cannot survive it.
    {
        WavPackEncoder wv;   // default mode (normal) — the named constant
        bool ok = wv.open("seam_wv.wv", kFrames);
        for (auto& blk : blocks) {
            if (!ok) break;
            ok = wv.writeFrames(blk.p, (size_t)blk.samples);
        }
        ok = wv.finalize(ok) && ok;
        check(ok, "WavPack arm encoded (finalize completed)");

        ma_data_source* ds = nullptr;
        ma_decoding_backend_config bc = ma_decoding_backend_config_init(ma_format_unknown, 0);
        ma_result r = ma_wavpack_backend_vtable()->onInitFile(
            nullptr, "seam_wv.wv", &bc, nullptr, &ds);
        check(r == MA_SUCCESS, "shipping WavPackDecoder opens the rip output");
        if (r == MA_SUCCESS) {
            // The decoder emits f32 = int/2^15 for 16-bit sources; the input
            // s16 scaled by 1/32768 is exactly representable — compare bits.
            std::vector<float> dec(kFrames * 2);
            ma_uint64 got = 0;
            ma_data_source_read_pcm_frames(ds, dec.data(), kFrames, &got);
            bool bit_exact = (got == kFrames);
            for (size_t i = 0; bit_exact && i < kFrames * 2; ++i)
                if (dec[i] != (float)pcm[i] / 32768.0f) bit_exact = false;
            check(got == kFrames, "WavPack round-trip frame count exact");
            check(bit_exact, "WavPack round-trip BIT-EXACT through the decoder");
            ma_wavpack_backend_vtable()->onUninit(nullptr, ds, nullptr);
        }
        std::remove("seam_wv.wv");
    }

#ifndef _WIN32
    // ── WavPack strict-block-callback chain (Linux: /dev/full ENOSPC) ─────
    // The WAV laundering trap one level deeper: libwavpack writes through a
    // BLOCK callback, and the finalize insurance is blind to write failures
    // (sample index counts SUBMITTED samples). The whole safety is: strict
    // callback -> propagated returns (probe-proven through PackSamples AND
    // FlushSamples) -> writeFrames/finalize false -> track aborts. Prove the
    // chain end-to-end under a real ENOSPC.
    {
        WavPackEncoder wv;
        bool opened = wv.open("/dev/full", 44100 * 4);
        if (opened) {
            std::vector<int16_t> buf((size_t)44100 * 2, 12345);
            bool all_writes_ok = true;
            for (int s = 0; s < 4 && all_writes_ok; ++s)
                all_writes_ok = wv.writeFrames(buf.data(), 44100);
            bool fin_ok = wv.finalize(all_writes_ok);
            check(!(all_writes_ok && fin_ok),
                  "WavPack ENOSPC cannot yield success (write or finalize fails)");
        } else {
            check(true, "WavPack /dev/full open refused (also acceptable)");
        }
    }
#endif

    // ── R128 <-> RG round trip (rip-opus-encoder) ──────────────────────────
    // The encode direction (tagFile) must be the EXACT inverse of the decode
    // direction (LocalFileSource) for every representable Q7.8 tag value —
    // /256 and +/-5 are exact in binary floating point over the int range,
    // so this is a machine proof, not a tolerance check. Full range.
    {
        bool rt = true;
        for (int q = -32768; q <= 32767; ++q)
            if (r128FromDb((double)dbFromR128(q)) != q) { rt = false; break; }
        check(rt, "R128 round trip exact over full Q7.8 range");
        check(dbFromR128(-2560) == -5.0f, "R128 -2560 -> -5.00 dB (decode-flight vector)");
        check(r128FromDb(-5.0)  == -2560, "R128 -5.00 dB -> -2560 (encode inverse)");
        check(r128FromDb(0.0)   == -1280, "RG 0 dB -> Q7.8 -1280 (the -5 rebase, sign right)");
    }

    if (failures) { std::printf("%d FAILURE(S)\n", failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
