// gain_scan_test — the batch-ReplayGain engine contract (batch-r128).
//
// Synthetic suite (CI-safe, no artifacts needed): one sine rendered through
// the four production encoders (FLAC/MP3/Opus/WavPack), then the REAL
// GainScan engine over the folder. Pins: every format gets its
// dialect-correct tag; the gains agree across formats (same audio); the
// values read back through LocalFileSource (the player's path); skip/force
// semantics; the legacy-blob MP3 HEAL (blob frames removed, standard pair
// written); wav counted-skipped; per-file error isolation; cancel leaves
// whole files only.
//
// Artifact parity (the rip-agreement proof) runs when GAIN_SCAN_ARTIFACTS
// names a folder holding copies of the retained rip artifacts
// (subF/01.flac, subO/01.opus): their tags are stripped, the engine re-tags,
// and the values must equal the rip's originals within rounding. CI skips
// this leg (no smoke dir there); the local gate runs it.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "GainScan.h"
#include "LocalFileSource.h"
#include "FlacEncoder.h"
#include "Mp3Encoder.h"
#include "OpusRipEncoder.h"
#include "WavPackEncoder.h"
#include "StringUtils.h"
#include "PortUtil.h"   // port::sleepMs / tickMs (waitDone)

#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/opusfile.h>   // TagLib's own (collision-safe prefix)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
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

static bool waitDone(GainScan& g, int timeout_ms = 120000) {
    uint32_t t0 = port::tickMs();
    while (g.running() && (long)(port::tickMs() - t0) < timeout_ms) port::sleepMs(20);
    return !g.running();
}

// 6 s of -6 dBFS 440 Hz sine through one encoder.
template <typename Enc>
static bool renderSine(Enc&& enc, const std::string& path) {
    if (!enc.open(path, 6 * 44100)) return false;
    std::vector<int16_t> pcm((size_t)4410 * 2);
    uint64_t ph = 0;
    for (int blk = 0; blk < 60; ++blk) {
        for (size_t f = 0; f < 4410; ++f) {
            int16_t s = (int16_t)(16384.0 * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                                     (double)(ph + f) / 44100.0));
            pcm[f*2] = s; pcm[f*2+1] = s;
        }
        ph += 4410;
        if (!enc.writeFrames(pcm.data(), 4410)) return false;
    }
    return enc.finalize(true);
}

static float readRg(const std::string& path) {
    LocalFileSource s;
    if (!s.open(path)) return -999.0f;
    return s.info().replaygain_db;
}

int main() {
    const std::string root = (fs::temp_directory_path() / "remoct_gain_scan_test").string();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ── fixtures: the same sine in all four formats + a wav + a corrupt file ─
    CHECK(renderSine(FlacEncoder(),    root + "/a.flac"));
    CHECK(renderSine(Mp3Encoder(),     root + "/b.mp3"));
    CHECK(renderSine(OpusRipEncoder(), root + "/c.opus"));
    CHECK(renderSine(WavPackEncoder(), root + "/d.wv"));
    { FILE* f = fopen((root + "/e.wav").c_str(), "wb");    // wav: counted, never tagged
      fputs("RIFFnotreallywav", f); fclose(f); }
    { FILE* f = fopen((root + "/z.flac").c_str(), "wb");   // corrupt: error-isolated
      fputs("not a flac at all", f); fclose(f); }
    // A legacy BLOB-shape MP3 (the pre-mp3-rg-write library): must read as
    // untagged and come out HEALED.
    CHECK(renderSine(Mp3Encoder(), root + "/legacy.mp3"));
    {
        TagLib::MPEG::File f(TL_PATH(root + "/legacy.mp3"), false);
        auto* tag = f.ID3v2Tag(true);
        auto* fr = new TagLib::ID3v2::TextIdentificationFrame("TXXX", TagLib::String::UTF8);
        fr->setText(TagLib::String("REPLAYGAIN_TRACK_GAIN=-99.90 dB", TagLib::String::UTF8));
        tag->addFrame(fr);
        f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
    }

    // ── 1. first scan: everything untagged gets tagged; errors isolated ─────
    float flac_gain = 0;
    {
        GainScan g;
        CHECK(g.start(root, false));
        CHECK(!g.start(root, false));           // no double-start
        CHECK(waitDone(g));
        CHECK(g.tagged() == 5);                 // 4 formats + the healed legacy
        CHECK(g.skipped() == 0);
        CHECK(g.errors() == 1);                 // the corrupt z.flac, isolated
        CHECK(g.wavNoted() == 1);

        flac_gain = readRg(root + "/a.flac");
        CHECK(flac_gain < -5.0f && flac_gain > -13.0f);   // sane for a -6 dBFS sine
        // Same audio -> the gains agree across formats (codec tolerance).
        CHECK(std::fabs(readRg(root + "/b.mp3")  - flac_gain) < 0.6f);
        CHECK(std::fabs(readRg(root + "/c.opus") - flac_gain) < 0.6f);
        CHECK(std::fabs(readRg(root + "/d.wv")   - flac_gain) < 0.1f);  // lossless twin
        // The heal: standard frames present, the blob gone, value sane
        // (NOT the blob's -99.90). SCOPED: TagLib::MPEG::File opens
        // read-write by default, and a still-live handle blocks the FileRef
        // inside LocalFileSource on Windows (sharing violation -> metadata
        // silently skipped, rg reads 0) - the handle must close before the
        // readRg below. Found the hard way; the scope brace IS the fix.
        {
            TagLib::MPEG::File f(TL_PATH(root + "/legacy.mp3"), false);
            auto* tag = f.ID3v2Tag(false);
            int proper = 0, blob = 0;
            for (auto* fr : tag->frameList("TXXX")) {
                auto* u = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(fr);
                if (!u) continue;
                std::string d = u->description().to8Bit(true);
                if (d == "REPLAYGAIN_TRACK_GAIN") ++proper;
                if (d.find("REPLAYGAIN_TRACK_GAIN=") == 0) ++blob;
            }
            CHECK(proper == 1 && blob == 0);
        }
        CHECK(std::fabs(readRg(root + "/legacy.mp3") - flac_gain) < 0.6f);
    }

    // ── 2. re-run: everything skips (the resumability contract) ─────────────
    {
        GainScan g;
        CHECK(g.start(root, false));
        CHECK(waitDone(g));
        CHECK(g.tagged() == 0);
        CHECK(g.skipped() == 5);
        CHECK(g.errors() == 1);                 // corrupt file still errors
    }

    // ── 3. force: re-tags all, values stable ────────────────────────────────
    {
        GainScan g;
        CHECK(g.start(root, true));
        CHECK(waitDone(g));
        CHECK(g.tagged() == 5 && g.skipped() == 0);
        CHECK(std::fabs(readRg(root + "/a.flac") - flac_gain) < 0.05f);
    }

    // ── 4. cancel: immediate cancel leaves whole files only, no crash ───────
    {
        GainScan g;
        CHECK(g.start(root, true));
        g.cancel();
        CHECK(waitDone(g));
        CHECK(g.cancelled());
        CHECK(readRg(root + "/a.flac") != -999.0f);   // library intact + decodable
    }

    // ── 5. artifact parity (local gate; CI skips when env unset) ────────────
    if (const char* art = std::getenv("GAIN_SCAN_ARTIFACTS")) {
        std::string ad(art);
        // Strip the retained artifacts' gain tags, re-tag with the engine,
        // and the values must equal the rip's originals within rounding.
        float flac_orig = readRg(ad + "/01.flac");
        float opus_orig = readRg(ad + "/01.opus");
        {
            TagLib::FLAC::File f(TL_PATH(ad + "/01.flac"), false);
            auto* t = f.xiphComment(true);
            t->removeFields("REPLAYGAIN_TRACK_GAIN"); t->removeFields("REPLAYGAIN_TRACK_PEAK");
            t->removeFields("REPLAYGAIN_ALBUM_GAIN"); t->removeFields("REPLAYGAIN_ALBUM_PEAK");
            f.save();
        }
        {
            TagLib::Ogg::Opus::File f(TL_PATH(ad + "/01.opus"), false);
            f.tag()->removeFields("R128_TRACK_GAIN");
            f.tag()->removeFields("R128_ALBUM_GAIN");
            f.save();
        }
        CHECK(readRg(ad + "/01.flac") == 0.0f);       // stripped
        GainScan g;
        CHECK(g.start(ad, false));
        CHECK(waitDone(g, 600000));
        CHECK(g.tagged() >= 2);
        float flac_new = readRg(ad + "/01.flac");
        float opus_new = readRg(ad + "/01.opus");
        std::printf("parity: flac rip=%.2f batch=%.2f | opus rip=%.2f batch=%.2f\n",
                    flac_orig, flac_new, opus_orig, opus_new);
        // Lossless parity is EXACT (same math, same bytes). The lossy leg
        // measures the DECODED opus where the rip measured the pre-encode
        // PCM - the codec round-trip shifts loudness a few hundredths of a
        // dB (physics, inaudible; batch measures what playback hears).
        CHECK(std::fabs(flac_new - flac_orig) < 0.05f);
        CHECK(std::fabs(opus_new - opus_orig) < 0.15f);
    } else {
        std::printf("(artifact parity leg skipped - GAIN_SCAN_ARTIFACTS unset)\n");
    }

    if (!g_fail) fs::remove_all(root, ec);   // keep evidence on failure
    std::printf(g_fail ? "gain_scan_test: %d FAILED\n"
                       : "gain_scan_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
