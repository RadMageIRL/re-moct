// ConvertJob.cpp - the convert-core batch engine. See ConvertJob.h for the
// thesis: nothing here is new machinery, it is the GainScan/BPM worker shape
// (a private LocalFileSource drained on a worker) feeding the shared makeEncoder
// seam, with a text-tag carryover pass. The audio thread is never involved.
#include "ConvertJob.h"

#include "LocalFileSource.h"
#include "EncoderFactory.h"   // makeEncoder (now external linkage)
#include "IEncoder.h"
#include "ArtEmbed.h"         // extractEmbeddedArt / embedArt (art carryover)
#include "StringUtils.h"      // utf8_to_wide

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace fs = std::filesystem;

#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

namespace {

std::string lowerExt(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

// f32 -> s16, the exact inverse of miniaudio's s16->f32 (x = V / 32768): round
// to nearest and clamp. For a sample that originated as s16 (e.g. a WAV) this
// recovers the integer bit-for-bit (V/32768 is exactly representable, and
// lrintf(V) == V), so a WAV convert round-trips exactly; a general lossy f32 is
// rounded deterministically with no dither.
inline int16_t f32_to_s16(float x) {
    long v = std::lrintf(x * 32768.0f);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

// Carry the source's TEXT tags to the output via the PropertyMap surface the
// player reads (artist/title/album/track/date/genre/...). PropertyMap does NOT
// carry cover art - art carryover is a deferred follow-up slice. Best-effort:
// the audio on disk is already valid; a tag failure must not fail the convert.
void copyTextTags(const std::string& src, const std::string& dst) {
    try {
        TagLib::FileRef in(TL_PATH(src), false);
        if (in.isNull() || !in.file()) return;
        TagLib::PropertyMap props = in.file()->properties();
        if (props.isEmpty()) return;
        TagLib::FileRef out(TL_PATH(dst), false);
        if (out.isNull() || !out.file()) return;
        out.file()->setProperties(props);
        out.file()->save();
    } catch (...) {}
}

} // namespace

std::string convertDstPath(const std::string& src, RipFormat fmt) {
    const RipFormatRow* row = ripFormatRow(fmt);
    if (!row) return {};
    fs::path p(src);
    p.replace_extension(row->ext);   // row->ext carries the leading dot
    return p.string();
}

bool convertSupportedInput(const std::string& path) {
    const std::string e = lowerExt(path);
    // The decodable set (LocalFileSource / custom backends): the rip-taggable
    // formats plus WAV and the MP4/AAC family the FDK backend handles.
    return e == ".flac" || e == ".mp3"  || e == ".wav" || e == ".opus" ||
           e == ".wv"   || e == ".m4a"  || e == ".m4b" || e == ".mp4"  ||
           e == ".aac";
}

ConvertJob::Result ConvertJob::convertOne(const std::string& src, const std::string& dst,
                                          RipFormat fmt, const RipOptions& opt,
                                          const std::atomic<bool>* cancel) {
    std::error_code ec1, ec2;
    // Guard 1: never write over the source (e.g. flac -> flac in place). Compare
    // weakly-canonical paths so ./a.flac and a.flac resolve equal.
    fs::path cs = fs::weakly_canonical(fs::path(src), ec1);
    fs::path cd = fs::weakly_canonical(fs::path(dst), ec2);
    if (!ec1 && !ec2 && cs == cd) return Result::SkippedSamePath;
    if (src == dst)               return Result::SkippedSamePath;
    // Guard 2: never clobber a prior output.
    std::error_code exec;
    if (fs::exists(fs::path(dst), exec)) return Result::SkippedExists;

    // Decode: a private LocalFileSource, fully drained (the GainScan/BPM twin).
    LocalFileSource in;
    if (!in.open(src)) return Result::Error;

    auto enc = makeEncoder(fmt, opt);
    const uint64_t est_frames = (uint64_t)(in.durationSec() * 44100.0 + 0.5);
    if (!enc || !enc->open(dst, est_frames)) return Result::Error;

    std::vector<float>   f((size_t)4096 * 2);
    std::vector<int16_t> s((size_t)4096 * 2);
    bool ok = true;
    for (;;) {
        uint32_t got = in.readFrames(f.data(), 4096);
        if (got == 0) break;
        const size_t n = (size_t)got * 2;
        for (size_t i = 0; i < n; ++i) s[i] = f32_to_s16(f[i]);
        if (!enc->writeFrames(s.data(), got)) { ok = false; break; }
        // Cancel abandons the in-flight file entirely (nothing valid is left) -
        // a long file must not pin the cancel for the whole decode.
        if (cancel && cancel->load(std::memory_order_relaxed)) { ok = false; break; }
    }
    const bool completed = enc->finalize(ok) && ok;
    in.close();
    if (!completed) {
        std::error_code rmec;
        fs::remove(fs::path(dst), rmec);   // no partial/torn file left behind
        return Result::Error;
    }
    copyTextTags(src, dst);
    // Embedded cover art carryover (art-embed-shared): pull the source's own
    // picture across to the output. Best-effort - a source with no art, or an
    // embed failure, never fails the convert.
    if (auto art = extractEmbeddedArt(src)) embedArt(dst, fmt, *art);
    return Result::Converted;
}

ConvertJob::~ConvertJob() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

bool ConvertJob::startPairs(std::vector<ConvertPair> pairs, RipFormat fmt, RipOptions opt) {
    if (running_.load()) return false;
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    finished_.store(false);
    total_.store(0); index_.store(0);
    converted_.store(0); skipped_.store(0); errors_.store(0);
    running_.store(true);
    worker_ = std::thread(&ConvertJob::workerLoop, this, std::move(pairs), fmt, std::move(opt));
    return true;
}

bool ConvertJob::startFiles(std::vector<ConvertPair> pairs, RipFormat fmt, RipOptions opt) {
    return startPairs(std::move(pairs), fmt, std::move(opt));
}

bool ConvertJob::startFolder(const std::string& dir, RipFormat fmt, RipOptions opt) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    // Enumerate FLAT (one level, non-recursive) - a nested directory's files are
    // never included. This is the guard that keeps recursive convert out of the
    // slice. Enumeration is cheap for one level; kept on the caller here (unlike
    // GainScan's worker-side scan) since flat folders are bounded by design.
    std::vector<ConvertPair> pairs;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;   // regular files only -> subdirs skipped
        std::string p = it->path().string();
        if (!convertSupportedInput(p)) continue;
        pairs.push_back({ p, convertDstPath(p, fmt) });
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const ConvertPair& a, const ConvertPair& b) { return a.src < b.src; });
    return startPairs(std::move(pairs), fmt, std::move(opt));
}

void ConvertJob::cancel() { cancel_.store(true); }

std::string ConvertJob::currentFile() const {
    std::lock_guard<std::mutex> lk(file_mtx_);
    return current_file_;
}

void ConvertJob::workerLoop(std::vector<ConvertPair> pairs, RipFormat fmt, RipOptions opt) {
    total_.store((int)pairs.size());
    for (const auto& pr : pairs) {
        if (cancel_.load(std::memory_order_relaxed)) break;
        {
            std::lock_guard<std::mutex> lk(file_mtx_);
            current_file_ = fs::path(pr.src).filename().string();
        }
        Result r = convertOne(pr.src, pr.dst, fmt, opt, &cancel_);
        // If cancel fired during this file it was abandoned (partial removed) -
        // do not tally it, mirroring GainScan's abandon-in-flight semantics.
        if (cancel_.load(std::memory_order_relaxed)) break;
        switch (r) {
            case Result::Converted:       converted_.fetch_add(1); break;
            case Result::SkippedExists:
            case Result::SkippedSamePath: skipped_.fetch_add(1);   break;
            case Result::Error:           errors_.fetch_add(1);    break;
        }
        index_.fetch_add(1);
    }
    {
        std::lock_guard<std::mutex> lk(file_mtx_);
        current_file_.clear();
    }
    running_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
}
