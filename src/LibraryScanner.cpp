#include "LibraryScanner.h"

#include "AudioExts.h"     // the canonical extension list, shared with PlaylistManager
#include "PortUtil.h"      // port::fopenUtf8, port::statFile
#include "StringUtils.h"   // utf8_to_wide

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>   // PropertyMap is only forward-declared in tag.h
#include <taglib/audioproperties.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace libidx {
namespace {

// TagLib::FileName is wide on Windows and narrow on POSIX - the same shape every
// other call site in the tree uses (ArtEmbed.cpp, ConvertJob.cpp, GainScan.cpp).
#ifdef _WIN32
#  define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#  define TL_PATH(p) (p).c_str()
#endif

// Build an fs::path WITHOUT a narrow-to-wide conversion on Windows. A wstring
// constructed path cannot throw; a std::string one throws for any path that is
// not valid UTF-8. Ours come from the OS and are fine, but the index file can
// also carry a path written on another platform, so this is the safe route
// everywhere rather than in the places we think we need it.
inline fs::path toPath(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(utf8_to_wide(utf8));
#else
    return fs::path(utf8);
#endif
}

// Windows: .string() converts wide to narrow as UTF-8, measured byte-exact on
// this toolchain. It can still fail on a name containing an unpaired surrogate,
// so callers treat the empty return as "skip this entry".
inline std::string fromPath(const fs::path& p) {
    try { return p.string(); } catch (...) { return {}; }
}

std::string tagStr(const TagLib::String& s) {
    // toCString(true) is UTF-8. TagLib returns an empty String for absent tags.
    if (s.isEmpty()) return {};
    const char* c = s.toCString(true);
    return c ? std::string(c) : std::string();
}

// Fills the tag-derived fields. Returns false when TagLib could not open the
// file at all, or opened it and had nothing to say - the caller still indexes
// the record (an unreadable file the user can see and play beats a silently
// incomplete library) but counts it.
bool readTags(const std::string& path, LibraryTrack& t) {
    TagLib::FileRef ref(TL_PATH(path), true, TagLib::AudioProperties::Fast);
    if (ref.isNull() || !ref.file() || !ref.file()->isValid()) return false;

    bool any = false;
    if (const TagLib::Tag* tag = ref.tag()) {
        t.artist = tagStr(tag->artist());
        t.album  = tagStr(tag->album());
        t.title  = tagStr(tag->title());
        t.genre  = tagStr(tag->genre());
        t.track_no = static_cast<int32_t>(tag->track());
        t.year     = static_cast<int32_t>(tag->year());
        any = !t.artist.empty() || !t.album.empty() || !t.title.empty() ||
              !t.genre.empty() || t.track_no != 0 || t.year != 0;
    }
    // Album-artist and disc number are not on the generic Tag interface; they
    // live in the format-specific property map, which every backend implements.
    if (TagLib::File* f = ref.file()) {
        const TagLib::PropertyMap props = f->properties();
        auto first = [&props](const char* key) -> std::string {
            auto it = props.find(key);
            if (it == props.end() || it->second.isEmpty()) return {};
            return tagStr(it->second.front());
        };
        std::string aa = first("ALBUMARTIST");
        if (aa.empty()) aa = first("ALBUM ARTIST");
        if (aa.empty()) aa = first("ALBUMARTISTSORT");
        t.album_artist = aa;
        const std::string disc = first("DISCNUMBER");
        if (!disc.empty()) {
            // "1" or "1/2" - take the leading integer, ignore anything else.
            int32_t v = 0;
            for (char c : disc) {
                if (c < '0' || c > '9') break;
                if (v > 100000) break;              // absurd, stop rather than wrap
                v = v * 10 + (c - '0');
            }
            t.disc_no = v;
        }
        if (!t.album_artist.empty() || t.disc_no != 0) any = true;
    }
    if (const TagLib::AudioProperties* ap = ref.audioProperties()) {
        t.duration_sec = static_cast<int32_t>(ap->lengthInSeconds());
        if (t.duration_sec != 0) any = true;
    }
    return any;
}

// Third copy of the config-directory resolution, matching the precedent set by
// Config.cpp's themePath(), whose own comment says it mirrors configPath()
// rather than refactoring, "to keep this a surgical add". The alternative here
// was worse: calling into Config.cpp would drag Config's secret-store
// dependency into every test that links this unit, which is the exact breakage
// the secret-at-rest slice had to unpick. Unifying all three is a future tidy.
std::string configDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::string(buf) + "\\RE-MOCT" : std::string(".");
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/RE-MOCT" : std::string(".");
#endif
}

} // namespace

std::string libraryIndexPath() {
#ifdef _WIN32
    return configDir() + "\\library.idx";
#else
    return configDir() + "/library.idx";
#endif
}

bool loadIndexFile(const std::string& path, LibraryIndex& out, std::size_t* skipped_out) {
    out = LibraryIndex{};
    if (skipped_out) *skipped_out = 0;

    FILE* f = port::fopenUtf8(path, "rb");
    if (!f) return false;
    std::string text;
    char buf[64 * 1024];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        text.append(buf, n);
    }
    std::fclose(f);

    ParseResult r = parseIndex(text);
    if (!r.ok) return false;                       // corrupt header: treat as no index
    if (skipped_out) *skipped_out = r.skipped_records;
    out = std::move(r.index);
    return true;
}

bool saveIndexFileAtomic(const std::string& path, const LibraryIndex& idx) {
    const std::string tmp = path + ".tmp";
    const std::string text = serialiseIndex(idx);

    FILE* f = port::fopenUtf8(tmp, "wb");
    if (!f) return false;
    const bool wrote = text.empty() ||
                       std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool flushed = (std::fflush(f) == 0);
    std::fclose(f);
    if (!wrote || !flushed) {
        std::error_code rm;
        fs::remove(toPath(tmp), rm);
        return false;
    }

    // Rename over the existing index. fs::rename replaces an existing file on
    // POSIX; on Windows libstdc++ routes it through MoveFileExW with
    // MOVEFILE_REPLACE_EXISTING. Both toolchains are asserted in the gate rather
    // than assumed, because a rename that refused to replace would silently
    // leave a stale index and every scan would look like it had done nothing.
    std::error_code ec;
    fs::rename(toPath(tmp), toPath(path), ec);
    if (ec) {
        std::error_code rm;
        fs::remove(toPath(tmp), rm);
        return false;
    }
    return true;
}

ScanOutcome scanCollection(const std::vector<std::string>& roots,
                           const LibraryIndex& previous,
                           ScanProgress& progress) {
    ScanOutcome out;
    out.index.roots = roots;

    // Previous records by path, so revalidation is a hash lookup rather than a
    // scan per file. A root change invalidates everything by construction: the
    // paths simply will not match.
    std::unordered_map<std::string, const LibraryTrack*> prev;
    prev.reserve(previous.tracks.size() * 2);
    for (const auto& t : previous.tracks) prev.emplace(t.path, &t);

    std::size_t walked_roots = 0;

    for (const std::string& root : roots) {
        std::error_code ec;
        fs::recursive_directory_iterator it;
        try {
            it = fs::recursive_directory_iterator(
                toPath(root), fs::directory_options::skip_permission_denied, ec);
        } catch (...) {
            ec = std::make_error_code(std::errc::invalid_argument);
        }
        if (ec) {
            // SKIPPED, NOT EMPTY. An unplugged drive must not read as "every track
            // on it was deleted", which is what would happen if we simply walked
            // nothing: deletion here is implicit, so a root that contributes no
            // records loses all of them. Carry its previous records forward verbatim.
            out.skipped_roots.push_back(root);
            for (const LibraryTrack& t : previous.tracks)
                if (detail::isPathUnder(t.path, root)) out.index.tracks.push_back(t);
            continue;
        }
        ++walked_roots;

        const fs::recursive_directory_iterator end;
        for (; it != end; ) {
            if (progress.cancel.load(std::memory_order_acquire)) {
                // CANCELLATION BEATS EVERYTHING, and returns before any of the
                // bookkeeping below. A partial multi-root walk must not commit a
                // half-deleted index any more than a partial single-root one could.
                out.completed = false;      // PARTIAL - caller must not commit
                return out;
            }

            std::string path;
            bool is_file = false;
            try {
                std::error_code fe;
                is_file = it->is_regular_file(fe);
                if (!fe && is_file) path = fromPath(it->path());
            } catch (...) {
                ++out.counts.errors;
            }

            // Advance first, so any failure below cannot turn into an infinite loop.
            std::error_code ie;
            it.increment(ie);
            if (ie) break;

            if (!is_file || path.empty()) continue;
            if (!audioext::isSupportedAudio(path)) continue;

            ++out.counts.seen;
            progress.files_seen.fetch_add(1, std::memory_order_relaxed);

            int64_t  mtime = 0;
            uint64_t size  = 0;
            if (!port::statFile(path, mtime, size)) { ++out.counts.errors; continue; }

            auto pit = prev.find(path);
            if (pit != prev.end() && pit->second->mtime == mtime && pit->second->size == size) {
                out.index.tracks.push_back(*pit->second);   // unchanged: no tag read
                ++out.counts.unchanged;
                continue;
            }

            LibraryTrack t;
            t.path  = path;
            t.mtime = mtime;
            t.size  = size;
            if (!readTags(path, t)) ++out.counts.tagless;   // indexed anyway
            progress.files_read.fetch_add(1, std::memory_order_relaxed);
            if (pit != prev.end()) ++out.counts.updated; else ++out.counts.added;
            out.index.tracks.push_back(std::move(t));
        }
    }

    // NOT ONE root could be read. With a single configured root this is exactly the
    // shipped behaviour - nothing walked, nothing committed, and slice 6's
    // "Cannot read the music folder" path fires as it always has. Multi-root ADDS a
    // case rather than changing this one.
    if (walked_roots == 0 && !roots.empty()) {
        out.completed = false;
        return out;
    }

    // Anything in the previous index the walk never reached is gone from disk.
    // Sound ONLY because the walk completed - see the invariant in the header.
    //
    // COUNTED AGAINST WALKED ROOTS ONLY. Records carried forward from a skipped root
    // were not "kept" by a walk and must not be counted as removals either, or the
    // number reported is the size of the offline drive - the same class of lie the
    // album-append count would have told if it had counted instead of measuring.
    std::size_t prev_in_walked = previous.tracks.size();
    std::size_t carried        = 0;
    for (const std::string& skipped : out.skipped_roots)
        for (const LibraryTrack& t : previous.tracks)
            if (detail::isPathUnder(t.path, skipped)) ++carried;
    prev_in_walked = prev_in_walked > carried ? prev_in_walked - carried : 0;

    const std::size_t kept = out.counts.unchanged + out.counts.updated;
    out.counts.removed = prev_in_walked > kept
                       ? static_cast<uint32_t>(prev_in_walked - kept) : 0u;
    out.completed = true;
    // Slice 8: the compilation set is derived from the records, so a freshly scanned
    // index has to build it too - a scan is the other of the two ways an index comes
    // into existence, the first being parseIndex. Only on the COMPLETED path: a
    // cancelled walk returns a partial index the caller must not commit, and computing
    // a compilation verdict over half an album would be wrong as well as wasted.
    rebuildCompilations(out.index);
    return out;
}

// ── LibraryScanner ──────────────────────────────────────────────────────────

LibraryScanner::~LibraryScanner() {
    cancel();
    join();
}

void LibraryScanner::join() {
    if (worker_.joinable()) worker_.join();
}

void LibraryScanner::cancel() {
    progress_.cancel.store(true, std::memory_order_release);
}

void LibraryScanner::start(const std::vector<std::string>& roots,
                           const std::string& index_path) {
    if (active_.load(std::memory_order_acquire)) return;
    join();                       // reap a finished previous run

    progress_.files_seen.store(0, std::memory_order_relaxed);
    progress_.files_read.store(0, std::memory_order_relaxed);
    progress_.cancel.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(mtx_); outcome_ = ScanOutcome{}; saved_ = false; }
    done_.store(false, std::memory_order_release);
    active_.store(true, std::memory_order_release);

    worker_ = std::thread([this, roots, index_path]() {
        LibraryIndex previous;
        (void)loadIndexFile(index_path, previous);      // absent/corrupt -> full scan

        ScanOutcome res = scanCollection(roots, previous, progress_);

        // THE INVARIANT: a cancelled scan never commits. Its index is partial,
        // and committing it would delete every record the walk had not reached.
        bool saved = false;
        if (res.completed) saved = saveIndexFileAtomic(index_path, res.index);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            outcome_ = std::move(res);
            saved_   = saved;
        }
        active_.store(false, std::memory_order_release);
        done_.store(true, std::memory_order_release);
    });
}

ScanOutcome LibraryScanner::take() {
    std::lock_guard<std::mutex> lk(mtx_);
    return std::move(outcome_);
}

} // namespace libidx
