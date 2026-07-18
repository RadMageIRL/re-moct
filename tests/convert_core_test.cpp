// convert_core_test - the convert-core contract, headless (the GainScan/
// copy_mux precedent). No UI: it drives ConvertJob::convertOne and the
// selector-list helpers directly.
//
// Coverage:
//   - MarkSet: toggle/contains/clear, path-keyed (order-independent) list.
//   - convertDstPath / convertSupportedInput.
//   - convert core: every encodable input decodes, converts to every output,
//     the output decodes with >0 frames, and text tags carry across.
//   - f32<->s16 handoff: a WAV -> WAV convert is byte-exact on the audio data.
//   - non-recursive folder enumeration (a nested dir is excluded).
//   - the src==dst and dst-exists guards.
//
// makeEncoder (EncoderFactory) is reused to synthesize the input fixtures, so
// the test never hand-rolls an encoder.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ConvertJob.h"
#include "EncoderFactory.h"
#include "LocalFileSource.h"
#include "MarkSet.h"
#include "StringUtils.h"   // utf8_to_wide

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

static int failures = 0;
static void check(bool cond, const char* what) {
    std::printf("%-56s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) ++failures;
}

static std::vector<int16_t> makeFixture(size_t frames) {
    std::vector<int16_t> pcm(frames * 2);
    uint32_t x = 0x4D6F4354u;
    for (auto& s : pcm) { x = x * 1103515245u + 12345u; s = (int16_t)(x >> 16); }
    return pcm;
}

static bool encodeFixture(const std::string& path, RipFormat fmt,
                          const std::vector<int16_t>& pcm) {
    RipOptions opt;                       // defaults
    auto enc = makeEncoder(fmt, opt);
    if (!enc || !enc->open(path, pcm.size() / 2)) return false;
    const size_t frames = pcm.size() / 2;
    size_t off = 0; bool ok = true;
    while (off < frames && ok) {
        size_t n = std::min(frames - off, (size_t)4096);
        ok = enc->writeFrames(pcm.data() + off * 2, (size_t)n);
        off += n;
    }
    return enc->finalize(ok) && ok;
}

static uint64_t decodeCount(const std::string& path) {
    LocalFileSource s;
    if (!s.open(path)) return 0;
    std::vector<float> buf((size_t)4096 * 2);
    uint64_t total = 0;
    for (;;) { uint32_t g = s.readFrames(buf.data(), 4096); if (!g) break; total += g; }
    s.close();
    return total;
}

static void writeTag(const std::string& path, const char* key, const char* val) {
    TagLib::FileRef f(TL_PATH(path), false);
    if (f.isNull() || !f.file()) return;
    TagLib::PropertyMap pm = f.file()->properties();
    pm.replace(key, TagLib::StringList(TagLib::String(val, TagLib::String::UTF8)));
    f.file()->setProperties(pm);
    f.file()->save();
}

static std::string readTag(const std::string& path, const char* key) {
    TagLib::FileRef f(TL_PATH(path), true);
    if (f.isNull() || !f.file()) return {};
    TagLib::PropertyMap pm = f.file()->properties();
    if (pm.contains(key) && !pm[key].isEmpty()) return pm[key].front().to8Bit(true);
    return {};
}

static std::vector<uint8_t> slurp(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    if (sz > 0 && fread(out.data(), 1, (size_t)sz, f) != (size_t)sz) out.clear();
    fclose(f);
    return out;
}

int main() {
    // ── MarkSet: path-keyed membership ─────────────────────────────────────
    {
        MarkSet m;
        check(m.toggle("/z/a.flac") && m.contains("/z/a.flac"), "MarkSet toggle marks");
        check(!m.toggle("/z/a.flac") && !m.contains("/z/a.flac"), "MarkSet toggle unmarks");
        m.add("/z/c.flac"); m.add("/z/a.flac"); m.add("/z/b.flac");
        auto l = m.list();
        check(l.size() == 3 && l[0] == "/z/a.flac" && l[2] == "/z/c.flac",
              "MarkSet list is sorted, order-independent (survives re-sort)");
        m.remove("/z/b.flac");
        check(m.size() == 2 && !m.contains("/z/b.flac"), "MarkSet remove");
        m.clear();
        check(m.empty(), "MarkSet clear");
    }

    // ── dst policy + supported-input ───────────────────────────────────────
    {
        std::string d = convertDstPath("/music/song.mp3", RipFormat::Flac);
        check(fs::path(d).extension() == ".flac" &&
              fs::path(d).stem() == "song" &&
              fs::path(d).parent_path() == fs::path("/music"),
              "convertDstPath = same dir + basename + new ext");
        check(convertSupportedInput("a.flac") && convertSupportedInput("a.WV") &&
              convertSupportedInput("a.m4a") && !convertSupportedInput("a.txt"),
              "convertSupportedInput recognizes the decodable set");
    }

    const size_t kFrames = 44100;           // 1 s
    auto pcm = makeFixture(kFrames);
    fs::path dir = fs::temp_directory_path() / "remoct_convert_test";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    auto P = [&](const char* n) { return (dir / n).string(); };

    // Build one source per encodable input format.
    struct Src { RipFormat fmt; const char* file; };
    Src srcs[] = {
        { RipFormat::Flac,    "src.flac" },
        { RipFormat::Mp3,     "src.mp3"  },
        { RipFormat::Wav,     "src.wav"  },
        { RipFormat::Opus,    "src.opus" },
        { RipFormat::WavPack, "src.wv"   },
    };
    bool all_src = true;
    for (auto& s : srcs) all_src &= encodeFixture(P(s.file), s.fmt, pcm);
    check(all_src, "all input fixtures encoded (via makeEncoder)");

    // Every input decodes and converts to FLAC with >0 frames (input coverage).
    // Output name keyed on the source EXTENSION (all fixtures share the stem
    // "src", so a stem-based name would collide and trip the skip-existing guard).
    for (auto& s : srcs) {
        std::string tag = fs::path(s.file).extension().string();   // ".mp3" etc.
        if (!tag.empty()) tag.erase(0, 1);
        std::string out = P((std::string("in_") + tag + ".flac").c_str());
        auto r = ConvertJob::convertOne(P(s.file), out, RipFormat::Flac, RipOptions{});
        bool ok = (r == ConvertJob::Result::Converted) && decodeCount(out) > 0;
        check(ok, (std::string("convert ") + s.file + " -> flac decodes").c_str());
    }

    // FLAC source converts to every output with >0 frames (output coverage).
    struct Out { RipFormat fmt; const char* ext; };
    Out outs[] = {
        { RipFormat::Flac, ".flac" }, { RipFormat::Mp3, ".mp3" }, { RipFormat::Wav, ".wav" },
        { RipFormat::Opus, ".opus" }, { RipFormat::WavPack, ".wv" },
    };
    for (auto& o : outs) {
        std::string out = P((std::string("out") + o.ext).c_str());
        auto r = ConvertJob::convertOne(P("src.flac"), out, o.fmt, RipOptions{});
        bool ok = (r == ConvertJob::Result::Converted) && decodeCount(out) > 0;
        check(ok, (std::string("convert flac -> ") + o.ext + " decodes").c_str());
    }

    // ── text tag carryover ─────────────────────────────────────────────────
    writeTag(P("src.flac"), "ARTIST", "Convert Test Artist");
    {
        std::string out = P("tagged.mp3");
        auto r = ConvertJob::convertOne(P("src.flac"), out, RipFormat::Mp3, RipOptions{});
        check(r == ConvertJob::Result::Converted &&
              readTag(out, "ARTIST") == "Convert Test Artist",
              "text tag (ARTIST) carried source -> output");
    }

    // ── f32<->s16: WAV -> WAV is byte-exact on the audio data ──────────────
    {
        std::string out = P("roundtrip.wav");
        auto r = ConvertJob::convertOne(P("src.wav"), out, RipFormat::Wav, RipOptions{});
        auto a = slurp(P("src.wav"));
        auto b = slurp(out);
        bool same = (r == ConvertJob::Result::Converted) &&
                    a.size() == b.size() && a.size() > 44 &&
                    std::equal(a.begin() + 44, a.end(), b.begin() + 44);
        check(same, "WAV -> WAV audio data byte-exact (f32<->s16 round-trip)");
    }

    // ── guards ─────────────────────────────────────────────────────────────
    check(ConvertJob::convertOne(P("src.flac"), P("src.flac"), RipFormat::Flac, RipOptions{})
          == ConvertJob::Result::SkippedSamePath, "src == dst rejected (no in-place clobber)");
    check(ConvertJob::convertOne(P("src.mp3"), P("out.flac"), RipFormat::Flac, RipOptions{})
          == ConvertJob::Result::SkippedExists, "existing dst skipped (no clobber)");

    // ── non-recursive folder enumeration ───────────────────────────────────
    {
        fs::path flat = dir / "flat";
        fs::create_directories(flat / "sub", ec);
        encodeFixture((flat / "top.wav").string(),        RipFormat::Wav, pcm);
        encodeFixture((flat / "sub" / "deep.wav").string(), RipFormat::Wav, pcm);
        ConvertJob job;
        bool started = job.startFolder(flat.string(), RipFormat::Flac, RipOptions{});
        for (int i = 0; i < 2000 && job.running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        bool ok = started && !job.running() && job.total() == 1 && job.converted() == 1 &&
                  fs::exists(flat / "top.flac") && !fs::exists(flat / "sub" / "deep.flac");
        check(ok, "startFolder is flat (nested dir excluded; top.wav only)");
    }

    fs::remove_all(dir, ec);
    std::printf("\n%s (%d failure%s)\n",
                failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
