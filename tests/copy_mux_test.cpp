// copy_mux_test — abi-cluster slice B: FrameSync + the ADTS->M4A mux writer,
// round-tripped through the app's OWN decode path (the plan's oracle move:
// AacDecoder already parses MP4 — the writer's correctness check exists
// in-tree, no external tool).
//
//   1. FrameSync unit contract: ADTS walk over a REAL FDK-encoded stream
//      (every frame parses, lengths tile the buffer exactly, sr/samples/
//      channels from the header), MP3 header vectors (MPEG1/2, padding,
//      rejects), the ID3v2 syncsafe skipper.
//   2. The full copy chain on REAL audio: encodeAdtsSine -> the recorder's
//      copy pump (injected pull, no plugin/network) -> a tagged .m4a cut ->
//      LocalFileSource::open (miniaudio + the FDK MP4 backend — the exact
//      code playback uses) -> decoded sample count is FRAME-EXACT
//      (1024 * AU count) and carries real energy; TagLib::MP4 reads the tag
//      pass's fields + covr (decision #1: no untagged backlog).
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "LocalFileSource.h"
#include "StreamRecorder.h"
#include "FrameSync.h"
#include "MuxWriter.h"
#include "StringUtils.h"   // utf8_to_wide (TagLib::FileName is wide on Windows)
#include "hls_fixture.h"   // encodeAdtsSine (REAL FDK ADTS) + waitFor

#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
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

int main() {
    const std::string root = (fs::temp_directory_path() / "remoct_copy_mux_test").string();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ── 1a. FrameSync over a real ADTS stream ────────────────────────────────
    std::vector<uint8_t> adts = encodeAdtsSine(2.0, 440.0, 0.5);
    CHECK(adts.size() > 10000);
    size_t n_aus = 0, pos = 0;
    bool tiled = true;
    while (pos < adts.size()) {
        framesync::FrameInfo fi;
        if (!framesync::adtsParse(adts.data() + pos, adts.size() - pos, fi)) {
            tiled = false;
            break;
        }
        CHECK(fi.sample_rate == 44100 && fi.samples == 1024 && fi.channels == 2);
        CHECK(fi.header_len == 7 && fi.frame_len > fi.header_len);
        pos += fi.frame_len;
        ++n_aus;
    }
    CHECK(tiled && pos == adts.size());               // frames tile exactly
    CHECK(n_aus >= 80);                               // ~2 s at 1024/44100

    // ── 1b. MP3 header vectors ───────────────────────────────────────────────
    {
        framesync::FrameInfo fi;
        uint8_t v1[] = { 0xFF, 0xFB, 0x90, 0x00 };    // MPEG1 L3 128k 44100
        CHECK(framesync::mp3Parse(v1, 4, fi));
        CHECK(fi.frame_len == 417 && fi.samples == 1152 &&
              fi.sample_rate == 44100 && fi.channels == 2 && fi.header_len == 4);
        uint8_t v1p[] = { 0xFF, 0xFB, 0x92, 0x00 };   // + padding bit
        CHECK(framesync::mp3Parse(v1p, 4, fi) && fi.frame_len == 418);
        uint8_t v2[] = { 0xFF, 0xF3, 0x80, 0x00 };    // MPEG2 L3 64k 22050
        CHECK(framesync::mp3Parse(v2, 4, fi));
        CHECK(fi.frame_len == 208 && fi.samples == 576 && fi.sample_rate == 22050);
        uint8_t mono[] = { 0xFF, 0xFB, 0x90, 0xC0 };  // mode 3 = mono
        CHECK(framesync::mp3Parse(mono, 4, fi) && fi.channels == 1);
        uint8_t freefmt[] = { 0xFF, 0xFB, 0x00, 0x00 };   // free-format: reject
        CHECK(!framesync::mp3Parse(freefmt, 4, fi));
        uint8_t nosync[] = { 0xFE, 0xFB, 0x90, 0x00 };
        CHECK(!framesync::mp3Parse(nosync, 4, fi));
        CHECK(!framesync::mp3Parse(v1, 3, fi));       // truncation honest
    }

    // ── 1c. ID3v2 skipper ────────────────────────────────────────────────────
    {
        uint8_t tag[] = { 'I','D','3', 4, 0, 0x00, 0, 0, 0, 100 };
        CHECK(framesync::id3v2Size(tag, sizeof tag) == 110);
        uint8_t foot[] = { 'I','D','3', 4, 0, 0x10, 0, 0, 0, 100 };
        CHECK(framesync::id3v2Size(foot, sizeof foot) == 120);   // + footer
        uint8_t not_tag[] = { 'I','D','4', 4, 0, 0, 0, 0, 0, 1 };
        CHECK(framesync::id3v2Size(not_tag, sizeof not_tag) == 0);
        uint8_t bad_sync[] = { 'I','D','3', 4, 0, 0, 0x80, 0, 0, 1 };
        CHECK(framesync::id3v2Size(bad_sync, sizeof bad_sync) == 0);
    }

    // ── 2. the full copy chain on real audio ────────────────────────────────
    {
        StreamRecorder rec;
        std::mutex m;
        std::deque<std::vector<uint8_t>> q;
        // Serve the real ADTS stream in transport-sized chunks (8 KiB), the
        // shape a live tee pull actually sees.
        for (size_t i = 0; i < adts.size(); i += 8192) {
            size_t n = std::min<size_t>(8192, adts.size() - i);
            q.emplace_back(adts.begin() + i, adts.begin() + i + n);
        }
        RecOptions opt;
        opt.copy_mode = true;
        opt.split_offset_ms = 0;
        opt.pull = [&](uint8_t* dst, uint32_t cap, int32_t* c, int32_t* d) -> uint32_t {
            std::lock_guard<std::mutex> lk(m);
            *c = 2; *d = 0;                            // AAC ADTS
            if (q.empty()) return 0;
            auto& b = q.front();
            uint32_t n = (uint32_t)std::min<size_t>(cap, b.size());
            std::memcpy(dst, b.data(), n);
            if (n == b.size()) q.pop_front();
            else b.erase(b.begin(), b.begin() + n);
            return n;
        };
        CHECK(rec.start(opt, "RT FM", root));
        rec.onTitle("RT Artist - RT Song");
        std::vector<uint8_t> jpeg = { 0xFF, 0xD8, 0xFF, 0xE0 };
        jpeg.resize(200, 0x37);
        rec.onArt("RT Artist - RT Song", jpeg);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == (uint64_t)n_aus * 1024; }));
        rec.stop();
        CHECK(rec.lastError().empty());

        auto cuts = rec.cuts();
        CHECK(cuts.size() == 1);
        if (cuts.size() == 1 && cuts[0].paths.size() == 1) {
            const std::string& p = cuts[0].paths[0];
            CHECK(cuts[0].frames == (uint64_t)n_aus * 1024);

            // Round-trip through the app's own decode path (AacDecoder's MP4
            // box parser behind miniaudio): FRAME-EXACT sample count + energy.
            LocalFileSource src;
            CHECK(src.open(p));
            uint64_t total = 0;
            double   acc = 0.0;
            float    buf[1024 * 2];
            for (;;) {
                uint32_t got = src.readFrames(buf, 1024);
                if (got == 0) break;
                for (uint32_t i = 0; i < got * 2; ++i)
                    acc += (double)buf[i] * buf[i];
                total += got;
                if (total > (uint64_t)n_aus * 1024 + 44100) break;   // runaway guard
            }
            CHECK(total == (uint64_t)n_aus * 1024);    // frame-exact round trip
            double rms = std::sqrt(acc / (double)(total * 2));
            CHECK(rms > 0.1);                          // real sine, not silence

            // TagLib::MP4 reads the tag pass (title/artist/covr) — no
            // untagged backlog, per the locked container decision.
            TagLib::MP4::File f(TL_PATH(p), true);
            CHECK(f.isValid() && f.tag());
            if (f.isValid() && f.tag()) {
                CHECK(f.tag()->title().to8Bit(true)  == "RT Song");
                CHECK(f.tag()->artist().to8Bit(true) == "RT Artist");
                CHECK(f.tag()->contains("covr"));
                if (f.tag()->contains("covr")) {
                    auto covers = f.tag()->item("covr").toCoverArtList();
                    CHECK(!covers.isEmpty() &&
                          covers.front().data().size() == (unsigned)jpeg.size());
                }
            }
            if (f.isValid() && f.audioProperties())
                CHECK(f.audioProperties()->sampleRate() == 44100);
        }
    }

    fs::remove_all(root, ec);
    std::printf(g_fail ? "copy_mux_test: %d FAILED\n"
                       : "copy_mux_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
