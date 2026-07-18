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
#include "FrameSync.h"     // slice B: synthetic frame construction (copy mode)
#include "StringUtils.h"   // utf8_to_wide for the TagLib read-back on Windows
#include "PortUtil.h"      // sleepMs / tickMs

#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>   // UserTextIdentificationFrame (readMp3)
#include <taglib/attachedpictureframe.h>      // rec-cover-art: APIC read-back
#include <taglib/opusfile.h>   // TagLib's own (collision-safe: taglib/ prefixed)
#include <taglib/xiphcomment.h>
#include <taglib/mp4file.h>    // slice B: the .m4a copy cut's tag read-back
#include <taglib/mp4tag.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
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

// ─── slice B: copy-mode fixtures ─────────────────────────────────────────────
// Synthetic-but-VALID frame headers (FrameSync parses them; nothing decodes
// them — the copy path never decodes, that's the claim under test).

// MPEG-1 Layer III, 128 kbps, 44100 Hz, no padding -> 417 bytes, 1152 samples.
static std::vector<uint8_t> mp3Frame(uint8_t fill) {
    std::vector<uint8_t> f(417, fill);
    f[0] = 0xFF; f[1] = 0xFB; f[2] = 0x90; f[3] = 0x00;
    return f;
}
// ADTS AAC-LC, 44100 Hz (sr index 4), stereo, 256-byte payload -> 263 bytes.
static std::vector<uint8_t> adtsFrame(uint8_t fill) {
    const uint32_t len = 7 + 256;
    std::vector<uint8_t> f(len, fill);
    f[0] = 0xFF; f[1] = 0xF1;                       // sync, MPEG-4, no CRC
    f[2] = (uint8_t)((1 << 6) | (4 << 2));          // LC profile, sr index 4
    f[3] = (uint8_t)((2 << 6) | ((len >> 11) & 3)); // stereo, len hi
    f[4] = (uint8_t)((len >> 3) & 0xFF);            // len mid
    f[5] = (uint8_t)(((len & 7) << 5) | 0x1F);      // len lo, fullness hi
    f[6] = 0xFC;                                    // fullness lo, 1 block
    return f;
}

// The injected byte source: a scripted queue of (chunk, discont) pairs the
// worker pump drains — exactly the shape AudioManager wires to read_encoded.
struct FakePull {
    std::mutex m;
    std::deque<std::pair<std::vector<uint8_t>, int32_t>> q;
    int32_t codec = 1;                              // 1=MP3, 2=AAC ADTS
    void push(std::vector<uint8_t> b, int32_t disc = 0) {
        std::lock_guard<std::mutex> lk(m);
        q.emplace_back(std::move(b), disc);
    }
    void pushFrames(const std::vector<uint8_t>& fr, int n, int32_t disc = 0) {
        std::vector<uint8_t> b;
        b.reserve(fr.size() * (size_t)n);
        for (int i = 0; i < n; ++i) b.insert(b.end(), fr.begin(), fr.end());
        push(std::move(b), disc);
    }
    uint32_t pull(uint8_t* dst, uint32_t cap, int32_t* c, int32_t* d) {
        std::lock_guard<std::mutex> lk(m);
        *c = codec; *d = 0;
        if (q.empty()) return 0;
        auto& [bytes, disc] = q.front();
        *d = disc;
        uint32_t n = (uint32_t)std::min<size_t>(cap, bytes.size());
        std::memcpy(dst, bytes.data(), n);
        if (n == bytes.size()) q.pop_front();
        else { bytes.erase(bytes.begin(), bytes.begin() + n); disc = 0; }
        return n;
    }
    RecOptions wire(RecOptions opt) {               // convenience: bind + copy_mode
        opt.copy_mode = true;
        opt.pull = [this](uint8_t* dst, uint32_t cap, int32_t* c, int32_t* d) {
            return pull(dst, cap, c, d);
        };
        return opt;
    }
};

static std::vector<uint8_t> readAll(const std::string& p) {
    std::ifstream f(fs::path(p), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}
// Audio bytes of an MP3 copy cut: everything after the leading ID3v2 tag the
// tag pass prepended (the byte-fidelity comparand).
static std::vector<uint8_t> mp3AudioBytes(const std::string& p) {
    auto all = readAll(p);
    uint32_t skip = framesync::id3v2Size(all.data(), all.size());
    return std::vector<uint8_t>(all.begin() + skip, all.end());
}
// mdat payload of an .m4a (scanned by box walk, position-independent — the
// tag pass may shift boxes).
static std::vector<uint8_t> m4aMdatBytes(const std::string& p) {
    auto all = readAll(p);
    size_t i = 0;
    while (i + 8 <= all.size()) {
        uint32_t sz = ((uint32_t)all[i] << 24) | ((uint32_t)all[i+1] << 16) |
                      ((uint32_t)all[i+2] << 8) | all[i+3];
        if (std::memcmp(&all[i+4], "mdat", 4) == 0 && sz >= 8 && i + sz <= all.size())
            return std::vector<uint8_t>(all.begin() + i + 8, all.begin() + i + sz);
        if (sz < 8) break;
        i += sz;
    }
    return {};
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
    bool has_art = false;      // rec-cover-art: an APIC frame is present
    unsigned art_size = 0;
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
    for (auto* fr : tag->frameList("APIC")) {
        auto* ap = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(fr);
        if (ap) { out.has_art = true; out.art_size = ap->picture().size(); }
    }
    return out;
}

struct OpusTags {
    std::string title, artist;
    bool has_r128 = false;
    int  r128 = 0;
    int  length_sec = -1;
    bool has_art = false;      // rec-cover-art: a picture block is present
    unsigned art_size = 0;
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
    const auto pics = tag->pictureList();
    if (!pics.isEmpty()) { out.has_art = true; out.art_size = pics.front()->data().size(); }
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
        opt.split_offset_ms = 0;   // split-trim regression anchor: today's timing
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
        opt.split_offset_ms = 0;   // split-trim regression anchor: today's timing
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
        opt.split_offset_ms = 0;   // split-trim regression anchor: today's timing
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
        opt.split_offset_ms = 0;   // split-trim regression anchor: today's timing
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

    // ── 4b. cover art (rec-cover-art): key-match embed, both dialects ────────
    // No network anywhere: onArt is driven directly with synthetic bytes, the
    // way the UI tick feeds it from the radio-art cache in production.
    {
        std::vector<uint8_t> jpeg = { 0xFF, 0xD8, 0xFF, 0xE0 };
        jpeg.resize(256, 0x42);                       // magic + padding = "a jpeg"
        std::vector<uint8_t> junk(256, 0x00);         // no magic: must be rejected

        // (a) matching key -> picture embedded in BOTH formats.
        {
            StreamRecorder rec;
            RecOptions opt; opt.formats = { RipFormat::Mp3, RipFormat::Opus }; opt.split_offset_ms = 0;
            CHECK(rec.start(opt, "Test FM", root));
            uint64_t phase = 0;
            rec.onTitle("Art Artist - Art Song");
            rec.onArt("Art Artist - Art Song", jpeg);
            pushSine(rec, phase, 22050, 440.0, 0.5);
            CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 22050; }));
            rec.stop();
            auto cuts = rec.cuts();
            CHECK(cuts.size() == 1 && cuts[0].paths.size() == 2);
            if (cuts.size() == 1 && cuts[0].paths.size() == 2) {
                std::string mp3p, opusp;
                for (const auto& p : cuts[0].paths)
                    (p.size() > 4 && p.substr(p.size()-4) == ".mp3" ? mp3p : opusp) = p;
                Mp3Tags mt = readMp3(mp3p);
                CHECK(mt.has_art && mt.art_size == jpeg.size());
                OpusTags ot = readOpus(opusp);
                CHECK(ot.has_art && ot.art_size == jpeg.size());
            }
        }
        // (b) mismatched key -> NO picture (no art rather than wrong art);
        // (c) garbage bytes on a matching key -> rejected at onArt, NO picture,
        //     cut still fully valid and tagged.
        {
            StreamRecorder rec;
            RecOptions opt; opt.formats = { RipFormat::Opus }; opt.split_offset_ms = 0;
            CHECK(rec.start(opt, "Test FM", root));
            uint64_t phase = 0;
            rec.onTitle("Real Artist - Real Song");
            rec.onArt("Somebody Else - Other Song", jpeg);   // (b) stale/mismatch
            rec.onArt("Real Artist - Real Song", junk);      // (c) not an image
            pushSine(rec, phase, 22050, 440.0, 0.5);
            CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 22050; }));
            rec.stop();
            auto cuts = rec.cuts();
            CHECK(cuts.size() == 1 && !cuts[0].failed);
            if (cuts.size() == 1 && !cuts[0].paths.empty()) {
                OpusTags ot = readOpus(cuts[0].paths[0]);
                CHECK(!ot.has_art);
                CHECK(ot.title == "Real Song" && ot.artist == "Real Artist");
            }
        }
    }

    // ── 4b-hold. cover art survives the split-trim HOLD (rec-art-pending-race-fix)
    // The bug: with the default hold, song A's cut stays open while song B's art
    // resolves and clobbers the single pending-art slot, so A's held cut rolls
    // artless. The ring keeps each song's art keyed by its raw title. Modeled
    // with real ordering (B's art lands WHILE A is still held) and DISTINCT art
    // sizes so cross-contamination is caught, not just presence. A is inverted,
    // B forward - proves the fix is independent of the deinvert path. This is
    // the case 4b (split_offset_ms = 0) never exercised.
    {
        std::vector<uint8_t> artA = { 0xFF, 0xD8, 0xFF, 0xE0 }; artA.resize(300, 0x11);
        std::vector<uint8_t> artB = { 0xFF, 0xD8, 0xFF, 0xE0 }; artB.resize(500, 0x22);
        StreamRecorder rec;
        RecOptions opt; opt.formats = { RipFormat::Opus };
        opt.split_offset_ms = 1200;                       // the shipped default hold
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("Shins, The - New Slang");            // song A (inverted)
        rec.onArt("Shins, The - New Slang", artA);        // A's art (pane-confirmed)
        pushSine(rec, phase, 44100, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 44100; }));
        rec.onTitle("Sia - Unstoppable");                 // song B commits -> hold arms
        // B's fetch is in flight; the broadcast keeps flowing to the held A cut.
        pushSine(rec, phase, 22050, 880.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 44100 + 22050; }));
        rec.onArt("Sia - Unstoppable", artB);             // B's art lands DURING A's hold
        // Past the 1200 ms hold (52920 frames after B's title): A rolls, the
        // rest attributes to B. With the OLD single slot, A's art was already
        // clobbered by B's here; with the ring, A keeps A's.
        pushSine(rec, phase, 66150, 880.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.cutIndex() >= 1; }));
        rec.stop();                                       // B rolls as the tail cut
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 2);
        if (cuts.size() == 2) {
            // Each cut carries ITS OWN art, proven by the distinct sizes.
            OpusTags a = readOpus(cuts[0].paths[0]);
            CHECK(a.artist == "The Shins" && a.title == "New Slang");   // deinvert (filename/tag)
            CHECK(a.has_art && a.art_size == 300);                      // A's art, not B's
            OpusTags b = readOpus(cuts[1].paths[0]);
            CHECK(b.artist == "Sia" && b.title == "Unstoppable");
            CHECK(b.has_art && b.art_size == 500);                      // B's art, not A's
        }
    }

    // ── 4c. split-trim: the hold defers the roll FRAME-EXACTLY ───────────────
    {
        // offset 500 ms = 22050 frames: the closing cut keeps exactly that
        // much of the audio arriving after the title event (its outro).
        StreamRecorder rec;
        RecOptions opt; opt.formats = { RipFormat::Opus };
        opt.split_offset_ms = 500;
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("Hold A - One");
        pushSine(rec, phase, 44100, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 44100; }));
        rec.onTitle("Hold B - Two");              // flag set; hold arms
        pushSine(rec, phase, 44100, 880.0, 0.5);  // 22050 -> old cut, 22050 -> new
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 2ull * 44100; }));
        CHECK(waitFor(5000,  [&]{ return rec.cutIndex() == 1; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 2);
        if (cuts.size() == 2) {
            CHECK(cuts[0].frames == 44100 + 22050);   // outro protected, exact
            CHECK(cuts[1].frames == 44100 - 22050);
            CHECK(cuts[0].artist == "Hold A" && cuts[1].artist == "Hold B");
        }
    }
    {
        // stop() outranks the hold: a split still owing frames finalizes the
        // tail immediately, under the OLD title, as one cut.
        StreamRecorder rec;
        RecOptions opt; opt.formats = { RipFormat::Opus };
        opt.split_offset_ms = 5000;
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("Hold C - Three");
        pushSine(rec, phase, 22050, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 22050; }));
        rec.onTitle("Hold D - Four");
        pushSine(rec, phase, 4410, 440.0, 0.5);   // 0.1 s: the hold cannot expire
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 26460; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 1);
        if (cuts.size() == 1) {
            CHECK(cuts[0].frames == 26460);
            CHECK(cuts[0].artist == "Hold C");
        }
    }

    // ── 4d. ad-aware: classify + route (Save) / suppress (Discard) ──────────
    {
        // Save: non-song cuts route to <Station>/ads/ (sortable names, files
        // exist — routing never destroys); songs stay in main.
        StreamRecorder rec;
        RecOptions opt; opt.formats = { RipFormat::Opus }; opt.split_offset_ms = 0;
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("Song Guy - Good Tune");
        pushSine(rec, phase, 22050, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 22050; }));
        rec.onTitle("Test FM - LIVE");                    // iHeart structural floor
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 1; }));
        pushSine(rec, phase, 22050, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 44100; }));
        rec.onTitle("Spot Break - Advertisement Hour");   // ICY vocabulary
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 2; }));
        pushSine(rec, phase, 22050, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 66150; }));
        rec.onTitle("Song Gal - Next Tune");
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 3; }));
        pushSine(rec, phase, 22050, 440.0, 0.5);
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 4);
        CHECK(rec.adsSkipped() == 0);                     // Save never suppresses
        if (cuts.size() == 4) {
            auto inAds = [](const StreamRecorder::CutInfo& c) {
                return !c.paths.empty() && c.paths[0].find("ads") != std::string::npos;
            };
            CHECK(!cuts[0].is_ad && !inAds(cuts[0]));
            CHECK( cuts[1].is_ad &&  inAds(cuts[1]) && !cuts[1].discarded);
            CHECK( cuts[2].is_ad &&  inAds(cuts[2]) && !cuts[2].discarded);
            CHECK(!cuts[3].is_ad && !inAds(cuts[3]));
            for (const auto& c : cuts)
                for (const auto& p : c.paths) CHECK(fs::exists(p));
        }
    }
    {
        // Discard: suppressed cuts are counted (the trust surface) and never
        // written; unparseable titles stay MAIN even under Discard; and the
        // classification COMPOSES with a split-trim hold (trim = when,
        // ad-aware = where).
        StreamRecorder rec;
        RecOptions opt; opt.formats = { RipFormat::Opus };
        opt.split_offset_ms = 500;                        // 22050-frame hold
        opt.ads_discard = true;
        CHECK(rec.start(opt, "Test FM", root));
        uint64_t phase = 0;
        rec.onTitle("Song Guy - Good Tune");
        pushSine(rec, phase, 44100, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 44100; }));
        rec.onTitle("Test FM - LIVE");                    // held, then suppressed
        pushSine(rec, phase, 44100, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 2ull * 44100; }));
        CHECK(waitFor(5000,  [&]{ return rec.cutIndex() == 1; }));
        rec.onTitle("PlainTitleNoDash");                  // unparseable -> MAIN
        pushSine(rec, phase, 44100, 440.0, 0.5);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 3ull * 44100; }));
        CHECK(waitFor(5000,  [&]{ return rec.cutIndex() == 2; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 3);
        CHECK(rec.adsSkipped() == 1);
        if (cuts.size() == 3) {
            CHECK(!cuts[0].discarded && cuts[0].frames == 44100 + 22050); // trim held
            CHECK( cuts[1].discarded && cuts[1].paths.empty());
            CHECK( cuts[1].frames == 44100);              // counted, not written
            CHECK(!cuts[2].discarded && !cuts[2].paths.empty());
            CHECK( cuts[2].frames == 22050);
            CHECK( cuts[2].paths[0].find("ads") == std::string::npos);
            for (const auto& p : cuts[0].paths) CHECK(fs::exists(p));
            for (const auto& p : cuts[2].paths) CHECK(fs::exists(p));
        }
    }

    // ── 4e. copy mode (abi-cluster slice B): byte fidelity + frame-exact
    // splits through the injected pull — no plugin, no network, no decode ────
    {
        // Three-cut MP3 copy session: every emitted cut's audio bytes are the
        // EXACT bytes pushed for it (the copy claim, literal), splits land on
        // frame boundaries by construction, partial marking matches PCM mode.
        StreamRecorder rec;
        FakePull fp;                                  // codec defaults to MP3
        RecOptions opt = fp.wire(RecOptions{});
        opt.split_offset_ms = 0;
        CHECK(rec.start(opt, "Copy FM", root));
        CHECK(rec.recording());

        auto fr1 = mp3Frame(0x11), fr2 = mp3Frame(0x22), fr3 = mp3Frame(0x33);
        rec.onTitle("Copy One - Song One");
        fp.pushFrames(fr1, 100);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 100ull * 1152; }));
        rec.onTitle("Copy Two - Song Two");
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 1; }));
        fp.pushFrames(fr2, 100);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 200ull * 1152; }));
        rec.onTitle("Copy Three - Song Three");
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 2; }));
        fp.pushFrames(fr3, 50);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 250ull * 1152; }));
        rec.stop();
        CHECK(rec.lastError().empty());
        // elapsed uses the BROADCAST rate: 250*1152/44100 = 6.53.. -> 6
        CHECK(rec.elapsedSec() == 6);

        auto cuts = rec.cuts();
        CHECK(cuts.size() == 3);
        if (cuts.size() == 3) {
            CHECK(cuts[0].frames == 100ull * 1152);
            CHECK(cuts[1].frames == 100ull * 1152);
            CHECK(cuts[2].frames ==  50ull * 1152);
            CHECK(cuts[0].partial && !cuts[1].partial && cuts[2].partial);
            for (const auto& c : cuts) {
                CHECK(c.paths.size() == 1);
                CHECK(!c.paths.empty() && c.paths[0].size() > 4 &&
                      c.paths[0].substr(c.paths[0].size() - 4) == ".mp3");
            }
            // Byte fidelity on the clean middle cut: audio bytes == the 100
            // pushed frames, verbatim.
            std::vector<uint8_t> want;
            for (int i = 0; i < 100; ++i) want.insert(want.end(), fr2.begin(), fr2.end());
            CHECK(mp3AudioBytes(cuts[1].paths[0]) == want);
            // ...and the tag pass ran (ID3v2, no RG frame by design).
            Mp3Tags mt = readMp3(cuts[1].paths[0]);
            CHECK(mt.title == "Song Two" && mt.artist == "Copy Two");
            CHECK(!mt.has_rg);                        // no ReplayGain on copy
        }
    }
    {
        // Split-trim hold counts down in encoded-frame DURATIONS: 500 ms at
        // 44100 = 22050 samples; MP3 frames carry 1152 -> the roll snaps to
        // the 20th whole frame (19*1152=21888 < 22050 <= 20*1152).
        StreamRecorder rec;
        FakePull fp;
        RecOptions opt = fp.wire(RecOptions{});
        opt.split_offset_ms = 500;
        CHECK(rec.start(opt, "Copy FM", root));
        auto fr = mp3Frame(0x44);
        rec.onTitle("Hold Copy A - One");
        fp.pushFrames(fr, 100);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 100ull * 1152; }));
        rec.onTitle("Hold Copy B - Two");             // hold arms
        fp.pushFrames(fr, 100);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 200ull * 1152; }));
        CHECK(waitFor(5000,  [&]{ return rec.cutIndex() == 1; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 2);
        if (cuts.size() == 2) {
            CHECK(cuts[0].frames == 120ull * 1152);   // outro: whole-frame snap
            CHECK(cuts[1].frames ==  80ull * 1152);
            CHECK(cuts[0].artist == "Hold Copy A" && cuts[1].artist == "Hold Copy B");
        }
    }
    {
        // Discont resync: a flagged gap drops the orphaned partial frame and
        // the cut carries ONLY whole clean frames — no torn frame ever lands.
        StreamRecorder rec;
        FakePull fp;
        RecOptions opt = fp.wire(RecOptions{});
        opt.split_offset_ms = 0;
        CHECK(rec.start(opt, "Copy FM", root));
        auto fr = mp3Frame(0x55);
        rec.onTitle("Gap Artist - Gap Song");
        fp.pushFrames(fr, 50);
        // The gap chunk: 200 bytes of header-less tail (what a mid-frame
        // reconnect leaves) then 50 clean frames, discont-flagged.
        {
            std::vector<uint8_t> b(fr.begin() + 4, fr.begin() + 204);   // no sync
            for (int i = 0; i < 50; ++i) b.insert(b.end(), fr.begin(), fr.end());
            fp.push(std::move(b), 1);
        }
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 100ull * 1152; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 1);
        if (cuts.size() == 1) {
            CHECK(cuts[0].frames == 100ull * 1152);   // garbage never counted
            std::vector<uint8_t> want;
            for (int i = 0; i < 100; ++i) want.insert(want.end(), fr.begin(), fr.end());
            CHECK(mp3AudioBytes(cuts[0].paths[0]) == want);
        }
    }
    {
        // AAC copy -> .m4a: the mdat payload is the de-ADTS'd access units
        // verbatim (byte fidelity through the mux), and TagLib::MP4 reads the
        // tag pass's title/artist (decision #1: no untagged backlog). An
        // in-stream ID3 block must be stripped, never muxed.
        StreamRecorder rec;
        FakePull fp;
        fp.codec = 2;                                 // AAC ADTS
        RecOptions opt = fp.wire(RecOptions{});
        opt.split_offset_ms = 0;
        CHECK(rec.start(opt, "Copy FM", root));
        auto fr = adtsFrame(0x66);
        rec.onTitle("M4A Artist - M4A Song");
        fp.pushFrames(fr, 40);
        {
            // A stray in-stream ID3v2 tag between frames (transport metadata).
            std::vector<uint8_t> tag = { 'I','D','3', 3, 0, 0, 0, 0, 0, 20 };
            tag.resize(10 + 20, 0x00);
            fp.push(std::move(tag));
        }
        fp.pushFrames(fr, 40);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 80ull * 1024; }));
        rec.stop();
        CHECK(rec.lastError().empty());
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 1);
        if (cuts.size() == 1 && cuts[0].paths.size() == 1) {
            const std::string& p = cuts[0].paths[0];
            CHECK(p.size() > 4 && p.substr(p.size() - 4) == ".m4a");
            std::vector<uint8_t> want;                // 80 AUs, headers stripped
            for (int i = 0; i < 80; ++i) want.insert(want.end(), fr.begin() + 7, fr.end());
            CHECK(m4aMdatBytes(p) == want);
            TagLib::MP4::File f(TL_PATH(p), true);
            CHECK(f.isValid() && f.tag());
            if (f.isValid() && f.tag()) {
                CHECK(f.tag()->title().to8Bit(true)  == "M4A Song");
                CHECK(f.tag()->artist().to8Bit(true) == "M4A Artist");
            }
        }
    }
    {
        // Ad-aware composes in copy mode: Discard suppresses (counted, no
        // file), songs land tagged — the classifier is upstream of the writer.
        StreamRecorder rec;
        FakePull fp;
        RecOptions opt = fp.wire(RecOptions{});
        opt.split_offset_ms = 0;
        opt.ads_discard = true;
        CHECK(rec.start(opt, "Copy FM", root));
        auto fr = mp3Frame(0x77);
        rec.onTitle("Copy Guy - Fine Tune");
        fp.pushFrames(fr, 50);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 50ull * 1152; }));
        rec.onTitle("Copy FM - LIVE");                // structural ad floor
        CHECK(waitFor(5000, [&]{ return rec.cutIndex() == 1; }));
        fp.pushFrames(fr, 50);
        CHECK(waitFor(15000, [&]{ return rec.totalFrames() == 100ull * 1152; }));
        rec.stop();
        auto cuts = rec.cuts();
        CHECK(cuts.size() == 2);
        CHECK(rec.adsSkipped() == 1);
        if (cuts.size() == 2) {
            CHECK(!cuts[0].discarded && !cuts[0].paths.empty());
            CHECK( cuts[1].discarded &&  cuts[1].paths.empty());
            CHECK( cuts[1].frames == 50ull * 1152);   // counted, not written
        }
    }
    {
        // Copy-mode start() honesty: no pull -> a reasoned failure, not a crash.
        StreamRecorder rec;
        RecOptions opt;
        opt.copy_mode = true;
        CHECK(!rec.start(opt, "X", root));
        CHECK(!rec.lastError().empty());
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
