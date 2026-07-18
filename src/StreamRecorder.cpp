// StreamRecorder.cpp — the stream-capture engine (stream-record slice R1).
// Threading contract and lane isolation: see the header. Everything in this
// file except push() runs on the recorder's own worker thread (or the UI
// thread for start/stop/onTitle, which never touch audio or files).
#include "StreamRecorder.h"
#include "IEncoder.h"
#include "FrameSync.h"     // copy mode: frame parse (abi-cluster slice B)
#include "MuxWriter.h"     // copy mode: MP3 append / ADTS->M4A mux writers
#include "Mp3Encoder.h"
#include "OpusRipEncoder.h"
#include "M4aEncoder.h"
#include "R128Gain.h"      // r128FromDb — the one home for the R128<->RG dialect
#include "StringUtils.h"   // sanitizePathComponent / localtimeSafe / utf8_to_wide
#include "PortUtil.h"      // sleepMs
#include "Version.h"

#include <ebur128.h>

#include <taglib/tstring.h>
#include <taglib/id3v2tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/attachedpictureframe.h>   // rec-cover-art: MP3 APIC
#include <taglib/flacpicture.h>            // rec-cover-art: Opus picture block
#include <taglib/mpegfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/opusfile.h>   // TagLib's own (collision-safe: taglib/ prefixed)
#include <taglib/mp4file.h>    // copy mode + re-encode M4A: the .m4a tag pass
#include <taglib/mp4tag.h>
#include <taglib/mp4item.h>
#include <taglib/mp4coverart.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

// Same per-platform separator rule as CDRipper's kSep (file-scoped there);
// recordings paths display alongside rip paths, so they keep the same shape.
#ifdef _WIN32
static const std::string kSep = "\\";
#else
static const std::string kSep = "/";
#endif

// ─── title parsing ────────────────────────────────────────────────────────────
StreamRecorder::ParsedTitle StreamRecorder::parseNowPlaying(const std::string& raw) {
    // The scrobbler's exact idiom (UIManager::updateScrobbler radio branch):
    // FIRST " - " splits, so "A - B - C" keeps "B - C" as the title.
    ParsedTitle p;
    auto dash = raw.find(" - ");
    if (dash == std::string::npos) return p;
    p.artist = raw.substr(0, dash);
    p.title  = raw.substr(dash + 3);
    auto trim = [](std::string& x) {
        while (!x.empty() && (x.front()==' '||x.front()=='\t')) x.erase(x.begin());
        while (!x.empty() && (x.back()==' '||x.back()=='\t')) x.pop_back();
    };
    trim(p.artist); trim(p.title);
    // Article de-inversion ("Shins, The" -> "The Shins"; "Tyler, The Creator"
    // untouched) — applied HERE, not in the tag pass, so filename and tag get
    // the same artist (stream-record R2; shared home in StringUtils.h).
    p.artist = deinvertArtist(p.artist);
    p.ok = !p.artist.empty() && !p.title.empty();
    return p;
}

// ─── construction / control (UI thread) ──────────────────────────────────────
StreamRecorder::StreamRecorder(uint32_t ring_frames) {
    ring_frames_ = std::bit_ceil(std::max(ring_frames, 1024u));
    ring_.assign((size_t)ring_frames_ * CHANNELS, 0.0f);
}

StreamRecorder::~StreamRecorder() { stop(); }

bool StreamRecorder::start(const RecOptions& opt, const std::string& station,
                           const std::string& out_dir) {
    auto fail = [this](const char* why) {
        std::lock_guard<std::mutex> lk(cuts_mtx_);
        error_ = why;
        return false;
    };
    if (running_.load()) return fail("already recording");
    if (worker_.joinable()) worker_.join();     // reap a self-stopped worker
    if (opt.copy_mode) {
        // Copy mode: the injected pull is the whole input path; the format
        // list and quality knobs are inert (the container follows the codec).
        if (!opt.pull) return fail("copy mode needs an encoded byte source");
    } else {
        if (opt.formats.empty()) return fail("no output format selected");
        for (RipFormat f : opt.formats)
            if (f != RipFormat::Mp3 && f != RipFormat::Opus && f != RipFormat::M4a)
                return fail("stream capture offers lossy output only (MP3/Opus/M4A)");
    }

    station_ = sanitizePathComponent(station.empty() ? "Stream" : station);
    station_dir_ = out_dir + kSep + station_;
    std::error_code ec;
    fs::create_directories(station_dir_, ec);
    if (ec) return fail("cannot create the recordings directory");

    opt_ = opt;
    if (opt_.split_offset_ms < 0) opt_.split_offset_ms = 0;   // lead RESERVED (plan doc)
    hold_armed_ = false;
    hold_frames_left_ = 0;
    cut_is_ad_ = false;
    suppressed_ = false;
    ads_skipped_.store(0);
    wpos_.store(0); rpos_.store(0);
    dropped_.store(0); total_frames_.store(0); bytes_written_.store(0);
    cut_index_.store(0);
    {
        std::lock_guard<std::mutex> lk(cuts_mtx_);
        cuts_.clear(); error_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(meta_mtx_);
        pending_raw_.clear();
        pending_arts_.clear();
    }
    split_pending_.store(false);
    cut_artist_.clear(); cut_title_.clear(); cut_raw_.clear();
    cut_frames_ = 0; cut_open_ = false; first_cut_ = true;
    copy_out_.reset(); copy_path_.clear(); copy_codec_ = 0;
    copy_sr_.store(0);

    stop_req_.store(false);
    running_.store(true);
    // The tap exclusivity hazard, closed structurally: ONE start arms exactly
    // one input. Copy mode runs the pull pump and never arms the PCM tap
    // (capture() stays a no-op); PCM mode never constructs a copy writer.
    if (opt_.copy_mode) {
        worker_ = std::thread(&StreamRecorder::copyLoop, this);
    } else {
        worker_ = std::thread(&StreamRecorder::workerLoop, this);
        capturing_.store(true, std::memory_order_release);
    }
    return true;
}

void StreamRecorder::stop() {
    capturing_.store(false, std::memory_order_release);
    stop_req_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    // running_ was cleared by the worker on exit; make idle-stop idempotent.
    running_.store(false);
}

void StreamRecorder::onTitle(const std::string& raw) {
    if (!running_.load(std::memory_order_relaxed)) return;
    if (!opt_.split_on_meta) return;   // opt_ is read-only while running
    {
        std::lock_guard<std::mutex> lk(meta_mtx_);
        if (raw == pending_raw_) return;   // dedup: repeats must not cut twice
        pending_raw_ = raw;
    }
    split_pending_.store(true, std::memory_order_release);
}

// JPEG/PNG magic -> MIME; anything else rejected (a partial or non-image body
// must never produce a broken picture block). Layered over CoverArt's own
// raster guard on the fetch side.
static const char* artMimeFromMagic(const std::vector<uint8_t>& b) {
    if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return "image/jpeg";
    if (b.size() > 8 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47)
        return "image/png";
    return nullptr;
}

void StreamRecorder::onArt(const std::string& raw_now_playing,
                           std::vector<uint8_t> bytes) {
    if (!running_.load(std::memory_order_relaxed)) return;
    const char* mime = artMimeFromMagic(bytes);   // static literal (or null)
    if (!mime) return;                 // reject early: no broken picture blocks
    std::lock_guard<std::mutex> lk(meta_mtx_);
    // Newest wins on a key collision (a song's art re-fetching replaces it).
    for (auto it = pending_arts_.begin(); it != pending_arts_.end(); ++it)
        if (it->key == raw_now_playing) { pending_arts_.erase(it); break; }
    pending_arts_.push_back({ raw_now_playing, std::move(bytes), mime });
    if (pending_arts_.size() > kPendingArtMax) pending_arts_.pop_front();  // evict oldest
}

bool StreamRecorder::takePendingArt(const std::string& key,
                                    std::vector<uint8_t>& out, const char*& mime) {
    std::lock_guard<std::mutex> lk(meta_mtx_);
    for (auto it = pending_arts_.begin(); it != pending_arts_.end(); ++it)
        if (it->key == key) {
            out  = std::move(it->bytes);
            mime = it->mime;               // static literal; outlives the entry
            pending_arts_.erase(it);       // evict on use
            return true;
        }
    return false;
}

std::string StreamRecorder::currentTitle() const {
    std::lock_guard<std::mutex> lk(meta_mtx_);
    return pending_raw_;
}

std::string StreamRecorder::lastError() const {
    std::lock_guard<std::mutex> lk(cuts_mtx_);
    return error_;
}

std::vector<StreamRecorder::CutInfo> StreamRecorder::cuts() const {
    std::lock_guard<std::mutex> lk(cuts_mtx_);
    return cuts_;
}

// ─── audio thread ─────────────────────────────────────────────────────────────
// One bounded copy + two atomic ops; a full ring drops the WHOLE incoming
// block (never a partial — a torn block would corrupt the stream) and counts
// it. Never blocks, never allocates, never syscalls.
void StreamRecorder::push(const float* src, uint32_t frames) {
    uint64_t w = wpos_.load(std::memory_order_relaxed);
    uint64_t r = rpos_.load(std::memory_order_acquire);
    uint32_t free_frames = ring_frames_ - (uint32_t)(w - r);
    if (frames > free_frames) {
        dropped_.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    uint32_t off   = (uint32_t)(w & (ring_frames_ - 1));
    uint32_t first = std::min(frames, ring_frames_ - off);
    std::memcpy(&ring_[(size_t)off * CHANNELS], src,
                (size_t)first * CHANNELS * sizeof(float));
    if (frames > first)
        std::memcpy(ring_.data(), src + (size_t)first * CHANNELS,
                    (size_t)(frames - first) * CHANNELS * sizeof(float));
    wpos_.store(w + frames, std::memory_order_release);
}

// ─── worker thread ────────────────────────────────────────────────────────────
namespace {
std::string timestampNow() {
    std::time_t t = std::time(nullptr);
    std::tm tmbuf{};
    localtimeSafe(t, tmbuf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H%M%S", &tmbuf);
    return buf;
}

// ReplayGain string formatting — tagFile's exact formatters.
std::string rgStr(double gain) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(2) << gain << " dB";
    return ss.str();
}
std::string rgPeakStr(double peak) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(6) << peak;
    return ss.str();
}

// The recorder-side tag pass. NOT CDRipper::tagFile (that signature is
// MB/AR-shaped); these are tagFile's per-format shapes with the fields a
// stream capture honestly has: title/artist from StreamTitle, provenance
// comment, TRACK gain only (no album exists — absent because false). MP3
// carries the plain ReplayGain TXXX dialect; Opus carries R128 (RFC 7845)
// via the shared R128Gain.h conversion — same homes as the rip path.
void tagCut(const std::string& path, RipFormat fmt,
            const std::string& artist, const std::string& title,
            const std::string& raw, const std::string& station,
            bool rg_valid, double rg_gain, double rg_peak,
            const std::vector<uint8_t>& art, const char* art_mime) {
    try {
#ifdef _WIN32
        auto wp = utf8_to_wide(path);        // TagLib::FileName is wide on Windows
#else
        const std::string& wp = path;        // and UTF-8 bytes elsewhere
#endif
        // Best available title: the parsed one, else the raw StreamTitle —
        // metadata is never silently dropped just because it didn't parse.
        const std::string& title_str = !title.empty() ? title : raw;
        const std::string  comment   = "Recorded from " + station + " by RE-MOCT";

        if (fmt == RipFormat::Mp3) {
            TagLib::MPEG::File f(wp.c_str(), false);
            auto* tag = f.ID3v2Tag(true); if (!tag) return;
            if (!title_str.empty())
                tag->setTitle(TagLib::String(title_str, TagLib::String::UTF8));
            if (!artist.empty())
                tag->setArtist(TagLib::String(artist, TagLib::String::UTF8));
            tag->setComment(TagLib::String(comment, TagLib::String::UTF8));
            auto addTxt = [&](const char* id, const std::string& val) {
                auto* f2 = new TagLib::ID3v2::TextIdentificationFrame(id, TagLib::String::UTF8);
                f2->setText(TagLib::String(val, TagLib::String::UTF8));
                tag->addFrame(f2);
            };
            // TXXX = proper description/value split (mp3-rg-write, the
            // tagFile mirror): the old "KEY=value" blob was invisible to
            // every non-RE-MOCT reader. TENC stays generic — correct for a
            // standard text frame.
            auto addUserTxt = [&](const char* desc, const std::string& val) {
                auto* f2 = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
                f2->setDescription(desc);
                f2->setText(TagLib::String(val, TagLib::String::UTF8));
                tag->addFrame(f2);
            };
            addTxt("TENC", "RE-MOCT v" REMOCT_VERSION);
            if (rg_valid) {
                addUserTxt("REPLAYGAIN_TRACK_GAIN", rgStr(rg_gain));
                addUserTxt("REPLAYGAIN_TRACK_PEAK", rgPeakStr(rg_peak));
            }
            // Cover art (rec-cover-art): tagFile's MP3 APIC shape, MIME from
            // the magic check instead of hardcoded jpeg.
            if (!art.empty() && art_mime) {
                auto* ap = new TagLib::ID3v2::AttachedPictureFrame();
                ap->setMimeType(art_mime);
                ap->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
                ap->setDescription("Cover");
                ap->setPicture(TagLib::ByteVector((const char*)art.data(), (unsigned)art.size()));
                tag->addFrame(ap);
            }
            f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
        } else if (fmt == RipFormat::Opus) {
            TagLib::Ogg::Opus::File f(wp.c_str(), false);
            auto* tag = f.tag(); if (!tag) return;
            if (!title_str.empty())
                tag->setTitle(TagLib::String(title_str, TagLib::String::UTF8));
            if (!artist.empty())
                tag->setArtist(TagLib::String(artist, TagLib::String::UTF8));
            tag->setComment(TagLib::String(comment, TagLib::String::UTF8));
            tag->addField("ENCODER",
                TagLib::String("RE-MOCT v" REMOCT_VERSION, TagLib::String::UTF8), true);
            if (rg_valid)
                tag->addField("R128_TRACK_GAIN",
                    TagLib::String(std::to_string(r128FromDb(rg_gain)),
                                   TagLib::String::UTF8), true);
            // Cover art (rec-cover-art): tagFile's Opus picture shape
            // (XiphComment carries FLAC::Picture natively).
            if (!art.empty() && art_mime) {
                auto* pic = new TagLib::FLAC::Picture();
                pic->setMimeType(art_mime);
                pic->setType(TagLib::FLAC::Picture::FrontCover);
                pic->setDescription(TagLib::String("Cover"));
                pic->setData(TagLib::ByteVector((const char*)art.data(), (unsigned)art.size()));
                tag->addPicture(pic);
            }
            f.save();
        } else if (fmt == RipFormat::M4a) {
            // Re-encode M4A cut (aac-m4a-encoder): MP4 atoms, tagFile's is_m4a
            // shape trimmed to a stream capture's honest fields. TRACK gain only
            // (no album on a live cut), as the PLAIN ReplayGain (----) dialect
            // TagLib surfaces through PropertyMap. Distinct from the copy path's
            // tagCutM4a (which carries the broadcast untouched, so no RG).
            TagLib::MP4::File f(wp.c_str(), false);
            auto* tag = f.tag(); if (!tag) return;
            if (!title_str.empty())
                tag->setTitle(TagLib::String(title_str, TagLib::String::UTF8));
            if (!artist.empty())
                tag->setArtist(TagLib::String(artist, TagLib::String::UTF8));
            tag->setComment(TagLib::String(comment, TagLib::String::UTF8));
            tag->setItem("\251too", TagLib::MP4::Item(TagLib::StringList(
                TagLib::String("RE-MOCT v" REMOCT_VERSION, TagLib::String::UTF8))));
            if (rg_valid) {
                tag->setItem("----:com.apple.iTunes:REPLAYGAIN_TRACK_GAIN",
                    TagLib::MP4::Item(TagLib::StringList(
                        TagLib::String(rgStr(rg_gain), TagLib::String::UTF8))));
                tag->setItem("----:com.apple.iTunes:REPLAYGAIN_TRACK_PEAK",
                    TagLib::MP4::Item(TagLib::StringList(
                        TagLib::String(rgPeakStr(rg_peak), TagLib::String::UTF8))));
            }
            if (!art.empty() && art_mime) {
                auto cf = std::strcmp(art_mime, "image/png") == 0
                          ? TagLib::MP4::CoverArt::PNG : TagLib::MP4::CoverArt::JPEG;
                TagLib::MP4::CoverArtList covers;
                covers.append(TagLib::MP4::CoverArt(
                    cf, TagLib::ByteVector((const char*)art.data(), (unsigned)art.size())));
                tag->setItem("covr", TagLib::MP4::Item(covers));
            }
            f.save();
        }
    } catch (...) {
        // Tagging is best-effort exactly as the rip pass: the audio on disk is
        // already complete and valid; a tag failure must not fail the cut.
    }
}

// The copy path's .m4a tag pass (abi-cluster slice B): title/artist/comment +
// covr through TagLib::MP4 — library-supported, the reason the AAC container
// decision landed on M4A (plan §1). No ReplayGain by design: copy cuts carry
// the broadcast untouched; re-encode remains the RG-bearing mode. Scoped
// tightly (the TagLib RW-by-default lesson): the File lives only inside this
// call, after the writer finalized.
void tagCutM4a(const std::string& path,
               const std::string& artist, const std::string& title,
               const std::string& raw, const std::string& station,
               const std::vector<uint8_t>& art, const char* art_mime) {
    try {
#ifdef _WIN32
        auto wp = utf8_to_wide(path);
#else
        const std::string& wp = path;
#endif
        TagLib::MP4::File f(wp.c_str(), false);
        auto* tag = f.tag(); if (!tag) return;
        const std::string& title_str = !title.empty() ? title : raw;
        if (!title_str.empty())
            tag->setTitle(TagLib::String(title_str, TagLib::String::UTF8));
        if (!artist.empty())
            tag->setArtist(TagLib::String(artist, TagLib::String::UTF8));
        tag->setComment(TagLib::String("Recorded from " + station + " by RE-MOCT",
                                       TagLib::String::UTF8));
        tag->setItem("\251too", TagLib::MP4::Item(
            TagLib::StringList(TagLib::String("RE-MOCT v" REMOCT_VERSION,
                                              TagLib::String::UTF8))));
        if (!art.empty() && art_mime) {
            auto fmt = std::strcmp(art_mime, "image/png") == 0
                       ? TagLib::MP4::CoverArt::PNG : TagLib::MP4::CoverArt::JPEG;
            TagLib::MP4::CoverArtList covers;
            covers.append(TagLib::MP4::CoverArt(
                fmt, TagLib::ByteVector((const char*)art.data(), (unsigned)art.size())));
            tag->setItem("covr", TagLib::MP4::Item(covers));
        }
        f.save();
    } catch (...) {
        // Best-effort, same as tagCut: the audio on disk is already valid.
    }
}

std::unique_ptr<IEncoder> makeRecEncoder(RipFormat f, const RecOptions& opt) {
    switch (f) {
        case RipFormat::Mp3:  return std::make_unique<Mp3Encoder>(opt.mp3_vbr_q, opt.mp3_cbr, opt.mp3_cbr_bitrate);
        case RipFormat::Opus: return std::make_unique<OpusRipEncoder>(opt.opus_bitrate, opt.opus_vbr);
        case RipFormat::M4a:  return std::make_unique<M4aEncoder>(opt.aac_vbr, opt.aac_vbr_level, opt.aac_cbr_bitrate);
        default:              return nullptr;   // start() rejected everything else
    }
}
} // namespace

std::string StreamRecorder::cutBasePath() const {
    ParsedTitle p = parseNowPlaying(cut_raw_);
    // Timestamp fallback (plan §5): a bad-metadata cut is still a sanely-named
    // valid file, never an empty-named one.
    std::string name = p.ok ? p.artist + " - " + p.title
                            : station_ + " - " + timestampNow();
    if (first_cut_) name += " (partial)";     // joined mid-song at session start
    return station_dir_ + kSep + sanitizePathComponent(name);
}

bool StreamRecorder::openCut() {
    ParsedTitle p = parseNowPlaying(cut_raw_);
    cut_artist_ = p.artist; cut_title_ = p.title;

    // ad-aware classification: parseable-but-junk = non-song (the LIVE floor
    // is iHeart's STRUCTURAL ad signal surfacing as a string; the vocabulary
    // is the ICY heuristic — one classifier, two signal qualities).
    // Unparseable titles stay songs/main, deliberately conservative: a
    // title-only station's real songs must never misroute or drop.
    cut_is_ad_ = p.ok && !looksLikeRealTrack(p.artist, p.title);

    if (cut_is_ad_ && opt_.ads_discard) {
        // Suppressed cut (Discard): no encoders, no files — frames are counted
        // (the skip is VISIBLE) but never written. The user's explicit opt-in.
        suppressed_ = true;
        outs_.clear();
        ebur_ = nullptr;
        cut_frames_ = 0;
        cut_open_   = true;
        return true;
    }
    suppressed_ = false;

    std::string base;
    if (cut_is_ad_) {
        // Save: route to <Station>/ads/ with a sortable, self-describing name.
        // No (partial) marking — the folder's semantic is "not a song, prune".
        std::string ad_dir = station_dir_ + kSep + "ads";
        std::error_code ec;
        fs::create_directories(ad_dir, ec);
        base = ad_dir + kSep + timestampNow() + " - " +
               sanitizePathComponent(cut_raw_.empty() ? "ad" : cut_raw_);
    } else {
        base = cutBasePath();
    }
    // Collision: the same song returning in rotation must never overwrite an
    // earlier cut — probe across ALL selected formats, suffix " (2)", " (3)"…
    std::string chosen = base;
    for (int n = 2; ; ++n) {
        bool clash = false;
        std::error_code ec;
        for (RipFormat f : opt_.formats)
            if (fs::exists(chosen + ripFormatRow(f)->ext, ec)) { clash = true; break; }
        if (!clash) break;
        chosen = base + " (" + std::to_string(n) + ")";
    }

    outs_.clear();
    for (RipFormat f : opt_.formats) {
        Out o { f, chosen + ripFormatRow(f)->ext, makeRecEncoder(f, opt_) };
        if (!o.enc || !o.enc->open(o.path, 0)) {
            // Unwind anything already opened; leave no orphan files.
            for (auto& prev : outs_) { prev.enc->finalize(false); std::error_code e2; fs::remove(prev.path, e2); }
            outs_.clear();
            return false;
        }
        outs_.push_back(std::move(o));
    }
    ebur_ = ebur128_init(CHANNELS, SAMPLE_RATE,
                         EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    cut_frames_ = 0;
    cut_open_   = true;
    return true;
}

bool StreamRecorder::processBlock(const float* f32, uint32_t frames) {
    if (!cut_open_ && !openCut()) {
        failCut("cannot open output files");
        return false;
    }
    if (suppressed_) {   // Discard: count (the honesty surface), never write
        cut_frames_ += frames;
        total_frames_.fetch_add(frames, std::memory_order_relaxed);
        return true;
    }
    if (ebur_)
        ebur128_add_frames_float(ebur_, f32, (size_t)frames);   // frames, NOT samples

    // f32 -> s16 for the IEncoder contract (the exact inverse of the decode
    // side's x/32768, clamped).
    s16_.resize((size_t)frames * CHANNELS);
    for (size_t i = 0; i < (size_t)frames * CHANNELS; ++i) {
        long v = std::lround((double)f32[i] * 32768.0);
        s16_[i] = (int16_t)std::clamp(v, -32768L, 32767L);
    }
    for (auto& o : outs_)
        if (!o.enc->writeFrames(s16_.data(), frames)) {
            failCut("write failed (disk full?)");
            return false;
        }
    cut_frames_ += frames;
    total_frames_.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

void StreamRecorder::rollCut(bool partial_tail) {
    if (!cut_open_) return;

    // Suppressed (Discard) cut: record the skip visibly, write nothing.
    if (suppressed_) {
        CutInfo info;
        info.artist = cut_artist_; info.title = cut_title_; info.raw = cut_raw_;
        info.frames = cut_frames_;
        info.is_ad = true; info.discarded = true;
        {
            std::lock_guard<std::mutex> lk(cuts_mtx_);
            cuts_.push_back(std::move(info));
        }
        ads_skipped_.fetch_add(1, std::memory_order_relaxed);
        suppressed_ = false;
        cut_open_   = false;
        cut_frames_ = 0;
        first_cut_  = false;
        cut_index_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    CutInfo info;
    info.artist = cut_artist_; info.title = cut_title_; info.raw = cut_raw_;
    info.frames = cut_frames_;
    info.is_ad  = cut_is_ad_;
    info.partial = (first_cut_ || partial_tail) && !cut_is_ad_;

    bool ok = true;
    for (auto& o : outs_)
        if (!o.enc->finalize(true)) ok = false;    // buffering-encoder flush check

    if (!ok) {
        // The fflush-and-check discipline: an incomplete file is a failed cut,
        // removed — never left looking like a full capture.
        for (auto& o : outs_) { std::error_code ec; fs::remove(o.path, ec); }
        info.failed = true;
        {
            std::lock_guard<std::mutex> lk(cuts_mtx_);
            cuts_.push_back(std::move(info));
            error_ = "finalize failed (disk full?)";
        }
        capturing_.store(false, std::memory_order_release);
        stop_req_.store(true, std::memory_order_release);
    } else {
        // Per-cut ReplayGain (track gain only — no album exists). Guarded:
        // integrated loudness needs real audio; silence/too-short yields -inf.
        bool   rg_valid = false;
        double rg_gain = 0.0, rg_peak = 0.0;
        if (ebur_) {
            double loudness = 0;
            if (ebur128_loudness_global(ebur_, &loudness) == EBUR128_SUCCESS &&
                std::isfinite(loudness) && loudness > -70.0) {
                rg_gain = -18.0 - loudness;        // RG reference = -18 LUFS
                double pl = 0, pr = 0;
                ebur128_true_peak(ebur_, 0, &pl);
                ebur128_true_peak(ebur_, 1, &pr);
                rg_peak  = std::max(pl, pr);
                rg_valid = true;
            }
        }
        // Cover art: key-match at tag time — the pending image embeds ONLY if
        // it belongs to THIS cut's raw title (no art rather than wrong art;
        // an edge cut under the metadata slop may miss a cover, never carry
        // a neighbor's). The ring survives newer songs' art landing during a
        // split-trim hold (rec-art-pending-race-fix); take + evict this cut's.
        std::vector<uint8_t> art;
        const char* art_mime = nullptr;
        takePendingArt(cut_raw_, art, art_mime);
        for (auto& o : outs_)
            tagCut(o.path, o.fmt, cut_artist_, cut_title_, cut_raw_, station_,
                   rg_valid, rg_gain, rg_peak, art, art_mime);
        // The session-stop cut is partial (stopped mid-song): rename AFTER
        // tagging so the suffix survives. The first cut's suffix was already
        // in the name at open; don't double-mark.
        for (auto& o : outs_) {
            if (partial_tail && !cut_is_ad_ &&
                o.path.find(" (partial)") == std::string::npos) {
                const std::string ext = ripFormatRow(o.fmt)->ext;
                std::string np = o.path.substr(0, o.path.size() - ext.size())
                               + " (partial)" + ext;
                std::error_code ec;
                fs::rename(o.path, np, ec);
                if (!ec) o.path = np;
            }
            std::error_code ec;
            auto sz = fs::file_size(o.path, ec);
            if (!ec) bytes_written_.fetch_add(sz, std::memory_order_relaxed);
            info.paths.push_back(o.path);
        }
        std::lock_guard<std::mutex> lk(cuts_mtx_);
        cuts_.push_back(std::move(info));
    }

    if (ebur_) ebur128_destroy(&ebur_);
    outs_.clear();
    cut_open_  = false;
    cut_frames_ = 0;
    first_cut_ = false;
    cut_index_.fetch_add(1, std::memory_order_relaxed);
}

void StreamRecorder::failCut(const std::string& why) {
    for (auto& o : outs_) {
        o.enc->finalize(false);
        std::error_code ec;
        fs::remove(o.path, ec);
    }
    if (ebur_) { ebur128_destroy(&ebur_); }
    CutInfo info;
    info.artist = cut_artist_; info.title = cut_title_; info.raw = cut_raw_;
    info.frames = cut_frames_; info.failed = true;
    {
        std::lock_guard<std::mutex> lk(cuts_mtx_);
        cuts_.push_back(std::move(info));
        error_ = why;
    }
    outs_.clear();
    cut_open_ = false;
    cut_frames_ = 0;
    // A failed write is almost always persistent (disk full): stop recording
    // honestly rather than looping new cuts into the same wall.
    capturing_.store(false, std::memory_order_release);
    stop_req_.store(true, std::memory_order_release);
}

void StreamRecorder::workerLoop() {
    constexpr uint32_t CHUNK = 4096;   // ~93 ms per drain block — the split
                                       // granularity, noise vs broadcast slop
    std::vector<float> scratch((size_t)CHUNK * CHANNELS);
    bool healthy = true;

    // Adopt a pending title while the open cut has no audio attributed yet
    // (or none is open): a relabel in place, not a split.
    auto adoptIfCutEmpty = [&]() {
        if (!split_pending_.load(std::memory_order_acquire)) return;
        if (cut_open_ && cut_frames_ > 0) return;   // real split — handled below
        std::lock_guard<std::mutex> lk(meta_mtx_);
        cut_raw_ = pending_raw_;
        split_pending_.store(false, std::memory_order_release);
    };

    // Finalize the current cut and adopt the pending title as the next cut's
    // identity — the one roll path both the immediate (offset<=0) and the
    // held (offset>0, countdown expired) splits share.
    auto rollAndAdopt = [&]() {
        rollCut(false);
        std::lock_guard<std::mutex> lk(meta_mtx_);
        cut_raw_ = pending_raw_;
        split_pending_.store(false, std::memory_order_release);
        hold_armed_ = false;
    };

    while (healthy) {
        adoptIfCutEmpty();

        // split-trim: arm the hold when a split is detected and an offset is
        // configured. The flag STAYS SET while armed (the pending title is
        // adopted at the roll); arriving audio keeps attributing to the OLD
        // cut until exactly offset*44.1 frames have passed — protecting the
        // outro the early metadata would have guillotined. A second title
        // change during the hold overwrites the pending title (its would-be
        // cut is sub-offset-length by definition). stop() outranks the hold:
        // the session tail finalizes immediately at loop exit.
        if (opt_.split_offset_ms > 0 && !hold_armed_ &&
            split_pending_.load(std::memory_order_acquire) &&
            cut_open_ && cut_frames_ > 0) {
            hold_armed_ = true;
            hold_frames_left_ =
                (uint64_t)opt_.split_offset_ms * SAMPLE_RATE / 1000;
        }

        uint64_t r = rpos_.load(std::memory_order_relaxed);
        uint64_t w = wpos_.load(std::memory_order_acquire);
        uint64_t avail = w - r;   // SNAPSHOT: frames queued as of now

        // Drain the snapshot — pre-event audio belongs to the OLD cut; under
        // an armed hold the countdown decides frame-exactly where the old cut
        // ends, splitting a chunk mid-block when the boundary lands inside it.
        while (avail > 0 && healthy) {
            uint32_t n = (uint32_t)std::min<uint64_t>(CHUNK, avail);
            uint32_t off   = (uint32_t)(r & (ring_frames_ - 1));
            uint32_t first = std::min(n, ring_frames_ - off);
            std::memcpy(scratch.data(), &ring_[(size_t)off * CHANNELS],
                        (size_t)first * CHANNELS * sizeof(float));
            if (n > first)
                std::memcpy(scratch.data() + (size_t)first * CHANNELS, ring_.data(),
                            (size_t)(n - first) * CHANNELS * sizeof(float));
            r += n;
            rpos_.store(r, std::memory_order_release);
            if (hold_armed_) {
                uint32_t to_old = (uint32_t)std::min<uint64_t>(n, hold_frames_left_);
                if (to_old > 0) {
                    healthy = processBlock(scratch.data(), to_old);
                    hold_frames_left_ -= to_old;
                }
                if (healthy && hold_frames_left_ == 0) {
                    rollAndAdopt();
                    if (to_old < n)   // the chunk's tail opens the NEW cut
                        healthy = processBlock(scratch.data() + (size_t)to_old * CHANNELS,
                                               n - to_old);
                }
            } else {
                healthy = processBlock(scratch.data(), n);
            }
            avail -= n;
        }

        // Immediate roll (offset<=0 — today's behavior, the regression
        // anchor) — checked on EVERY pass including an idle ring, so a split
        // can never stall waiting for the next audio block. A held split
        // rolls only through its countdown above.
        if (healthy && opt_.split_offset_ms <= 0 &&
            split_pending_.load(std::memory_order_acquire) &&
            cut_open_ && cut_frames_ > 0) {
            rollAndAdopt();
        }

        if (rpos_.load(std::memory_order_relaxed) ==
            wpos_.load(std::memory_order_acquire)) {
            if (stop_req_.load(std::memory_order_acquire)) break;
            port::sleepMs(20);
        }
    }

    if (healthy && cut_open_) rollCut(true);   // session tail: partial cut
    running_.store(false, std::memory_order_release);
}

// ─── copy mode (abi-cluster slice B) ──────────────────────────────────────────
// The worker IS the pump: it polls the injected pull callback, frame-parses
// (FrameSync.h), and feeds a MuxWriter per cut. Same cut lifecycle, split
// triggers, ad-aware routing, and honesty surfaces as the PCM worker — the
// unit is the encoded frame instead of the PCM chunk, so splits snap to
// whole frames (~23-26 ms, under the broadcast metadata slop).

bool StreamRecorder::openCopyCut(int32_t codec) {
    ParsedTitle p = parseNowPlaying(cut_raw_);
    cut_artist_ = p.artist; cut_title_ = p.title;
    cut_is_ad_ = p.ok && !looksLikeRealTrack(p.artist, p.title);
    copy_codec_ = codec;

    if (cut_is_ad_ && opt_.ads_discard) {
        // Suppressed (Discard): frames are pulled and counted, never written.
        suppressed_ = true;
        copy_out_.reset(); copy_path_.clear();
        cut_frames_ = 0;
        cut_open_   = true;
        return true;
    }
    suppressed_ = false;

    const char* ext = (codec == 2) ? ".m4a" : ".mp3";
    std::string base;
    if (cut_is_ad_) {
        std::string ad_dir = station_dir_ + kSep + "ads";
        std::error_code ec;
        fs::create_directories(ad_dir, ec);
        base = ad_dir + kSep + timestampNow() + " - " +
               sanitizePathComponent(cut_raw_.empty() ? "ad" : cut_raw_);
    } else {
        base = cutBasePath();
    }
    std::string chosen = base;
    for (int n = 2; ; ++n) {
        std::error_code ec;
        if (!fs::exists(chosen + ext, ec)) break;
        chosen = base + " (" + std::to_string(n) + ")";
    }

    copy_out_ = (codec == 2)
        ? std::unique_ptr<MuxWriter>(std::make_unique<M4aMuxWriter>())
        : std::unique_ptr<MuxWriter>(std::make_unique<Mp3AppendWriter>());
    copy_path_ = chosen + ext;
    if (!copy_out_->open(copy_path_)) {
        copy_out_.reset(); copy_path_.clear();
        return false;
    }
    cut_frames_ = 0;
    cut_open_   = true;
    return true;
}

void StreamRecorder::rollCopyCut(bool partial_tail) {
    if (!cut_open_) return;

    if (suppressed_) {
        CutInfo info;
        info.artist = cut_artist_; info.title = cut_title_; info.raw = cut_raw_;
        info.frames = cut_frames_;
        info.is_ad = true; info.discarded = true;
        {
            std::lock_guard<std::mutex> lk(cuts_mtx_);
            cuts_.push_back(std::move(info));
        }
        ads_skipped_.fetch_add(1, std::memory_order_relaxed);
        suppressed_ = false;
        cut_open_   = false;
        cut_frames_ = 0;
        first_cut_  = false;
        cut_index_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    CutInfo info;
    info.artist = cut_artist_; info.title = cut_title_; info.raw = cut_raw_;
    info.frames = cut_frames_;
    info.is_ad  = cut_is_ad_;
    info.partial = (first_cut_ || partial_tail) && !cut_is_ad_;

    bool ok = copy_out_ && copy_out_->finalize(true);
    if (!ok) {
        // Incomplete container = failed cut, removed — never left looking
        // like a full capture (the fflush-and-check discipline).
        std::error_code ec;
        if (!copy_path_.empty()) fs::remove(copy_path_, ec);
        info.failed = true;
        {
            std::lock_guard<std::mutex> lk(cuts_mtx_);
            cuts_.push_back(std::move(info));
            error_ = "finalize failed (disk full?)";
        }
        stop_req_.store(true, std::memory_order_release);
    } else {
        // Cover art: same take-and-evict contract as the PCM roll (the ring
        // survives newer songs' art during a hold - rec-art-pending-race-fix).
        std::vector<uint8_t> art;
        const char* art_mime = nullptr;
        takePendingArt(cut_raw_, art, art_mime);
        if (copy_codec_ == 2)
            tagCutM4a(copy_path_, cut_artist_, cut_title_, cut_raw_, station_,
                      art, art_mime);
        else
            // The .mp3 copy rides the existing ID3v2 pass; rg_valid=false by
            // design (no RG on copy — re-encode is the RG-bearing mode).
            tagCut(copy_path_, RipFormat::Mp3, cut_artist_, cut_title_, cut_raw_,
                   station_, false, 0.0, 0.0, art, art_mime);
        if (partial_tail && !cut_is_ad_ &&
            copy_path_.find(" (partial)") == std::string::npos) {
            const std::string ext = (copy_codec_ == 2) ? ".m4a" : ".mp3";
            std::string np = copy_path_.substr(0, copy_path_.size() - ext.size())
                           + " (partial)" + ext;
            std::error_code ec;
            fs::rename(copy_path_, np, ec);
            if (!ec) copy_path_ = np;
        }
        std::error_code ec;
        auto sz = fs::file_size(copy_path_, ec);
        if (!ec) bytes_written_.fetch_add(sz, std::memory_order_relaxed);
        info.paths.push_back(copy_path_);
        std::lock_guard<std::mutex> lk(cuts_mtx_);
        cuts_.push_back(std::move(info));
    }

    copy_out_.reset();
    copy_path_.clear();
    cut_open_   = false;
    cut_frames_ = 0;
    first_cut_  = false;
    cut_index_.fetch_add(1, std::memory_order_relaxed);
}

void StreamRecorder::failCopyCut(const std::string& why) {
    if (copy_out_) copy_out_->finalize(false);
    std::error_code ec;
    if (!copy_path_.empty()) fs::remove(copy_path_, ec);
    CutInfo info;
    info.artist = cut_artist_; info.title = cut_title_; info.raw = cut_raw_;
    info.frames = cut_frames_; info.failed = true;
    {
        std::lock_guard<std::mutex> lk(cuts_mtx_);
        cuts_.push_back(std::move(info));
        error_ = why;
    }
    copy_out_.reset();
    copy_path_.clear();
    cut_open_ = false;
    cut_frames_ = 0;
    // Same stance as failCut: a failed write is almost always persistent —
    // stop honestly rather than looping new cuts into the same wall.
    stop_req_.store(true, std::memory_order_release);
}

void StreamRecorder::copyLoop() {
    std::vector<uint8_t> buf;               // parse accumulator (whole frames out)
    std::vector<uint8_t> chunk(64 * 1024);  // per-pull scratch
    uint64_t skip_left = 0;                 // in-stream ID3v2 remainder to drop
    int32_t  codec = 0;                     // latched from the pull's codec_out
    bool     healthy = true;

    auto adoptIfCutEmpty = [&]() {
        if (!split_pending_.load(std::memory_order_acquire)) return;
        if (cut_open_ && cut_frames_ > 0) return;
        std::lock_guard<std::mutex> lk(meta_mtx_);
        cut_raw_ = pending_raw_;
        split_pending_.store(false, std::memory_order_release);
    };
    auto rollAndAdopt = [&]() {
        rollCopyCut(false);
        std::lock_guard<std::mutex> lk(meta_mtx_);
        cut_raw_ = pending_raw_;
        split_pending_.store(false, std::memory_order_release);
        hold_armed_ = false;
    };

    while (healthy) {
        adoptIfCutEmpty();

        // split-trim: arm exactly as the PCM worker, in BROADCAST samples
        // (copy_sr_ is nonzero whenever cut_frames_ > 0 — a frame set it).
        if (opt_.split_offset_ms > 0 && !hold_armed_ &&
            split_pending_.load(std::memory_order_acquire) &&
            cut_open_ && cut_frames_ > 0) {
            hold_armed_ = true;
            hold_frames_left_ = (uint64_t)opt_.split_offset_ms *
                                copy_sr_.load(std::memory_order_relaxed) / 1000;
        }

        int32_t c = 0, disc = 0;
        uint32_t got = opt_.pull(chunk.data(), (uint32_t)chunk.size(), &c, &disc);
        if (c != 0) codec = c;
        if (disc) {
            // The gap sits at the APPEND point: buffered bytes are pre-gap
            // and any partial frame tail will never complete — drop it; the
            // next frame header resyncs (the plan's discont contract).
            buf.clear();
            skip_left = 0;
        }
        if (got > 0) buf.insert(buf.end(), chunk.data(), chunk.data() + got);

        size_t pos = 0;
        while (healthy && codec != 0) {
            if (skip_left > 0) {                       // inside an in-stream tag
                uint64_t take = buf.size() - pos;
                if (take > skip_left) take = skip_left;
                pos += (size_t)take; skip_left -= take;
                if (skip_left > 0) break;              // tag continues past buffer
                continue;
            }
            size_t avail = buf.size() - pos;
            if (avail < 10) break;                     // header room (ID3 needs 10)
            if (uint32_t s = framesync::id3v2Size(buf.data() + pos, avail)) {
                skip_left = s;                         // transport metadata: never
                continue;                              // reaches the writer
            }
            framesync::FrameInfo fi;
            bool hdr = (codec == 2)
                ? framesync::adtsParse(buf.data() + pos, avail, fi)
                : framesync::mp3Parse(buf.data() + pos, avail, fi);
            if (!hdr) { ++pos; continue; }             // resync scan, byte-wise
            if (avail < fi.frame_len) break;           // incomplete: await bytes

            if (!cut_open_ && !openCopyCut(codec)) {
                failCopyCut("cannot open output file");
                healthy = false;
                break;
            }
            if (!suppressed_ && copy_out_ &&
                !copy_out_->writeFrame(buf.data() + pos, fi.frame_len, fi)) {
                failCopyCut("write failed (disk full?)");
                healthy = false;
                break;
            }
            pos += fi.frame_len;
            cut_frames_ += fi.samples;
            total_frames_.fetch_add(fi.samples, std::memory_order_relaxed);
            if (copy_sr_.load(std::memory_order_relaxed) != fi.sample_rate)
                copy_sr_.store(fi.sample_rate, std::memory_order_relaxed);

            if (hold_armed_) {
                hold_frames_left_ = (hold_frames_left_ > fi.samples)
                                    ? hold_frames_left_ - fi.samples : 0;
                if (hold_frames_left_ == 0) rollAndAdopt();   // snap: whole frames
            }
        }
        if (pos > 0) buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)pos);

        // Immediate roll (offset<=0) — checked on EVERY pass including an
        // idle pump, so a split can never stall waiting for bytes.
        if (healthy && opt_.split_offset_ms <= 0 &&
            split_pending_.load(std::memory_order_acquire) &&
            cut_open_ && cut_frames_ > 0) {
            rollAndAdopt();
        }

        // Stop drains best-effort: keep pumping while bytes still arrive so
        // the tail cut carries everything the tee held, then exit on the
        // first dry pull (mirrors the PCM loop's drained-ring exit).
        if (got == 0) {
            if (stop_req_.load(std::memory_order_acquire)) break;
            port::sleepMs(50);                 // the pump cadence (plan §2)
        }
    }

    if (healthy && cut_open_) rollCopyCut(true);   // session tail: partial cut
    running_.store(false, std::memory_order_release);
}
