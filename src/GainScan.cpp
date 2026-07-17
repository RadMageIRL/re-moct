// GainScan.cpp — batch ReplayGain engine (batch-r128). See the header for
// the thesis: nothing here is new machinery, it is the fourth assembly of
// the rip/recorder/BPM shapes with known behavior.
#include "GainScan.h"
#include "LocalFileSource.h"
#include "R128Gain.h"      // r128FromDb — the one home for the Opus dialect
#include "StringUtils.h"   // utf8_to_wide
#include "PortUtil.h"      // sleepMs

#include <ebur128.h>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/mpegfile.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/opusfile.h>      // TagLib's own (collision-safe: taglib/ prefixed)
#include <taglib/wavpackfile.h>
#include <taglib/apetag.h>
#include <taglib/mp4file.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

namespace {

// The rip's exact formatters (tagFile / tagCut — parity is textual too).
std::string rgStr(double gain) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(2) << gain << " dB";
    return ss.str();
}
std::string rgPeakStr(double peak) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(6) << peak;
    return ss.str();
}

std::string lowerExt(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

// The taggable set = the rip-taggable four + MP4 (Dos's decision: the
// audiobook corner benefits). .wav is recognized separately for the note
// (untagged by rip policy); other audio extensions are simply not scanned.
bool taggableExt(const std::string& e) {
    return e == ".flac" || e == ".mp3" || e == ".opus" || e == ".wv" ||
           e == ".m4a" || e == ".m4b";
}

// Skip predicate: does the file already carry a track-gain tag in ITS
// dialect? PropertyMap spans Xiph/ID3v2/APEv2/MP4-freeform — the player's
// own read logic. Pre-fix BLOB MP3s are PropertyMap-invisible and thus
// "untagged" here: the scan heals them (the flagged side-effect).
bool hasTrackGain(const std::string& path, const std::string& ext) {
    TagLib::FileRef ref(TL_PATH(path), false);
    if (ref.isNull() || !ref.file()) return false;
    const char* key = (ext == ".opus") ? "R128_TRACK_GAIN" : "REPLAYGAIN_TRACK_GAIN";
    return !ref.file()->properties()[key].isEmpty();
}

// ── the dialect writers (tagCut/tagFile shapes, gain/peak only) ─────────────
bool writeFlac(const std::string& p, double gain, double peak) {
    TagLib::FLAC::File f(TL_PATH(p), false);
    if (!f.isValid()) return false;
    auto* tag = f.xiphComment(true); if (!tag) return false;
    tag->addField("REPLAYGAIN_TRACK_GAIN", TagLib::String(rgStr(gain),     TagLib::String::UTF8), true);
    tag->addField("REPLAYGAIN_TRACK_PEAK", TagLib::String(rgPeakStr(peak), TagLib::String::UTF8), true);
    return f.save();
}

bool writeMp3(const std::string& p, double gain, double peak) {
    TagLib::MPEG::File f(TL_PATH(p), false);
    if (!f.isValid()) return false;
    auto* tag = f.ID3v2Tag(true); if (!tag) return false;
    // Heal: remove EVERY prior REPLAYGAIN_* TXXX — both the pre-fix blob
    // shape (the whole "KEY=value" in the description) and any proper frame
    // being force-replaced — so exactly one standard pair remains.
    std::vector<TagLib::ID3v2::Frame*> doomed;
    for (auto* fr : tag->frameList("TXXX")) {
        auto* u = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(fr);
        if (u && u->description().to8Bit(true).rfind("REPLAYGAIN_", 0) == 0)
            doomed.push_back(fr);
    }
    for (auto* fr : doomed) tag->removeFrame(fr);
    auto addUserTxt = [&](const char* desc, const std::string& val) {
        auto* f2 = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
        f2->setDescription(desc);
        f2->setText(TagLib::String(val, TagLib::String::UTF8));
        tag->addFrame(f2);
    };
    addUserTxt("REPLAYGAIN_TRACK_GAIN", rgStr(gain));
    addUserTxt("REPLAYGAIN_TRACK_PEAK", rgPeakStr(peak));
    return f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
}

bool writeOpus(const std::string& p, double gain) {   // R128 dialect; no peak
    TagLib::Ogg::Opus::File f(TL_PATH(p), false);
    if (!f.isValid()) return false;
    auto* tag = f.tag(); if (!tag) return false;
    tag->addField("R128_TRACK_GAIN",
        TagLib::String(std::to_string(r128FromDb(gain)), TagLib::String::UTF8), true);
    return f.save();
}

bool writeWv(const std::string& p, double gain, double peak) {
    TagLib::WavPack::File f(TL_PATH(p), false);
    if (!f.isValid()) return false;
    auto* tag = f.APETag(true); if (!tag) return false;
    tag->addValue("REPLAYGAIN_TRACK_GAIN", TagLib::String(rgStr(gain),     TagLib::String::UTF8), true);
    tag->addValue("REPLAYGAIN_TRACK_PEAK", TagLib::String(rgPeakStr(peak), TagLib::String::UTF8), true);
    return f.save();
}

bool writeM4a(const std::string& p, double gain, double peak) {
    // MP4: the PropertyMap route (freeform REPLAYGAIN atoms) — the same
    // surface the player's read path consults.
    TagLib::MP4::File f(TL_PATH(p), false);
    if (!f.isValid()) return false;
    auto pm = f.properties();
    pm.replace("REPLAYGAIN_TRACK_GAIN", TagLib::StringList(TagLib::String(rgStr(gain),     TagLib::String::UTF8)));
    pm.replace("REPLAYGAIN_TRACK_PEAK", TagLib::StringList(TagLib::String(rgPeakStr(peak), TagLib::String::UTF8)));
    f.setProperties(pm);
    return f.save();
}

} // namespace

GainScan::~GainScan() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

bool GainScan::start(const std::string& dir, bool force) {
    if (running_.load()) return false;
    if (worker_.joinable()) worker_.join();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    cancel_.store(false);
    finished_.store(false);
    total_.store(0); index_.store(0);
    tagged_.store(0); skipped_.store(0); errors_.store(0); wav_noted_.store(0);
    running_.store(true);
    worker_ = std::thread(&GainScan::workerLoop, this, dir, force);
    return true;
}

void GainScan::cancel() { cancel_.store(true); }

std::string GainScan::currentFile() const {
    std::lock_guard<std::mutex> lk(file_mtx_);
    return current_file_;
}

void GainScan::workerLoop(std::string dir, bool force) {
    // Enumerate on the worker (a huge folder must not hitch the UI).
    files_.clear();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string p = it->path().string();
        std::string e = lowerExt(p);
        if (taggableExt(e)) files_.push_back(p);
        else if (e == ".wav") wav_noted_.fetch_add(1);   // untagged by rip policy too
    }
    std::sort(files_.begin(), files_.end());
    total_.store((int)files_.size());

    for (const auto& p : files_) {
        if (cancel_.load(std::memory_order_relaxed)) break;
        {
            std::lock_guard<std::mutex> lk(file_mtx_);
            current_file_ = fs::path(p).filename().string();
        }
        processFile(p, force);
        index_.fetch_add(1);
    }
    {
        std::lock_guard<std::mutex> lk(file_mtx_);
        current_file_.clear();
    }
    running_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
}

bool GainScan::processFile(const std::string& path, bool force) {
    const std::string ext = lowerExt(path);
    if (!force && hasTrackGain(path, ext)) {
        skipped_.fetch_add(1);
        return true;
    }

    // Decode-for-measurement: a private LocalFileSource, fully drained (the
    // BPM-detection twin). The playback contract is 44100/stereo f32, so the
    // ebur128 setup is IDENTICAL to the rip's — parity by construction.
    LocalFileSource src;
    if (!src.open(path)) { errors_.fetch_add(1); return false; }

    ebur128_state* st = ebur128_init(2, 44100,
                                     EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    if (!st) { errors_.fetch_add(1); return false; }

    std::vector<float> buf((size_t)4096 * 2);
    bool aborted = false;
    for (;;) {
        uint32_t got = src.readFrames(buf.data(), 4096);
        if (got == 0) break;
        ebur128_add_frames_float(st, buf.data(), (size_t)got);
        // Cancel abandons the in-flight file entirely — nothing is written
        // for it, so every stop path leaves whole files only (a 2-hour
        // audiobook must not pin the cancel for minutes).
        if (cancel_.load(std::memory_order_relaxed)) { aborted = true; break; }
    }
    if (aborted) { ebur128_destroy(&st); return false; }

    // The rip's exact result math (CDRipper/rollCut): RG ref -18 LUFS,
    // guarded against silence/too-short; peak = max true-peak L/R.
    double loudness = 0, gain = 0, peak = 0;
    bool valid = false;
    if (ebur128_loudness_global(st, &loudness) == EBUR128_SUCCESS &&
        std::isfinite(loudness) && loudness > -70.0) {
        gain = -18.0 - loudness;
        double pl = 0, pr = 0;
        ebur128_true_peak(st, 0, &pl);
        ebur128_true_peak(st, 1, &pr);
        peak  = std::max(pl, pr);
        valid = true;
    }
    ebur128_destroy(&st);
    if (!valid) { errors_.fetch_add(1); return false; }

    bool ok = false;
    if      (ext == ".flac")                 ok = writeFlac(path, gain, peak);
    else if (ext == ".mp3")                  ok = writeMp3(path, gain, peak);
    else if (ext == ".opus")                 ok = writeOpus(path, gain);
    else if (ext == ".wv")                   ok = writeWv(path, gain, peak);
    else if (ext == ".m4a" || ext == ".m4b") ok = writeM4a(path, gain, peak);

    if (ok) tagged_.fetch_add(1); else errors_.fetch_add(1);
    return ok;
}
