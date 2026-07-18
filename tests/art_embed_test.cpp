// art_embed_test - embedded-art carryover + text-tag (RG/AR) completeness.
//
//  A. Writer/reader round-trip per encodable output format, for BOTH a JPEG and
//     a PNG blob: embedArt then extractEmbeddedArt returns the same bytes+MIME.
//  B. Convert carries art end to end (reader -> writer through convertOne), JPEG
//     and PNG, across a format boundary.
//  C. A no-art source converts fine and yields no output picture (best-effort).
//  D. Text-tag completeness: RG + AccurateRip survive convertOne carryover on
//     FLAC->MP3 and MP3->Opus (pins Finding A - PropertyMap does NOT drop them).
//
// Blobs are opaque byte payloads with real JPEG/PNG magic - TagLib stores them
// verbatim, so a genuine raster is unnecessary for a round-trip.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ArtEmbed.h"
#include "ConvertJob.h"
#include "EncoderFactory.h"
#include "StringUtils.h"   // utf8_to_wide

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

static int failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("%-58s %s\n", what.c_str(), cond ? "OK" : "FAIL");
    if (!cond) ++failures;
}

static std::vector<int16_t> pcmFixture() {
    std::vector<int16_t> p(44100 * 2);
    uint32_t x = 0x41525421u;
    for (auto& s : p) { x = x * 1103515245u + 12345u; s = (int16_t)(x >> 16); }
    return p;
}

static bool synth(const std::string& path, RipFormat fmt) {
    auto pcm = pcmFixture();
    auto enc = makeEncoder(fmt, RipOptions{});
    if (!enc || !enc->open(path, pcm.size() / 2)) return false;
    size_t off = 0, frames = pcm.size() / 2; bool ok = true;
    while (off < frames && ok) {
        size_t n = frames - off < 4096 ? frames - off : 4096;
        ok = enc->writeFrames(pcm.data() + off * 2, n); off += n;
    }
    return enc->finalize(ok) && ok;
}

static std::vector<uint8_t> jpegBlob() {
    std::vector<uint8_t> b = { 0xFF,0xD8,0xFF,0xE0,0x00,0x10,'J','F','I','F' };
    for (int i = 0; i < 300; ++i) b.push_back((uint8_t)(i * 7 + 3));
    b.push_back(0xFF); b.push_back(0xD9);
    return b;
}
static std::vector<uint8_t> pngBlob() {
    std::vector<uint8_t> b = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    for (int i = 0; i < 300; ++i) b.push_back((uint8_t)(i * 5 + 11));
    return b;
}

static bool hasProp(const std::string& path, const char* key) {
    TagLib::FileRef f(TL_PATH(path), false);
    if (f.isNull() || !f.file()) return false;
    auto pm = f.file()->properties();
    return pm.contains(key) && !pm[key].isEmpty();
}

static void writeXiphRgAr(const std::string& flac) {
    TagLib::FLAC::File f(TL_PATH(flac), false);
    auto* x = f.xiphComment(true); if (!x) return;
    x->addField("REPLAYGAIN_TRACK_GAIN", "-6.50 dB", true);
    x->addField("ACCURATERIP", "AccurateRip v2 (confidence 200)", true);
    f.save();
}
static void writeId3RgAr(const std::string& mp3) {
    TagLib::MPEG::File f(TL_PATH(mp3), false);
    auto* t = f.ID3v2Tag(true); if (!t) return;
    auto add = [&](const char* k, const char* v) {
        auto* fr = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
        fr->setDescription(k); fr->setText(v); t->addFrame(fr);
    };
    add("REPLAYGAIN_TRACK_GAIN", "-6.50 dB");
    add("ACCURATERIP", "AccurateRip v2 (confidence 200)");
    f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
}

int main() {
    fs::path dir = fs::temp_directory_path() / "remoct_art_test";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    auto P = [&](const std::string& n) { return (dir / n).string(); };

    struct Fmt { RipFormat fmt; const char* ext; const char* name; };
    Fmt fmts[] = {
        { RipFormat::Flac, ".flac", "FLAC" }, { RipFormat::Mp3, ".mp3", "MP3" },
        { RipFormat::Opus, ".opus", "Opus" }, { RipFormat::WavPack, ".wv", "WavPack" },
    };
    struct Img { std::vector<uint8_t> bytes; std::string mime; const char* name; };
    Img imgs[] = { { jpegBlob(), "image/jpeg", "JPEG" }, { pngBlob(), "image/png", "PNG" } };

    // ── A. writer/reader round-trip, each format x {JPEG, PNG} ─────────────
    for (auto& fm : fmts) {
        for (auto& im : imgs) {
            std::string p = P(std::string("rt_") + fm.name + "_" + im.name + fm.ext);
            synth(p, fm.fmt);
            ArtBlob blob{ im.bytes, im.mime };
            bool w = embedArt(p, fm.fmt, blob);
            auto got = extractEmbeddedArt(p);
            bool ok = w && got.has_value() && got->bytes == im.bytes && got->mime == im.mime;
            check(ok, std::string("round-trip ") + fm.name + " / " + im.name);
        }
    }

    // WAV: writer is a no-op, reader yields nothing.
    { std::string wav = P("plain.wav"); synth(wav, RipFormat::Wav);
      check(!embedArt(wav, RipFormat::Wav, ArtBlob{ jpegBlob(), "image/jpeg" }) &&
            !extractEmbeddedArt(wav).has_value(), "WAV has no embedded-art surface"); }

    // ── B. convert carries art across a boundary (JPEG + PNG) ──────────────
    {
        std::string src = P("art_src.flac"); synth(src, RipFormat::Flac);
        embedArt(src, RipFormat::Flac, ArtBlob{ jpegBlob(), "image/jpeg" });
        std::string out = P("art_out.opus");
        auto r = ConvertJob::convertOne(src, out, RipFormat::Opus, RipOptions{});
        auto got = extractEmbeddedArt(out);
        check(r == ConvertJob::Result::Converted && got && got->bytes == jpegBlob(),
              "convert FLAC(JPEG art) -> Opus carries the picture");
    }
    {
        std::string src = P("art_src.mp3"); synth(src, RipFormat::Mp3);
        embedArt(src, RipFormat::Mp3, ArtBlob{ pngBlob(), "image/png" });
        std::string out = P("art_out.flac");
        auto r = ConvertJob::convertOne(src, out, RipFormat::Flac, RipOptions{});
        auto got = extractEmbeddedArt(out);
        check(r == ConvertJob::Result::Converted && got && got->bytes == pngBlob() &&
              got->mime == "image/png", "convert MP3(PNG art) -> FLAC carries the picture");
    }

    // ── C. no-art source converts fine, output has no picture ──────────────
    {
        std::string src = P("noart.flac"); synth(src, RipFormat::Flac);
        std::string out = P("noart_out.mp3");
        auto r = ConvertJob::convertOne(src, out, RipFormat::Mp3, RipOptions{});
        check(r == ConvertJob::Result::Converted && !extractEmbeddedArt(out).has_value(),
              "no-art source converts, output has no picture");
    }

    // ── D. RG/AR text-tag completeness through convertOne (Finding A) ──────
    {
        std::string src = P("rg_src.flac"); synth(src, RipFormat::Flac);
        writeXiphRgAr(src);
        std::string out = P("rg_out.mp3");
        auto r = ConvertJob::convertOne(src, out, RipFormat::Mp3, RipOptions{});
        check(r == ConvertJob::Result::Converted &&
              hasProp(out, "REPLAYGAIN_TRACK_GAIN") && hasProp(out, "ACCURATERIP"),
              "FLAC -> MP3 carries ReplayGain + AccurateRip");
    }
    {
        std::string src = P("rg_src.mp3"); synth(src, RipFormat::Mp3);
        writeId3RgAr(src);
        std::string out = P("rg_out.opus");
        auto r = ConvertJob::convertOne(src, out, RipFormat::Opus, RipOptions{});
        check(r == ConvertJob::Result::Converted &&
              hasProp(out, "REPLAYGAIN_TRACK_GAIN") && hasProp(out, "ACCURATERIP"),
              "MP3 -> Opus carries ReplayGain + AccurateRip");
    }

    fs::remove_all(dir, ec);
    std::printf("\n%s (%d failure%s)\n",
                failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
