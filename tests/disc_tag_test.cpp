// disc_tag_test — P5: the disc number, per container, proven rather than asserted.
//
// RE-MOCT wrote no disc number at all until this slice. The risk in adding one is
// not the arithmetic — it is that each container stores it in a DIFFERENT native
// field (ID3v2 TPOS, Xiph DISCNUMBER, APEv2 DISC, MP4 "disk"), and RE-MOCT's own
// library reader asks TagLib for ONE key, "DISCNUMBER", through the PropertyMap
// (readTags, src/LibraryScanner.cpp). Whether TagLib bridges each native field
// onto that key is a property of TagLib, not of our code, and the only honest way
// to know it is to write a real file and read it back.
//
// So each container gets three assertions:
//   A. the NATIVE field holds exactly what we meant to write,
//   B. TagLib's PropertyMap surfaces it as DISCNUMBER, and
//   C. the SHIPPING scanner, run over a real directory, reports disc_no.
//
// (C) is the one that matters: it is scanCollection(), the same call [Library]
// makes, reading files this test produced. A test that re-implemented the reader
// would only prove it agreed with itself.
//
// WHAT THIS DOES NOT PROVE. The writes go through disctag::writeDiscTag — the
// function CDRipper::tagFile calls — but not through tagFile itself, which is
// private and whose translation unit drags in CD I/O, HTTP and the whole rip
// stack. So this pins the FORMAT and the MAPPING; that tagFile passes the right
// disc to the right file is verified by reading the five call sites and by the
// next real rip. Named here so nobody reads a green bar as more than it is.

#include "DiscTag.h"
#include "EncoderFactory.h"
#include "LibraryScanner.h"
#include "RipFormats.h"
#include "StringUtils.h"     // utf8_to_wide

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/opusfile.h>
#include <taglib/wavpackfile.h>
#include <taglib/apetag.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4item.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace libidx;

#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

static int g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& got = "") {
    std::printf("  [%s] %-56s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                got.empty() ? "" : "  got: ", got.c_str());
    if (!ok) ++g_fail;
}

// ── Fixture: a real encoded file per taggable container ─────────────────────
// Same construction art_embed_test uses — a deterministic noise buffer through
// the production encoder. The audio is irrelevant; the container must be real,
// because a tag written into a fake one proves nothing about the tagger.
static std::vector<int16_t> pcmFixture() {
    std::vector<int16_t> p(44100 * 2);
    uint32_t x = 0x44495343u;                       // 'DISC'
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

// ── The write, dispatched per container ─────────────────────────────────────
// This mirrors tagFile's format switch, but NOT its disc-tag write: that is
// disctag::writeDiscTag, the shipping function, called here unchanged. The
// dispatch is duplicated; the thing under test is not.
static bool tagDisc(const std::string& path, RipFormat fmt, int disc, int total) {
    switch (fmt) {
        case RipFormat::Flac: {
            TagLib::FLAC::File f(TL_PATH(path), false);
            auto* t = f.xiphComment(true); if (!t) return false;
            disctag::writeDiscTag(t, disc, total);
            return f.save();
        }
        case RipFormat::Mp3: {
            TagLib::MPEG::File f(TL_PATH(path), false);
            auto* t = f.ID3v2Tag(true); if (!t) return false;
            disctag::writeDiscTag(t, disc, total);
            return f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone,
                          TagLib::ID3v2::v4);
        }
        case RipFormat::Opus: {
            TagLib::Ogg::Opus::File f(TL_PATH(path), false);
            auto* t = f.tag(); if (!t) return false;
            disctag::writeDiscTag(t, disc, total);
            return f.save();
        }
        case RipFormat::WavPack: {
            TagLib::WavPack::File f(TL_PATH(path), false);
            auto* t = f.APETag(true); if (!t) return false;
            disctag::writeDiscTag(t, disc, total);
            return f.save();
        }
        case RipFormat::M4a: {
            TagLib::MP4::File f(TL_PATH(path), false);
            auto* t = f.tag(); if (!t) return false;
            disctag::writeDiscTag(t, disc, total);
            return f.save();
        }
        default: return false;
    }
}

// ── Native-field readers, one per container ─────────────────────────────────
static std::string xiphField(const std::string& p, bool opus, const char* key) {
    if (opus) {
        TagLib::Ogg::Opus::File f(TL_PATH(p), false);
        auto* t = f.tag(); if (!t || !t->contains(key)) return {};
        return t->fieldListMap()[key].front().to8Bit(true);
    }
    TagLib::FLAC::File f(TL_PATH(p), false);
    auto* t = f.xiphComment(false); if (!t || !t->contains(key)) return {};
    return t->fieldListMap()[key].front().to8Bit(true);
}

static std::string id3Tpos(const std::string& p, int* count = nullptr) {
    TagLib::MPEG::File f(TL_PATH(p), false);
    auto* t = f.ID3v2Tag(false); if (!t) return {};
    const auto& fl = t->frameList("TPOS");
    if (count) *count = (int)fl.size();
    if (fl.isEmpty()) return {};
    return fl.front()->toString().to8Bit(true);
}

static std::string apeDisc(const std::string& p) {
    TagLib::WavPack::File f(TL_PATH(p), false);
    auto* t = f.APETag(false); if (!t) return {};
    // itemListMap() upper-cases keys for lookup (same note ArtEmbed.cpp carries).
    const auto& m = t->itemListMap();
    auto it = m.find("DISC");
    if (it == m.end() || it->second.values().isEmpty()) return {};
    return it->second.values().front().to8Bit(true);
}

static std::string mp4Disk(const std::string& p) {
    TagLib::MP4::File f(TL_PATH(p), false);
    auto* t = f.tag(); if (!t || !t->contains("disk")) return {};
    auto pr = t->item("disk").toIntPair();
    return std::to_string(pr.first) + "/" + std::to_string(pr.second);
}

// The key RE-MOCT's library reader asks for — see readTags in LibraryScanner.cpp.
static std::string propDiscNumber(const std::string& p) {
    TagLib::FileRef f(TL_PATH(p), false);
    if (f.isNull() || !f.file()) return {};
    const auto pm = f.file()->properties();
    auto it = pm.find("DISCNUMBER");
    if (it == pm.end() || it->second.isEmpty()) return {};
    return it->second.front().to8Bit(true);
}

struct Container { RipFormat fmt; const char* ext; const char* name; };
static const Container kContainers[] = {
    { RipFormat::Flac,    ".flac", "FLAC"    },
    { RipFormat::Mp3,     ".mp3",  "MP3"     },
    { RipFormat::Opus,    ".opus", "Opus"    },
    { RipFormat::WavPack, ".wv",   "WavPack" },
    { RipFormat::M4a,     ".m4a",  "M4A"     },
};

// Build one tagged file per container in `dir`; returns false if any step failed.
static bool buildSet(const fs::path& dir, int disc, int total) {
    std::error_code ec; fs::create_directories(dir, ec);
    bool all = true;
    for (const auto& c : kContainers) {
        const std::string p = (dir / (std::string("track") + c.ext)).string();
        if (!synth(p, c.fmt)) {
            check(false, std::string("synth ") + c.name); all = false; continue;
        }
        if (!tagDisc(p, c.fmt, disc, total)) {
            check(false, std::string("write disc tag ") + c.name); all = false;
        }
    }
    return all;
}

// Run the SHIPPING scanner over `dir` and return ext -> disc_no.
static std::vector<std::pair<std::string,int>> scanDiscNumbers(const fs::path& dir) {
    ScanProgress pr;
    ScanOutcome out = scanCollection({ dir.string() }, LibraryIndex{}, pr);
    std::vector<std::pair<std::string,int>> v;
    if (!out.completed) return v;
    for (const auto& t : out.index.tracks) {
        auto dot = t.path.rfind('.');
        v.emplace_back(dot == std::string::npos ? "" : t.path.substr(dot),
                       (int)t.disc_no);
    }
    return v;
}

static int discNoFor(const std::vector<std::pair<std::string,int>>& v,
                     const std::string& ext) {
    for (const auto& e : v) if (e.first == ext) return e.second;
    return -1;   // not indexed at all
}

int main() {
    fs::path root = fs::temp_directory_path() / "remoct_disc_tag_test";
    std::error_code ec; fs::remove_all(root, ec);

    // ── 1. Disc 3 of 7: the native field, per container ──────────────────────
    std::printf("\n-- disc 3 of 7, native fields --\n");
    const fs::path multi = root / "multi";
    buildSet(multi, 3, 7);
    auto MP = [&](const char* ext) { return (multi / (std::string("track") + ext)).string(); };

    check(xiphField(MP(".flac"), false, "DISCNUMBER") == "3" &&
          xiphField(MP(".flac"), false, "TOTALDISCS") == "7",
          "FLAC  Xiph DISCNUMBER=3 TOTALDISCS=7",
          xiphField(MP(".flac"), false, "DISCNUMBER") + "/" +
          xiphField(MP(".flac"), false, "TOTALDISCS"));
    check(id3Tpos(MP(".mp3")) == "3/7", "MP3   ID3v2 TPOS = 3/7", id3Tpos(MP(".mp3")));
    check(xiphField(MP(".opus"), true, "DISCNUMBER") == "3" &&
          xiphField(MP(".opus"), true, "TOTALDISCS") == "7",
          "Opus  Xiph DISCNUMBER=3 TOTALDISCS=7",
          xiphField(MP(".opus"), true, "DISCNUMBER") + "/" +
          xiphField(MP(".opus"), true, "TOTALDISCS"));
    check(apeDisc(MP(".wv")) == "3/7", "WvPk  APEv2 DISC = 3/7", apeDisc(MP(".wv")));
    check(mp4Disk(MP(".m4a")) == "3/7", "M4A   MP4 disk = (3,7)", mp4Disk(MP(".m4a")));

    // ── 2. TagLib's PropertyMap bridges each one onto DISCNUMBER ─────────────
    // The mapping this slice cannot assert and must measure. A failure here is
    // container-specific and says exactly which native field TagLib does not
    // bridge — which is the information needed to pick a different one.
    std::printf("\n-- PropertyMap[\"DISCNUMBER\"] (the key LibraryScanner reads) --\n");
    for (const auto& c : kContainers) {
        const std::string got = propDiscNumber(MP(c.ext));
        check(!got.empty() && got.rfind("3", 0) == 0,
              std::string(c.name) + " surfaces DISCNUMBER starting \"3\"",
              got.empty() ? "(absent)" : got);
    }

    // ── 3. The shipping scanner reports disc_no ──────────────────────────────
    std::printf("\n-- scanCollection() -> LibraryTrack::disc_no --\n");
    {
        auto v = scanDiscNumbers(multi);
        check(v.size() == 5, "all five containers indexed",
              std::to_string(v.size()));
        for (const auto& c : kContainers)
            check(discNoFor(v, c.ext) == 3,
                  std::string(c.name) + " scans as disc_no 3",
                  std::to_string(discNoFor(v, c.ext)));
    }

    // ── 4. 1/1 is WRITTEN, not omitted ───────────────────────────────────────
    // Dos's ruling, and the whole reason absence can now mean one thing. If a
    // single-disc rip left the tag out, "no DISCNUMBER" would still mean both
    // "standalone disc" and "ripped before this shipped".
    std::printf("\n-- disc 1 of 1 is present, not absent --\n");
    {
        const fs::path single = root / "single";
        buildSet(single, 1, 1);
        auto SP = [&](const char* ext) { return (single / (std::string("track") + ext)).string(); };
        for (const auto& c : kContainers) {
            const std::string got = propDiscNumber(SP(c.ext));
            check(!got.empty() && got.rfind("1", 0) == 0,
                  std::string(c.name) + " single-disc writes DISCNUMBER \"1\"",
                  got.empty() ? "(absent)" : got);
        }
        auto v = scanDiscNumbers(single);
        for (const auto& c : kContainers)
            check(discNoFor(v, c.ext) == 1,
                  std::string(c.name) + " single-disc scans as disc_no 1",
                  std::to_string(discNoFor(v, c.ext)));
    }

    // ── 5. Edges: the guard, the clamp, and no duplicate frame ───────────────
    std::printf("\n-- guard, clamp, idempotence --\n");
    {
        const fs::path edge = root / "edge";
        std::error_code e2; fs::create_directories(edge, e2);

        // disc 0 is not a disc: writes nothing rather than a zero a reader would
        // have to interpret.
        const std::string z = (edge / "zero.flac").string();
        synth(z, RipFormat::Flac);
        tagDisc(z, RipFormat::Flac, 0, 7);
        check(propDiscNumber(z).empty(), "disc 0 writes no DISCNUMBER at all",
              propDiscNumber(z));

        // "3 of 1" is not representable; the total clamps up to the disc.
        const std::string c1 = (edge / "clamp.mp3").string();
        synth(c1, RipFormat::Mp3);
        tagDisc(c1, RipFormat::Mp3, 3, 1);
        check(id3Tpos(c1) == "3/3", "total below disc clamps to 3/3", id3Tpos(c1));

        // ID3v2 addFrame APPENDS, so the writer removes first. A duplicate TPOS
        // is a legal tag that different players resolve differently.
        const std::string tw = (edge / "twice.mp3").string();
        synth(tw, RipFormat::Mp3);
        tagDisc(tw, RipFormat::Mp3, 2, 5);
        tagDisc(tw, RipFormat::Mp3, 3, 7);
        int n = 0; const std::string last = id3Tpos(tw, &n);
        check(n == 1 && last == "3/7", "second write replaces, one TPOS frame",
              std::to_string(n) + " frame(s), " + last);
    }

    fs::remove_all(root, ec);
    std::printf("\n%s (%d failure%s)\n",
                g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
