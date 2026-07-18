// aac_m4a_probe.cpp — PHASE-0 feasibility probe for AAC/M4A ripping.
// Scope ID: aac-m4a-phase0-probe. NOT in the production build.
//
// Proves the two unknowns the Phase 1 encoder slice rests on:
//   Probe 1  FDK-AAC *encoder* present + linkable, AAC-LC, VBR (1-5) and CBR both
//            applying via AACENC_BITRATEMODE, same libfdk-aac already shipped.
//   Probe 2  A hand-rolled MP4 mux (Mp4Mux.h, no new dep) assembles FDK's raw
//            access units into a valid .m4a that RE-MOCT's OWN AacDecoder opens,
//            reports the right length, and SEEKS correctly; then TagLib writes
//            tags + a covr atom that read back, without breaking the sample table.
//
// Verification uses the real production decode path: src/AacDecoder.cpp registered
// as a miniaudio custom backend, driven by ma_decoder — exactly how playback opens
// a file. If this opens + seeks, RE-MOCT plays it.
//
// The distinct-tone-per-third signal (440 / 880 / 1760 Hz) makes the seek check
// meaningful: seek to 50% must land in the 880 Hz region.

#include "Mp4Mux.h"

#include "miniaudio.h"
#include "AacDecoder.h"

#include <fdk-aac/aacenc_lib.h>

#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4item.h>
#include <taglib/mp4coverart.h>
#include <taglib/tstring.h>
#include <taglib/tbytevector.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " -> ", detail.c_str());
    if (ok) ++g_pass; else ++g_fail;
}

const double PI = 3.14159265358979323846;

// tone for the frequency-stepped test signal
double toneFreq(double t, double seconds) {
    if (t < seconds / 3.0)       return 440.0;
    if (t < 2.0 * seconds / 3.0) return 880.0;
    return 1760.0;
}

struct Encoded {
    std::vector<uint8_t>              asc;
    std::vector<std::vector<uint8_t>> aus;
    uint32_t                          frameLength = 1024;
    uint32_t                          avgBitrate  = 0;
    bool                              ok = false;
    std::string                       err;
};

// FDK encode -> raw AAC access units (TT_MP4_RAW) + the ASC from aacEncInfo.
Encoded encodeAac(bool vbr, int vbrMode, int cbrBitrate,
                  uint32_t sr, uint32_t ch, double seconds) {
    Encoded e;
    HANDLE_AACENCODER enc = nullptr;
    if (aacEncOpen(&enc, 0, ch) != AACENC_OK) { e.err = "aacEncOpen failed"; return e; }

    auto set = [&](AACENC_PARAM p, UINT v) { return aacEncoder_SetParam(enc, p, v) == AACENC_OK; };
    bool okp = true;
    okp &= set(AACENC_AOT, 2);                          // AAC-LC
    okp &= set(AACENC_SAMPLERATE, sr);
    okp &= set(AACENC_CHANNELMODE, ch == 2 ? MODE_2 : MODE_1);
    okp &= set(AACENC_TRANSMUX, TT_MP4_RAW);            // raw access units (we mux)
    if (vbr) {
        okp &= set(AACENC_BITRATEMODE, (UINT)vbrMode);  // 1..5 VBR quality ladder
    } else {
        okp &= set(AACENC_BITRATEMODE, 0);              // CBR
        okp &= set(AACENC_BITRATE, (UINT)cbrBitrate);
    }
    if (!okp) { e.err = "SetParam rejected (VBR/CBR mode may not apply)"; aacEncClose(&enc); return e; }

    if (aacEncEncode(enc, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
        e.err = "aacEncEncode(init) failed"; aacEncClose(&enc); return e;
    }
    AACENC_InfoStruct info{};
    if (aacEncInfo(enc, &info) != AACENC_OK) { e.err = "aacEncInfo failed"; aacEncClose(&enc); return e; }
    e.frameLength = (uint32_t)info.frameLength;         // 1024 for LC
    e.asc.assign(info.confBuf, info.confBuf + info.confSize);

    const int frame = (int)e.frameLength;
    const int total = (int)(seconds * sr);
    std::vector<INT_PCM> pcm((size_t)frame * ch);
    std::vector<uint8_t> obuf(16384);
    size_t totalBytes = 0;

    auto encodeOne = [&](INT numInSamples, void* inPtr) -> bool {
        INT   inId = IN_AUDIO_DATA, inSize = numInSamples > 0 ? numInSamples * (INT)sizeof(INT_PCM) : 0, inEl = sizeof(INT_PCM);
        void* outPtr = obuf.data();
        INT   outId = OUT_BITSTREAM_DATA, outSize = (INT)obuf.size(), outEl = 1;
        AACENC_BufDesc inDesc{};  inDesc.numBufs = 1; inDesc.bufs = &inPtr;
        inDesc.bufferIdentifiers = &inId; inDesc.bufSizes = &inSize; inDesc.bufElSizes = &inEl;
        AACENC_BufDesc outDesc{}; outDesc.numBufs = 1; outDesc.bufs = &outPtr;
        outDesc.bufferIdentifiers = &outId; outDesc.bufSizes = &outSize; outDesc.bufElSizes = &outEl;
        AACENC_InArgs inArgs{};  inArgs.numInSamples = numInSamples;
        AACENC_OutArgs outArgs{};
        AACENC_ERROR r = aacEncEncode(enc, &inDesc, &outDesc, &inArgs, &outArgs);
        if (numInSamples < 0 && r == AACENC_ENCODE_EOF && outArgs.numOutBytes == 0) return false; // drained
        if (r != AACENC_OK && r != AACENC_ENCODE_EOF) return false;
        if (outArgs.numOutBytes > 0) {
            e.aus.emplace_back(obuf.data(), obuf.data() + outArgs.numOutBytes);
            totalBytes += (size_t)outArgs.numOutBytes;
        }
        return true;
    };

    int pos = 0;
    while (pos < total) {
        for (int i = 0; i < frame; ++i) {
            double t = (double)(pos + i) / (double)sr;
            double a = 0.6 * std::sin(2.0 * PI * toneFreq(t, seconds) * t);
            INT_PCM s = (INT_PCM)(a * 32767.0);
            for (uint32_t c = 0; c < ch; ++c) pcm[(size_t)i * ch + c] = s;
        }
        void* inPtr = pcm.data();
        if (!encodeOne((INT)(frame * ch), inPtr)) break;
        pos += frame;
    }
    // flush
    for (int guard = 0; guard < 64; ++guard) { if (!encodeOne(-1, nullptr)) break; }
    aacEncClose(&enc);

    e.avgBitrate = seconds > 0 ? (uint32_t)(totalBytes * 8 / seconds) : 128000;
    e.ok = !e.asc.empty() && !e.aus.empty();
    if (!e.ok && e.err.empty()) e.err = "no ASC or no access units produced";
    return e;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t w = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return w == bytes.size();
}

// Decode the whole file through RE-MOCT's AacDecoder (miniaudio custom backend),
// returning RMS, length-in-frames, channels, rate, and the dominant tone at 50%.
struct Decoded {
    bool     opened = false;
    uint64_t lengthFrames = 0;
    uint32_t channels = 0, rate = 0;
    double   rms = 0.0;
    uint64_t framesRead = 0;
    // dominant tone (best of 440/880/1760) at seek points 25% / 50% / 75%
    double   toneAt25 = 0, toneAt50 = 0, toneAt75 = 0;
};

// Goertzel power of a mono signal at frequency f — robust to the quantization
// noise a zero-crossing counter trips over on decoded AAC.
double goertzel(const std::vector<double>& x, double f, double rate) {
    double w = 2.0 * PI * f / rate, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (double v : x) { double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

// Classify a decoded chunk to whichever of {440,880,1760} has the most energy.
double classifyTone(const std::vector<int16_t>& interleaved, uint32_t ch, uint32_t rate) {
    if (interleaved.size() < ch * 8 || rate == 0) return 0.0;
    std::vector<double> mono(interleaved.size() / ch);
    for (size_t i = 0; i < mono.size(); ++i) {
        double a = 0; for (uint32_t c = 0; c < ch; ++c) a += interleaved[i * ch + c];
        mono[i] = a / ch;
    }
    const double cand[3] = { 440.0, 880.0, 1760.0 };
    double best = 0, bestP = -1;
    for (double f : cand) { double p = goertzel(mono, f, rate); if (p > bestP) { bestP = p; best = f; } }
    return best;
}

double toneAtFraction(ma_decoder* dec, uint64_t total, double frac, uint32_t ch, uint32_t rate) {
    const ma_uint64 CHUNK = 8192;
    ma_decoder_seek_to_pcm_frame(dec, (ma_uint64)(total * frac));
    std::vector<int16_t> buf((size_t)CHUNK * ch);
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(dec, buf.data(), CHUNK, &got);
    buf.resize((size_t)got * ch);
    return classifyTone(buf, ch, rate);
}

Decoded decodeVerify(const std::string& path) {
    Decoded d;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoding_backend_vtable* vt[] = { ma_aac_backend_vtable() };
    cfg.ppCustomBackendVTables = vt;
    cfg.customBackendCount     = 1;
    cfg.pCustomBackendUserData = nullptr;

    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return d;
    d.opened = true;
    d.channels = dec.outputChannels;
    d.rate     = dec.outputSampleRate;
    ma_uint64 lenFrames = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &lenFrames);
    d.lengthFrames = lenFrames;

    // Read everything, accumulate RMS.
    const ma_uint64 CHUNK = 4096;
    std::vector<int16_t> buf((size_t)CHUNK * d.channels);
    double acc = 0.0; uint64_t n = 0;
    for (;;) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, buf.data(), CHUNK, &got) != MA_SUCCESS && got == 0) break;
        if (got == 0) break;
        for (ma_uint64 i = 0; i < got * d.channels; ++i) { double s = buf[(size_t)i]; acc += s * s; }
        n += got * d.channels;
    }
    d.framesRead = d.channels ? n / d.channels : 0;
    d.rms = n ? std::sqrt(acc / (double)n) : 0.0;

    // Seek to 25 / 50 / 75% — must track the frequency-stepped thirds exactly,
    // which is only possible if the sample table (stsc/stco/stsz/stts) is valid.
    uint64_t total = d.lengthFrames ? d.lengthFrames : d.framesRead;
    if (total > 0) {
        d.toneAt25 = toneAtFraction(&dec, total, 0.25, d.channels, d.rate);
        d.toneAt50 = toneAtFraction(&dec, total, 0.50, d.channels, d.rate);
        d.toneAt75 = toneAtFraction(&dec, total, 0.75, d.channels, d.rate);
    }
    ma_decoder_uninit(&dec);
    return d;
}

// A valid 1x1 PNG (red), for the covr atom round-trip.
const unsigned char kPng[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
    0xDE,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,
    0x00,0x00,0x03,0x01,0x01,0x00,0x18,0xDD,0x8D,0xB0,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4E,0x44,0xAE,0x42,0x60,0x82
};

bool tagWithArt(const std::string& path) {
    TagLib::MP4::File f(path.c_str(), false);
    if (!f.isValid() || !f.tag()) return false;
    f.tag()->setTitle("Probe Title");
    f.tag()->setArtist("Probe Artist");
    f.tag()->setAlbum("Probe Album");
    TagLib::MP4::CoverArt cover(TagLib::MP4::CoverArt::PNG,
                                TagLib::ByteVector((const char*)kPng, (unsigned)sizeof(kPng)));
    TagLib::MP4::CoverArtList list; list.append(cover);
    f.tag()->setItem("covr", TagLib::MP4::Item(list));
    return f.save();
}

bool readBackTags(const std::string& path, std::string& title, size_t& covrBytes, std::string& covrMime) {
    TagLib::MP4::File f(path.c_str(), false);
    if (!f.isValid() || !f.tag()) return false;
    title = f.tag()->title().to8Bit(true);
    if (!f.tag()->contains("covr")) return false;
    TagLib::MP4::CoverArtList covers = f.tag()->item("covr").toCoverArtList();
    if (covers.isEmpty()) return false;
    const TagLib::MP4::CoverArt& c = covers.front();
    covrBytes = c.data().size();
    covrMime  = (c.format() == TagLib::MP4::CoverArt::PNG) ? "image/png" : "image/jpeg";
    return true;
}

void runOne(const char* label, bool vbr, int vbrMode, int cbrBitrate,
            uint32_t sr, uint32_t ch, double seconds, const std::string& outPath) {
    std::printf("\n=== %s -> %s ===\n", label, outPath.c_str());

    Encoded enc = encodeAac(vbr, vbrMode, cbrBitrate, sr, ch, seconds);
    check(enc.ok, "FDK encode (raw AAC access units + ASC)",
          enc.ok ? (std::to_string(enc.aus.size()) + " AUs, ASC " +
                    std::to_string(enc.asc.size()) + "B, ~" +
                    std::to_string(enc.avgBitrate / 1000) + " kbps") : enc.err);
    if (!enc.ok) return;

    mp4mux::Track t;
    t.asc = enc.asc; t.sampleRate = sr; t.channels = ch;
    t.frameLength = enc.frameLength; t.aus = enc.aus; t.avgBitrate = enc.avgBitrate;
    std::vector<uint8_t> m4a = mp4mux::muxM4a(t);
    check(!m4a.empty(), "hand-rolled MP4 mux produced bytes",
          m4a.empty() ? "" : (std::to_string(m4a.size()) + " bytes"));
    if (m4a.empty()) return;
    check(writeFile(outPath, m4a), "wrote .m4a");

    double expSec = (double)enc.aus.size() * enc.frameLength / (double)sr;

    // Verify BEFORE tagging.
    Decoded d = decodeVerify(outPath);
    check(d.opened, "RE-MOCT AacDecoder opened the file (pre-tag)");
    if (d.opened) {
        double gotSec = d.lengthFrames ? (double)d.lengthFrames / d.rate : 0.0;
        check(std::fabs(gotSec - expSec) < 0.25,
              "reported length matches encoded duration",
              "got " + std::to_string(gotSec) + "s vs ~" + std::to_string(expSec) + "s");
        check(d.rms > 1000.0, "decoded audio is non-silent", "RMS " + std::to_string((int)d.rms));
        check(d.rate == sr && d.channels == ch, "format matches",
              std::to_string(d.rate) + "Hz " + std::to_string(d.channels) + "ch");
        bool seekOk = d.toneAt25 == 440.0 && d.toneAt50 == 880.0 && d.toneAt75 == 1760.0;
        check(seekOk, "seek to 25/50/75% tracks the 440/880/1760 thirds (sample table valid)",
              std::to_string((int)d.toneAt25) + "/" + std::to_string((int)d.toneAt50) + "/" +
              std::to_string((int)d.toneAt75) + " Hz");
    }

    // Tag + cover art, then verify decode + seek STILL work and tags read back.
    check(tagWithArt(outPath), "TagLib wrote title/artist/album + covr (PNG)");
    std::string title, mime; size_t covr = 0;
    bool rb = readBackTags(outPath, title, covr, mime);
    check(rb && title == "Probe Title" && covr == sizeof(kPng),
          "tags + covr read back",
          rb ? ("title='" + title + "', covr " + std::to_string(covr) + "B " + mime) : "read failed");

    Decoded d2 = decodeVerify(outPath);
    check(d2.opened, "AacDecoder still opens after tagging (mdat/stco intact)");
    if (d2.opened)
        check(d2.toneAt25 == 440.0 && d2.toneAt50 == 880.0 && d2.toneAt75 == 1760.0,
              "seek still correct after tagging",
              std::to_string((int)d2.toneAt25) + "/" + std::to_string((int)d2.toneAt50) + "/" +
              std::to_string((int)d2.toneAt75) + " Hz");
}

} // namespace

int main() {
    std::printf("aac-m4a-phase0-probe\n");
#if defined(_WIN32)
    std::printf("toolchain: Windows / UCRT64\n");
#else
    std::printf("toolchain: Linux\n");
#endif

    runOne("VBR level 4 (default)", true, 4, 0, 44100, 2, 6.0, "out_vbr4.m4a");
    runOne("CBR 128 kbps ([M] alt)", false, 0, 128000, 44100, 2, 6.0, "out_cbr128.m4a");

    std::printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
