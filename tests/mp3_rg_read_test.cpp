// mp3_rg_read_test — the MP3 ReplayGain read contract (mp3-rg-read slice).
//
// Pins the decode-side read across ALL THREE tag shapes so the upcoming
// write-side correction (proper desc/value TXXX) cannot silently break
// read-back — the seam-oracle move applied to a tag dialect:
//   (a) the LEGACY RE-MOCT blob: a generic TXXX whose single text is
//       "REPLAYGAIN_TRACK_GAIN=-6.92 dB" (re-parses with the whole blob in
//       the frame DESCRIPTION, value empty — invisible to PropertyMap; the
//       shape every RE-MOCT MP3 rip/recording shipped with)
//   (b) the STANDARD shape: UserTextIdentificationFrame with description
//       REPLAYGAIN_TRACK_GAIN and the gain in the VALUE field (what other
//       rippers write; read via PropertyMap — must keep working)
//   (c) lowercase-description standard (case-insensitivity end-to-end)
//   (d) no RG tags at all -> 0.00 (no gain invented)
// The fixture MP3 is REAL — encoded by the production Mp3Encoder — and read
// through the REAL LocalFileSource::open() metadata path (not TagLib
// re-parsed), so the assert covers the exact code playback uses.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "LocalFileSource.h"
#include "Mp3Encoder.h"
#include "StringUtils.h"   // utf8_to_wide (TagLib::FileName is wide on Windows)

#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

// One real 0.5 s MP3 via the production encoder.
static bool writeMp3(const std::string& path) {
    Mp3Encoder enc;                              // V0, the rip default
    if (!enc.open(path, 0)) return false;
    std::vector<int16_t> pcm(22050 * 2);
    for (size_t f = 0; f < 22050; ++f) {
        int16_t s = (int16_t)(16384.0 * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                                 (double)f / 44100.0));
        pcm[f*2] = s; pcm[f*2+1] = s;
    }
    if (!enc.writeFrames(pcm.data(), 22050)) return false;
    return enc.finalize(true);
}

static float readBack(const std::string& path) {
    LocalFileSource s;
    if (!s.open(path)) { std::printf("FAIL open %s\n", path.c_str()); ++g_fail; return -999.0f; }
    return s.info().replaygain_db;
}

int main() {
    const std::string root = (fs::temp_directory_path() / "remoct_mp3rg_test").string();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    auto approx = [](float a, float b) { return std::fabs(a - b) < 0.01f; };

    // (a) legacy blob — CDRipper::tagFile's exact addTxt shape (a generic
    //     TextIdentificationFrame carrying "KEY=value" as one text).
    {
        std::string p = root + "/legacy.mp3";
        CHECK(writeMp3(p));
        {
            TagLib::MPEG::File f(TL_PATH(p), false);
            auto* tag = f.ID3v2Tag(true);
            auto* fr = new TagLib::ID3v2::TextIdentificationFrame("TXXX", TagLib::String::UTF8);
            fr->setText(TagLib::String("REPLAYGAIN_TRACK_GAIN=-6.92 dB", TagLib::String::UTF8));
            tag->addFrame(fr);
            auto* pk = new TagLib::ID3v2::TextIdentificationFrame("TXXX", TagLib::String::UTF8);
            pk->setText(TagLib::String("REPLAYGAIN_TRACK_PEAK=1.038428", TagLib::String::UTF8));
            tag->addFrame(pk);                   // must NOT be mistaken for gain
            f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
        }
        CHECK(approx(readBack(p), -6.92f));
    }

    // (b) standard desc/value TXXX — what other rippers (and our upcoming
    //     write fix) produce; reads via the PropertyMap path.
    {
        std::string p = root + "/standard.mp3";
        CHECK(writeMp3(p));
        {
            TagLib::MPEG::File f(TL_PATH(p), false);
            auto* tag = f.ID3v2Tag(true);
            auto* fr = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
            fr->setDescription("REPLAYGAIN_TRACK_GAIN");
            fr->setText("-4.50 dB");
            tag->addFrame(fr);
            f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
        }
        CHECK(approx(readBack(p), -4.50f));
    }

    // (c) lowercase description, standard shape — case-insensitive end-to-end.
    {
        std::string p = root + "/lowercase.mp3";
        CHECK(writeMp3(p));
        {
            TagLib::MPEG::File f(TL_PATH(p), false);
            auto* tag = f.ID3v2Tag(true);
            auto* fr = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
            fr->setDescription("replaygain_track_gain");
            fr->setText("+1.25 dB");
            tag->addFrame(fr);
            f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
        }
        CHECK(approx(readBack(p), 1.25f));
    }

    // (d) no RG tags -> 0.00; no gain invented from TENC/title/peak frames.
    {
        std::string p = root + "/bare.mp3";
        CHECK(writeMp3(p));
        CHECK(readBack(p) == 0.0f);
    }

    fs::remove_all(root, ec);
    std::printf(g_fail ? "mp3_rg_read_test: %d FAILED\n"
                       : "mp3_rg_read_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
