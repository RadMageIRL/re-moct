// stream_record_test — the frozen StreamRecorder engine contract (stream-record
// slice R1), proven headless BEFORE any UI exists (the probe-first doctrine).
//
// Drives the REAL engine end-to-end: a producer pushes synthetic f32 PCM
// through capture() (the audio-thread entry), scripted onTitle() events fire
// the split path, and the worker's real Mp3Encoder/OpusRipEncoder output is
// read back off disk with TagLib. Covers:
//   1. the three-cut session: split boundaries exact at the frame level
//      (worker accounting), first/tail cuts marked partial, middle cut clean,
//      per-format files exist and carry the right tags + per-cut ReplayGain
//      (plain RG TXXX on MP3, R128 Q7.8 on Opus — the two dialects), zero
//      drops, duplicate onTitle dedup (a repeat must not cut twice)
//   2. timestamp-fallback naming on an unparseable StreamTitle (title-only) —
//      a sanely-named valid file, raw string preserved in the TITLE tag
//   3. split_on_meta off: one continuous timestamp-named file, titles ignored
//   4. ring overflow (tiny ring, instant production): drops COUNTED and
//      accounted (written + dropped == pushed), capture degrades honestly —
//      the finished file stays valid, recording does not fail
//   5. parseNowPlaying: the "Artist - Title" convention edge cases
#include "StreamRecorder.h"
#include "RipFormats.h"
#include "StringUtils.h"   // utf8_to_wide for the TagLib read-back on Windows
#include "PortUtil.h"      // sleepMs / tickMs

#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>   // UserTextIdentificationFrame (readMp3)
#include <taglib/opusfile.h>   // TagLib's own (collision-safe: taglib/ prefixed)
#include <taglib/xiphcomment.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static bool waitFor(int timeout_ms, const std::function<bool()>& pred) {
    uint32_t t0 = port::tickMs();
    while ((long)(port::tickMs() - t0) < timeout_ms) {
        if (pred()) return true;
        port::sleepMs(10);
    }
    return pred();
}

// n frames of a stereo sine at `freq`/`amp`, pushed through capture() in
// callback-sized blocks. phase carries across calls via the caller's counter.
static void pushSine(StreamRecorder& rec, uint64_t& phase, uint32_t frames,
                     double freq, double amp) {
    float blk[512 * 2];
    uint32_t left = frames;
    while (left > 0) {
        uint32_t n = left < 512 ? left : 512;
        for (uint32_t i = 0; i < n; ++i) {
            float s = (float)(amp * std::sin(2.0 * 3.14159265358979 * freq *
                                             (double)(phase + i) / 44100.0));
            blk[i*2] = s; blk[i*2+1] = s;
        }
        rec.capture(blk, n);
        phase += n;
        left  -= n;
    }
}

// TagLib read-back helpers (ASCII test paths; wide on Windows anyway, matching
// the writer).
#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

struct Mp3Tags {
    std::string title, artist;
    bool has_rg = false;       // a PROPER standard-shape RG frame was found
    std::string rg_gain;       // its VALUE field, e.g. "-6.92 dB"
};
// Strengthened for mp3-rg-write: the RG frame must be a genuine
// UserTextIdentificationFrame with a CLEAN description (exactly
// "REPLAYGAIN_TRACK_GAIN" — no "=value" blob baked in) and a NON-EMPTY
// value field. This is the production-write half of the write->read chain:
// tagCut writes the standard shape on every ctest, and mp3_rg_read_test's
// standard-shape case proves LocalFileSource reads exactly that shape.
static Mp3Tags readMp3(const std::string& path) {
    Mp3Tags out;
    TagLib::MPEG::File f(TL_PATH(path), true);
    if (!f.isValid()) return out;
    auto* tag = f.ID3v2Tag(false);
    if (!tag) return out;
    out.title  = tag->title().to8Bit(true);
    out.artist = tag->artist().to8Bit(true);
    for (auto* fr : tag->frameList("TXXX")) {
        auto* u = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(fr);
        if (!u) continue;
        if (u->description().to8Bit(true) != "REPLAYGAIN_TRACK_GAIN") continue;
        const auto& fl = u->fieldList();     // [0]=description, [1..]=value(s)
        for (unsigned int i = 1; i < fl.size(); ++i)
            if (!fl[i].isEmpty()) { out.has_rg = true; out.rg_gain = fl[i].to8Bit(true); break; }
    }
    return out;
}

struct OpusTags {
    std::string title, artist;
    bool has_r128 = false;
    int  r128 = 0;
    int  length_sec = -1;
};
static OpusTags readOpus(const std::string& path) {
    OpusTags out;
    TagLib::Ogg::Opus::File f(TL_PATH(path), true);
    if (!f.isValid()) return out;
    auto* tag = f.tag();
    if (!tag) return out;
    out.title  = tag->title().to8Bit(true);
    out.artist = tag->artist().to8Bit(true);
    const auto& m = tag->fieldListMap();
    auto it = m.find("R128_TRACK_GAIN");
    if (it != m.end() && !it->second.isEmpty()) {
        out.has_r128 = true;
        try { out.r128 = std::stoi(it->second.front().to8Bit(true)); } catch (...) {}
    }
    if (f.audioProperties()) out.length_sec = f.audioProperties()->lengthInSeconds();
    return out;
}

int main() {
    const std::string root =
        (fs::temp_directory_path() / "remoct_stream_record_test").string();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ── 1. the three-cut session (both formats) ──────────────────────────────
    {
        StreamRecorder rec;
        RecOptions opt;
        opt.formats = { RipFormat::Mp3, RipFormat::Opus };
        CHECK(rec.start(opt, "Test FM", root));
        CHECK(rec.recording());

        uint64_t phase = 0;
        rec.onTitle("Artist One - Song One");
        pushSine(rec, phase, 2 * 44100, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 2ull * 44100; }));

        rec.onTitle("Artist Two - Song Two");
        rec.onTitle("Artist Two - Song Two");   // duplicate: must NOT cut twice
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 1; }));

        pushSine(rec, phase, 2 * 44100, 880.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 4ull * 44100; }));

        rec.onTitle("Artist Three - Song Three");
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 2; }));

        pushSine(rec, phase, 1 * 44100, 660.0, 0.5);
        rec.stop();
        CHECK(!rec.recording());
        CHECK(rec.lastError().empty());
        CHECK(rec.droppedFrames() == 0);
        CHECK(rec.totalFrames() == 5ull * 44100);
        CHECK(rec.elapsedSec() == 5);

        auto cuts = rec.cuts();
        CHECK(cuts.size() == 3);
        if (cuts.size() == 3) {
            // Frame-exact split boundaries (the engine contract: everything
            // queued at the event goes to the old cut; the test quiesced the
            // ring first, so the boundary is exact).
            CHECK(cuts[0].frames == 2ull * 44100);
            CHECK(cuts[1].frames == 2ull * 44100);
            CHECK(cuts[2].frames == 1ull * 44100);
            CHECK(cuts[0].partial  && !cuts[0].failed);   // joined "mid-song"
            CHECK(!cuts[1].partial && !cuts[1].failed);   // clean middle cut
            CHECK(cuts[2].partial  && !cuts[2].failed);   // stopped mid-song
            CHECK(cuts[0].artist == "Artist One" && cuts[0].title == "Song One");
            CHECK(cuts[1].artist == "Artist Two" && cuts[1].title == "Song Two");
            CHECK(cuts[2].artist == "Artist Three" && cuts[2].title == "Song Three");
            for (const auto& c : cuts) {
                CHECK(c.paths.size() == 2);
                for (const auto& p : c.paths) {
                    CHECK(fs::exists(p));
                    CHECK(fs::file_size(p, ec) > 1000);
                    CHECK(p.find(root) == 0 && p.find("Test FM") != std::string::npos);
                }
            }
            CHECK(cuts[0].paths[0].find(" (partial)") != std::string::npos);
            CHECK(cuts[1].paths[0].find(" (partial)") == std::string::npos);
            CHECK(cuts[2].paths[0].find(" (partial)") != std::string::npos);

            // Tag read-back on the clean middle cut, both dialects.
            std::string mp3p, opusp;
            for (const auto& p : cuts[1].paths)
                (p.size() > 4 && p.substr(p.size()-4) == ".mp3" ? mp3p : opusp) = p;
            Mp3Tags mt = readMp3(mp3p);
            CHECK(mt.title == "Song Two" && mt.artist == "Artist Two");
            CHECK(mt.has_rg);
            OpusTags ot = readOpus(opusp);
            CHECK(ot.title == "Song Two" && ot.artist == "Artist Two");
            CHECK(ot.has_r128);
            // -6 dBFS sine -> integrated loudness ≈ -9 LUFS -> RG ≈ -9 dB ->
            // Q7.8 = (gain - 5) * 256 ≈ -3580. Loose band: the assert is
            // "a sane per-cut gain in the R128 dialect", not a loudness spec.
            CHECK(ot.r128 > -4600 && ot.r128 < -2600);
            CHECK(ot.length_sec >= 1 && ot.length_sec <= 3);   // 2 s cut
        }
    }

    // ── 2. timestamp fallback on an unparseable StreamTitle ─────────────────
    {
        StreamRecorder rec;
        RecOptions opt;
        opt.formats = { RipFormat::Opus };
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("JustATitleNoDash");
        pushSine(rec, phase, 44100, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 44100; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 1);
        if (cuts.size() == 1 && cuts[0].paths.size() == 1) {
            const std::string& p = cuts[0].paths[0];
            // Station-timestamp name, never an empty/garbage one...
            std::string base = fs::path(p).filename().string();
            CHECK(base.rfind("Test FM - ", 0) == 0);
            CHECK(cuts[0].partial);                    // first == last == partial
            // ...but the raw metadata still lands in the TITLE tag.
            OpusTags ot = readOpus(p);
            CHECK(ot.title == "JustATitleNoDash");
            CHECK(ot.artist.empty());
        }
    }

    // ── 3. split_on_meta off: one continuous file, titles ignored ────────────
    {
        StreamRecorder rec;
        RecOptions opt;
        opt.formats = { RipFormat::Opus };
        opt.split_on_meta = false;
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("Artist One - Song One");
        pushSine(rec, phase, 22050, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 22050; }));
        rec.onTitle("Artist Two - Song Two");
        pushSine(rec, phase, 22050, 880.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 44100; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 1);
        if (cuts.size() == 1) {
            CHECK(cuts[0].frames == 44100);
            CHECK(fs::path(cuts[0].paths[0]).filename().string().rfind("Test FM - ", 0) == 0);
        }
    }

    // ── 4. ring overflow: drops counted, capture degrades honestly ──────────
    {
        StreamRecorder rec(4096);          // ~93 ms ring — trivially floodable
        RecOptions opt;
        opt.formats = { RipFormat::Opus };
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("Flood Artist - Flood Song");
        pushSine(rec, phase, 10 * 44100, 440.0, 0.5);   // instant: MUST overflow
        CHECK(waitFor(15000, [&]{
            return rec.totalFrames() + rec.droppedFrames() == 10ull * 44100; }));
        rec.stop();
        CHECK(rec.droppedFrames() > 0);                 // the honesty counter
        CHECK(rec.lastError().empty());                 // degraded, not failed
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 1);
        if (cuts.size() == 1) {
            CHECK(!cuts[0].failed);
            CHECK(cuts[0].frames == rec.totalFrames());
            CHECK(fs::exists(cuts[0].paths[0]));
            CHECK(readOpus(cuts[0].paths[0]).title == "Flood Song");
        }
    }

    // ── 5. parseNowPlaying edge cases ─────────────────────────────────────────
    {
        auto p = StreamRecorder::parseNowPlaying("Artist - Title");
        CHECK(p.ok && p.artist == "Artist" && p.title == "Title");
        p = StreamRecorder::parseNowPlaying("A - B - C");        // first dash wins
        CHECK(p.ok && p.artist == "A" && p.title == "B - C");
        p = StreamRecorder::parseNowPlaying("  A  -  B  ");      // whitespace junk
        CHECK(p.ok && p.artist == "A" && p.title == "B");
        CHECK(!StreamRecorder::parseNowPlaying("NoDashAtAll").ok);
        CHECK(!StreamRecorder::parseNowPlaying("").ok);
        CHECK(!StreamRecorder::parseNowPlaying(" - TitleOnly").ok);
        CHECK(!StreamRecorder::parseNowPlaying("ArtistOnly - ").ok);
        p = StreamRecorder::parseNowPlaying("Bj\xc3\xb6rk - J\xc3\xb3ga");
        CHECK(p.ok && p.artist == "Bj\xc3\xb6rk");               // UTF-8 inert
        // Article de-inversion (R2: shared deinvertArtist, filename+tag home)
        p = StreamRecorder::parseNowPlaying("Shins, The - New Slang");
        CHECK(p.ok && p.artist == "The Shins" && p.title == "New Slang");
        p = StreamRecorder::parseNowPlaying("Tyler, The Creator - Earfquake");
        CHECK(p.ok && p.artist == "Tyler, The Creator");         // guarded: not a bare article
        // Rejected format lists fail fast with a reason, not a crash.
        StreamRecorder rec;
        RecOptions opt; opt.formats = { RipFormat::Flac };
        CHECK(!rec.start(opt, "X", root));
        CHECK(!rec.lastError().empty());
        opt.formats.clear();
        CHECK(!rec.start(opt, "X", root));
    }

    fs::remove_all(root, ec);
    std::printf(g_fail ? "stream_record_test: %d FAILED\n"
                       : "stream_record_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
