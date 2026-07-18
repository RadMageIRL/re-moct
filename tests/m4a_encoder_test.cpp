// m4a_encoder_test — the M4aEncoder + Mp4Mux contract (aac-m4a-encoder slice),
// headless on both toolchains.
//
// Encodes a frequency-stepped PCM fixture (440/880/1760 Hz thirds) through
// M4aEncoder in VBR and CBR, then opens the .m4a via the SHIPPING decode stack
// (AacDecoder registered through remoct_custom_backends, driven by ma_decoder -
// exactly how playback opens a file) and asserts:
//   * it opens and reports the right length,
//   * seek to 25/50/75% tracks the tone thirds (the sample-table validity gate),
//   * tags + a covr atom + a plain REPLAYGAIN_* freeform atom round-trip, and the
//     RG reads back through the generic PropertyMap the decode side uses (plan F3),
//   * finalize(true) returns FALSE when the container cannot be written (the
//     buffering-encoder ENOSPC contract - IEncoder::finalize returns completion).
//
// This is the phase-0 probe's verification, re-pointed at the production encoder.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "M4aEncoder.h"
#include "CustomBackends.h"

#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4item.h>
#include <taglib/mp4coverart.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
static void check(bool ok, const char* what, const std::string& d = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                d.empty() ? "" : " -> ", d.c_str());
    if (!ok) ++g_fail;
}

static const double PI = 3.14159265358979323846;

static double toneFreq(double t, double secs) {
    if (t < secs / 3.0)       return 440.0;
    if (t < 2.0 * secs / 3.0) return 880.0;
    return 1760.0;
}

// Frequency-stepped s16 stereo fixture.
static std::vector<int16_t> makeFixture(uint32_t sr, double secs) {
    size_t frames = (size_t)(sr * secs);
    std::vector<int16_t> pcm(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        double t = (double)i / sr;
        double a = 0.6 * std::sin(2.0 * PI * toneFreq(t, secs) * t);
        int16_t s = (int16_t)(a * 32767.0);
        pcm[i * 2] = s; pcm[i * 2 + 1] = s;
    }
    return pcm;
}

static double goertzel(const std::vector<double>& x, double f, double rate) {
    double w = 2.0 * PI * f / rate, c = 2.0 * std::cos(w), s1 = 0, s2 = 0;
    for (double v : x) { double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}
static double classifyTone(const std::vector<int16_t>& il, uint32_t ch, uint32_t rate) {
    if (il.size() < ch * 8 || rate == 0) return 0.0;
    std::vector<double> mono(il.size() / ch);
    for (size_t i = 0; i < mono.size(); ++i) {
        double a = 0; for (uint32_t c = 0; c < ch; ++c) a += il[i * ch + c];
        mono[i] = a / ch;
    }
    const double cand[3] = { 440.0, 880.0, 1760.0 };
    double best = 0, bestP = -1;
    for (double f : cand) { double p = goertzel(mono, f, rate); if (p > bestP) { bestP = p; best = f; } }
    return best;
}

// A valid 1x1 red PNG for the covr round-trip.
static const unsigned char kPng[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
    0xDE,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,
    0x00,0x00,0x03,0x01,0x01,0x00,0x18,0xDD,0x8D,0xB0,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4E,0x44,0xAE,0x42,0x60,0x82
};

struct Decoded {
    bool opened = false; uint64_t lengthFrames = 0; uint32_t ch = 0, rate = 0;
    double rms = 0; double t25 = 0, t50 = 0, t75 = 0;
};

static double toneAt(ma_decoder* dec, uint64_t total, double frac, uint32_t ch, uint32_t rate) {
    const ma_uint64 N = 8192;
    ma_decoder_seek_to_pcm_frame(dec, (ma_uint64)(total * frac));
    std::vector<int16_t> buf((size_t)N * ch);
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(dec, buf.data(), N, &got);
    buf.resize((size_t)got * ch);
    return classifyTone(buf, ch, rate);
}

static Decoded decodeVerify(const std::string& path) {
    Decoded d;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    size_t nb = 0;
    ma_decoding_backend_vtable** vts = remoct_custom_backends(&nb);
    cfg.ppCustomBackendVTables = vts;
    cfg.customBackendCount     = (ma_uint32)nb;
    cfg.pCustomBackendUserData = nullptr;

    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return d;
    d.opened = true;
    d.ch = dec.outputChannels; d.rate = dec.outputSampleRate;
    ma_uint64 len = 0; ma_decoder_get_length_in_pcm_frames(&dec, &len);
    d.lengthFrames = len;

    const ma_uint64 N = 4096;
    std::vector<int16_t> buf((size_t)N * d.ch);
    double acc = 0; uint64_t n = 0;
    for (;;) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, buf.data(), N, &got) != MA_SUCCESS && got == 0) break;
        if (got == 0) break;
        for (ma_uint64 i = 0; i < got * d.ch; ++i) { double s = buf[(size_t)i]; acc += s * s; }
        n += got * d.ch;
    }
    d.rms = n ? std::sqrt(acc / (double)n) : 0.0;
    uint64_t total = d.lengthFrames ? d.lengthFrames : (d.ch ? n / d.ch : 0);
    if (total) {
        d.t25 = toneAt(&dec, total, 0.25, d.ch, d.rate);
        d.t50 = toneAt(&dec, total, 0.50, d.ch, d.rate);
        d.t75 = toneAt(&dec, total, 0.75, d.ch, d.rate);
    }
    ma_decoder_uninit(&dec);
    return d;
}

// Write tags/covr/RG the way CDRipper::tagFile's is_m4a branch does, then read
// them back the way LocalFileSource does (generic PropertyMap for the RG).
static bool tagAndReadBack(const std::string& path) {
    {
        TagLib::MP4::File f(path.c_str(), false);
        if (!f.isValid() || !f.tag()) return false;
        f.tag()->setTitle("M4A Probe");
        f.tag()->setItem("----:com.apple.iTunes:REPLAYGAIN_TRACK_GAIN",
                         TagLib::MP4::Item(TagLib::StringList("-6.92 dB")));
        TagLib::MP4::CoverArtList covers;
        covers.append(TagLib::MP4::CoverArt(TagLib::MP4::CoverArt::PNG,
            TagLib::ByteVector((const char*)kPng, (unsigned)sizeof(kPng))));
        f.tag()->setItem("covr", TagLib::MP4::Item(covers));
        if (!f.save()) return false;
    }
    TagLib::MP4::File f(path.c_str(), true);
    if (!f.tag()) return false;
    bool title_ok = f.tag()->title().to8Bit(true) == "M4A Probe";
    bool covr_ok  = f.tag()->contains("covr") &&
                    !f.tag()->item("covr").toCoverArtList().isEmpty();
    TagLib::PropertyMap props = f.properties();          // the LocalFileSource path
    bool rg_ok = props.contains("REPLAYGAIN_TRACK_GAIN") &&
                 !props["REPLAYGAIN_TRACK_GAIN"].isEmpty();
    return title_ok && covr_ok && rg_ok;
}

static void runOne(const char* label, bool vbr, int level, int cbr, const std::string& path) {
    std::printf("\n=== %s ===\n", label);
    const uint32_t sr = 44100; const double secs = 6.0;
    std::vector<int16_t> pcm = makeFixture(sr, secs);

    {
        M4aEncoder enc(vbr, level, cbr);
        bool o = enc.open(path, pcm.size() / 2);
        check(o, "M4aEncoder.open");
        // Feed in irregular chunks to exercise the residual staging.
        bool w = true;
        size_t framesTotal = pcm.size() / 2, pos = 0;
        while (pos < framesTotal) {
            size_t chunk = (pos % 3 == 0) ? 1000 : (pos % 3 == 1) ? 4096 : 777;
            if (pos + chunk > framesTotal) chunk = framesTotal - pos;
            w &= enc.writeFrames(pcm.data() + pos * 2, chunk);
            pos += chunk;
        }
        check(w, "M4aEncoder.writeFrames");
        check(enc.finalize(true), "M4aEncoder.finalize(true) reports a completed write");
    }

    Decoded d = decodeVerify(path);
    check(d.opened, "shipping AacDecoder stack opens the .m4a");
    if (d.opened) {
        double gotSec = d.lengthFrames && d.rate ? (double)d.lengthFrames / d.rate : 0.0;
        check(std::fabs(gotSec - secs) < 0.3, "length matches",
              std::to_string(gotSec) + "s vs ~6s");
        check(d.rms > 1000.0, "decoded audio non-silent", "RMS " + std::to_string((int)d.rms));
        check(d.ch == 2 && d.rate == sr, "format 44100/2ch");
        check(d.t25 == 440.0 && d.t50 == 880.0 && d.t75 == 1760.0,
              "seek 25/50/75% tracks the 440/880/1760 thirds (sample table valid)",
              std::to_string((int)d.t25) + "/" + std::to_string((int)d.t50) + "/" +
              std::to_string((int)d.t75) + " Hz");
    }
    check(tagAndReadBack(path),
          "tags + covr + plain ReplayGain (----) round-trip; RG reads via PropertyMap");

    Decoded d2 = decodeVerify(path);
    check(d2.opened && d2.t50 == 880.0, "still opens + seeks after tagging");
}

int main() {
    std::printf("m4a_encoder_test\n");
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "remoct_m4a_test";
    fs::create_directories(dir, ec);

    runOne("VBR level 4", true, 4, 0, (dir / "vbr.m4a").string());
    runOne("CBR 128k",    false, 0, 128000, (dir / "cbr.m4a").string());

    // ENOSPC/unwritable: finalize must report the container write did NOT complete.
    {
        std::printf("\n=== finalize(true) on an unwritable path ===\n");
        std::vector<int16_t> pcm = makeFixture(44100, 1.0);
        M4aEncoder enc(true, 4, 0);
        // A path inside a directory that does not exist -> fopen fails at mux write.
        std::string bad = (dir / "nope" / "sub" / "x.m4a").string();
        enc.open(bad, pcm.size() / 2);
        enc.writeFrames(pcm.data(), pcm.size() / 2);
        check(!enc.finalize(true), "finalize(true) returns false when the file cannot be written");
    }

    fs::remove_all(dir, ec);
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
