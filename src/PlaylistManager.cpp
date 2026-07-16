#include "PlaylistManager.h"
#include "StringUtils.h"

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>

#include <fstream>
#include <map>

#include <filesystem>
#include <system_error>
#include <algorithm>
#include <random>
#include <numeric>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace fs = std::filesystem;

// Non-throwing existence check: the throwing fs::exists overload can raise
// filesystem_error on a malformed path from a crafted playlist, which would
// propagate out of the loaders below. The error_code overload returns false
// on any error instead.
static bool path_exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

static const std::vector<std::string> AUDIO_EXTS = {
    ".mp3", ".flac", ".ogg", ".opus", ".wav", ".aiff", ".aif",
    ".m4a", ".m4b", ".aac", ".wma", ".mp4", ".wv"
};

bool PlaylistManager::isSupportedAudio(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(AUDIO_EXTS.begin(), AUDIO_EXTS.end(), ext) != AUDIO_EXTS.end();
}

bool PlaylistManager::isAudiobook(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".m4b";
}

void PlaylistManager::populateMetadata(PlaylistEntry& entry) {
    entry.display_title = fs::path(entry.path).stem().string();
#ifdef _WIN32
    TagLib::FileRef ref(utf8_to_wide(entry.path).c_str(), true,
                        TagLib::AudioProperties::Fast);
#else
    TagLib::FileRef ref(entry.path.c_str(), true, TagLib::AudioProperties::Fast);
#endif
    if (ref.isNull()) return;
    if (auto* tag = ref.tag(); tag) {
        std::string title  = sanitizeForDisplay(tag->title().to8Bit(true));
        std::string artist = sanitizeForDisplay(tag->artist().to8Bit(true));
        if (!artist.empty() && !title.empty())
            entry.display_title = artist + " - " + title;
        else if (!title.empty())
            entry.display_title = title;
    }
    if (auto* ap = ref.audioProperties(); ap)
        entry.duration_sec = ap->lengthInSeconds();
}

void PlaylistManager::rebuildShuffleOrder() {
    shuffle_order_.resize(entries_.size());
    std::iota(shuffle_order_.begin(), shuffle_order_.end(), 0);
    if (shuffle_ && !shuffle_order_.empty()) {
        std::mt19937 rng(std::random_device{}());
        std::shuffle(shuffle_order_.begin(), shuffle_order_.end(), rng);
        // Put current track at the front so it stays playing
        auto it = std::find(shuffle_order_.begin(), shuffle_order_.end(), current_);
        if (it != shuffle_order_.end())
            std::iter_swap(shuffle_order_.begin(), it);
        shuffle_pos_ = 0;
    } else {
        // Not shuffling — reset pos to match current_ in linear order
        shuffle_pos_ = current_;
    }
}

std::size_t PlaylistManager::addStream(const std::string& url, const std::string& title) {
    // Internet-radio entry: store the URL verbatim with an explicit label.
    // No populateMetadata() — there is no local file to read tags from.
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].path == url) {                 // dedup by URL
            if (!title.empty()) entries_[i].display_title = title;  // refresh label on re-add (rename)
            return i;
        }
    PlaylistEntry entry;
    entry.path          = url;
    entry.display_title = title;
    entry.duration_sec  = 0;                        // live stream — continuous
    entries_.push_back(std::move(entry));
    rebuildShuffleOrder();
    return entries_.size() - 1;
}

std::string PlaylistManager::streamLabel(const std::string& url) {
    std::string rest = url;
    auto s = rest.find("://");
    if (s != std::string::npos) rest = rest.substr(s + 3);
    std::string host = rest.substr(0, rest.find('/'));
    std::string seg  = rest.substr(rest.rfind('/') + 1);
    return "RADIO: " + (seg.empty() ? host : seg);
}

std::size_t PlaylistManager::addTrack(const std::string& path) {
    // HTTP(S) URLs are radio streams — store with a derived label, never file I/O.
    // (This is also how session restore re-creates the RADIO: label, since only the
    //  URL is persisted.)
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
        return addStream(path, streamLabel(path));
    // Reject volatile CD track paths — they can't be stored persistently
    if (isCDTrackPath(path)) return std::string::npos;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].path == path) return i;
    PlaylistEntry entry;
    entry.path = path;
    populateMetadata(entry);
    entries_.push_back(std::move(entry));
    rebuildShuffleOrder();
    return entries_.size() - 1;
}

std::size_t PlaylistManager::addCDTrack(const std::string& fake_path,
                                        const std::string& title,
                                        int duration_sec) {
    PlaylistEntry e;
    e.path          = fake_path;
    e.display_title = title;
    e.duration_sec  = duration_sec;
    entries_.push_back(e);
    rebuildShuffleOrder();
    return entries_.size() - 1;
}

void PlaylistManager::addDirectory(const std::string& dir_path) {
    try {
        std::vector<std::string> found;
        // skip_permission_denied so we don't crash on protected subdirs
        fs::directory_options opts = fs::directory_options::skip_permission_denied;
        for (const auto& de : fs::recursive_directory_iterator(dir_path, opts))
            if (de.is_regular_file() && isSupportedAudio(de.path().string()))
                found.push_back(de.path().string());
        std::sort(found.begin(), found.end());
        for (const auto& f : found) addTrack(f);
    } catch (...) {}
}

void PlaylistManager::removeAt(std::size_t index) {
    if (index >= entries_.size()) return;
    entries_.erase(entries_.begin() + (std::ptrdiff_t)index);
    if (current_ > index) --current_;              // deleted above playing -> shift its index down
    if (current_ >= entries_.size() && !entries_.empty())
        current_ = entries_.size() - 1;            // clamp if playing was last (or was deleted)
    rebuildShuffleOrder();
}

void PlaylistManager::clear() {
    entries_.clear();
    current_ = 0;
    shuffle_order_.clear();
}

void PlaylistManager::moveUp(std::size_t index) {
    if (index == 0 || index >= entries_.size()) return;
    std::swap(entries_[index], entries_[index - 1]);
    // Keep current_ tracking the same track
    if (current_ == index)          current_ = index - 1;
    else if (current_ == index - 1) current_ = index;
    rebuildShuffleOrder();
}

void PlaylistManager::moveDown(std::size_t index) {
    if (index + 1 >= entries_.size()) return;
    std::swap(entries_[index], entries_[index + 1]);
    if (current_ == index)          current_ = index + 1;
    else if (current_ == index + 1) current_ = index;
    rebuildShuffleOrder();
}

std::optional<std::string> PlaylistManager::currentPath() const {
    if (entries_.empty()) return std::nullopt;
    return entries_[current_].path;
}

std::optional<std::string> PlaylistManager::next() {
    if (entries_.empty()) return std::nullopt;

    if (repeat_ == RepeatMode::One)
        return entries_[current_].path;

    if (shuffle_) {
        std::size_t next_pos = shuffle_pos_ + 1;
        if (next_pos >= shuffle_order_.size()) {
            if (repeat_ == RepeatMode::All) {
                // Reshuffle and wrap
                rebuildShuffleOrder();
                next_pos = 0;
            } else {
                return std::nullopt;
            }
        }
        shuffle_pos_ = next_pos;
        current_ = shuffle_order_[shuffle_pos_];
        return entries_[current_].path;
    }

    if (repeat_ == RepeatMode::All) {
        current_ = (current_ + 1) % entries_.size();
        return entries_[current_].path;
    }

    if (current_ + 1 < entries_.size()) {
        ++current_;
        return entries_[current_].path;
    }
    return std::nullopt;
}

std::optional<std::string> PlaylistManager::previous() {
    if (entries_.empty()) return std::nullopt;

    if (shuffle_) {
        if (shuffle_pos_ > 0) --shuffle_pos_;
        current_ = shuffle_order_[shuffle_pos_];
        return entries_[current_].path;
    }

    if (current_ > 0) --current_;
    return entries_[current_].path;
}

std::optional<std::string> PlaylistManager::peekNext() const {
    if (entries_.empty()) return std::nullopt;
    if (repeat_ == RepeatMode::One) return entries_[current_].path;
    if (shuffle_) {
        std::size_t next_pos = shuffle_pos_ + 1;
        if (next_pos >= shuffle_order_.size()) {
            if (repeat_ == RepeatMode::All) next_pos = 0;
            else return std::nullopt;
        }
        return entries_[shuffle_order_[next_pos]].path;
    }
    if (repeat_ == RepeatMode::All)
        return entries_[(current_ + 1) % entries_.size()].path;
    if (current_ + 1 < entries_.size())
        return entries_[current_ + 1].path;
    return std::nullopt;
}

bool PlaylistManager::selectAt(std::size_t index) {
    if (index >= entries_.size()) return false;
    current_ = index;
    if (shuffle_) {
        // Find this index in shuffle order
        auto it = std::find(shuffle_order_.begin(), shuffle_order_.end(), index);
        if (it != shuffle_order_.end())
            shuffle_pos_ = (std::size_t)(it - shuffle_order_.begin());
    }
    return true;
}
// ─── Play Queue ───────────────────────────────────────────────────────────────

void PlaylistManager::queueAdd(const PlaylistEntry& e) {
    play_queue_.push_back(e);
}

std::optional<PlaylistEntry> PlaylistManager::queuePop() {
    if (play_queue_.empty()) return std::nullopt;
    PlaylistEntry e = play_queue_.front();
    play_queue_.pop_front();
    return e;
}

void PlaylistManager::queueRemoveAt(int i) {
    if (i < 0 || i >= (int)play_queue_.size()) return;
    play_queue_.erase(play_queue_.begin() + i);
}

// Strip line breaks (and other C0 controls) from a single field before it is
// written to a playlist. A stray CR/LF in a title or path otherwise splits a line
// in the line-oriented M3U/PLS formats (corrupting every entry after it, and
// mis-parsing on reload); in XSPF it smuggles raw whitespace into the markup. Tabs
// fold to a space; other C0 controls are dropped. Legitimate Windows paths can't
// contain these characters, so this is a no-op for real data.
static std::string strip_breaks(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\t') { o += ' '; continue; }
        if (c == '\r' || c == '\n' || c < 0x20) continue;
        o += (char)c;
    }
    return o;
}

bool PlaylistManager::saveM3U(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "#EXTM3U\n";
    for (const auto& e : entries_) {
        // Skip CD audio tracks (hardware paths) and live radio streams — neither
        // belongs in a saved playlist.
        if (isCDTrackPath(e.path) || isStreamPath(e.path)) continue;
        f << "#EXTINF:" << e.duration_sec << "," << strip_breaks(e.display_title) << "\n";
        f << strip_breaks(e.path) << "\n";
    }
    return f.good();
}

int PlaylistManager::loadM3U(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    int added = 0;
    std::string line;
    while (std::getline(f, line)) {
        // Strip CR
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // Radio URL — keep as a stream entry (don't filter by file existence).
        if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0) {
            addTrack(line);   // delegates to addStream with a derived label
            ++added;
            continue;
        }
        // Could be absolute or relative path
        fs::path p(line);
        if (p.is_relative())
            p = fs::path(path).parent_path() / p;
        std::string abs = p.string();
        if (path_exists(abs) && isSupportedAudio(abs)) {
            addTrack(abs);
            ++added;
        } else if (isSupportedAudio(abs)) {
            ++last_load_missing_;   // audio entry whose file is not on disk
        }
    }
    return added;
}

// ─── PLS ─────────────────────────────────────────────────────────────────────

bool PlaylistManager::savePLS(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "[playlist]\n";
    int n = 0;
    for (const auto& e : entries_) {
        if (isCDTrackPath(e.path) || isStreamPath(e.path)) continue;  // no CD/radio in saved playlists
        ++n;
        f << "File" << n << "=" << strip_breaks(e.path) << "\n";
        f << "Title" << n << "=" << strip_breaks(e.display_title) << "\n";
        f << "Length" << n << "=" << e.duration_sec << "\n";
    }
    f << "NumberOfEntries=" << n << "\n";
    f << "Version=2\n";
    return f.good();
}

int PlaylistManager::loadPLS(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    // Read all key=value pairs
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '[' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    int added = 0;
    int count = 0;
    try { count = std::stoi(kv.count("NumberOfEntries") ? kv["NumberOfEntries"] : "0"); }
    catch (...) {}
    for (int i = 1; i <= count; ++i) {
        std::string file_key  = "File"  + std::to_string(i);
        std::string title_key = "Title" + std::to_string(i);
        if (!kv.count(file_key)) continue;
        std::string p = kv[file_key];
        fs::path fp(p);
        if (fp.is_relative()) fp = fs::path(path).parent_path() / fp;
        std::string abs = fp.string();
        if (path_exists(abs) && isSupportedAudio(abs)) {
            addTrack(abs);
            ++added;
        } else if (isSupportedAudio(abs)) {
            ++last_load_missing_;   // audio entry whose file is not on disk
        }
    }
    return added;
}

// ─── XSPF ────────────────────────────────────────────────────────────────────

// Minimal XML escape for XSPF
static std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

// Minimal XML unescape
static std::string xml_unescape(const std::string& s) {
    std::string out = s;
    auto replace_all = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    // &amp; must be unescaped LAST: unescaping it first would turn an escaped
    // entity such as "&amp;lt;" into "&lt;" and then into "<" — a double-unescape.
    replace_all("&lt;",   "<");
    replace_all("&gt;",   ">");
    replace_all("&quot;", "\"");
    replace_all("&apos;", "'");
    replace_all("&amp;",  "&");
    return out;
}

bool PlaylistManager::saveXSPF(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    f << "<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">\n";
    f << "  <trackList>\n";
    for (const auto& e : entries_) {
        if (isCDTrackPath(e.path) || isStreamPath(e.path)) continue;  // no CD/radio in saved playlists
        // Convert path to file URI
        std::string uri = e.path;
        // Replace backslashes and prepend file:///
        for (char& c : uri) if (c == '\\') c = '/';
        uri = "file:///" + uri;
        f << "    <track>\n";
        f << "      <location>" << xml_escape(strip_breaks(uri)) << "</location>\n";
        if (!e.display_title.empty())
            f << "      <title>" << xml_escape(strip_breaks(e.display_title)) << "</title>\n";
        if (e.duration_sec > 0)
            f << "      <duration>" << (e.duration_sec * 1000) << "</duration>\n";
        f << "    </track>\n";
    }
    f << "  </trackList>\n";
    f << "</playlist>\n";
    return f.good();
}

int PlaylistManager::loadXSPF(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    int added = 0;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Simple tag extraction — no full XML parser needed for our use case
    auto extract_tags = [&](const std::string& tag, const std::string& src,
                            std::vector<std::string>& out) {
        std::string open  = "<" + tag + ">";
        std::string close = "</" + tag + ">";
        size_t pos = 0;
        while ((pos = src.find(open, pos)) != std::string::npos) {
            pos += open.size();
            size_t end = src.find(close, pos);
            if (end == std::string::npos) break;
            out.push_back(src.substr(pos, end - pos));
            pos = end + close.size();
        }
    };

    // Extract <track> blocks
    std::vector<std::string> tracks;
    extract_tags("track", content, tracks);

    for (const auto& track_block : tracks) {
        std::vector<std::string> locations;
        extract_tags("location", track_block, locations);
        if (locations.empty()) continue;

        std::string uri = xml_unescape(locations[0]);
        // Strip file:/// prefix
        if (uri.substr(0, 8) == "file:///") uri = uri.substr(8);
        // Convert forward slashes back to backslashes on Windows
#ifdef _WIN32
        for (char& c : uri) if (c == '/') c = '\\';
#endif
        fs::path fp(uri);
        if (fp.is_relative()) fp = fs::path(path).parent_path() / fp;
        std::string abs = fp.string();
        if (path_exists(abs) && isSupportedAudio(abs)) {
            addTrack(abs);
            ++added;
        } else if (isSupportedAudio(abs)) {
            ++last_load_missing_;   // audio entry whose file is not on disk
        }
    }
    return added;
}

// ─── Unified save/load ────────────────────────────────────────────────────────

static std::string playlist_ext(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

bool PlaylistManager::savePlaylist(const std::string& path) const {
    std::string ext = playlist_ext(path);
    if (ext == ".pls")               return savePLS(path);
    if (ext == ".xspf")              return saveXSPF(path);
    return saveM3U(path);  // .m3u, .m3u8, or default
}

int PlaylistManager::loadPlaylist(const std::string& path) {
    last_load_missing_ = 0;
    std::string ext = playlist_ext(path);
    if (ext == ".pls")               return loadPLS(path);
    if (ext == ".xspf")              return loadXSPF(path);
    return loadM3U(path);
}

void PlaylistManager::sortBy(SortMode mode) {
    if (entries_.empty()) return;
    sort_mode_ = mode;
    // Remember which track is current so we can restore it after sort
    std::string current_path = entries_[current_].path;

    switch (mode) {
        case SortMode::None:
            break; // no-op — original add order is gone, just leave as-is
        case SortMode::Title:
            std::stable_sort(entries_.begin(), entries_.end(),
                [](const PlaylistEntry& a, const PlaylistEntry& b) {
                    return a.display_title < b.display_title;
                });
            break;
        case SortMode::Artist: {
            // display_title is "Artist - Title" for tagged files
            auto artist_of = [](const PlaylistEntry& e) {
                auto dash = e.display_title.find(" - ");
                return (dash != std::string::npos)
                    ? e.display_title.substr(0, dash)
                    : e.display_title;
            };
            std::stable_sort(entries_.begin(), entries_.end(),
                [&](const PlaylistEntry& a, const PlaylistEntry& b) {
                    return artist_of(a) < artist_of(b);
                });
            break;
        }
        case SortMode::Duration:
            std::stable_sort(entries_.begin(), entries_.end(),
                [](const PlaylistEntry& a, const PlaylistEntry& b) {
                    return a.duration_sec < b.duration_sec;
                });
            break;
        case SortMode::Filename:
            std::stable_sort(entries_.begin(), entries_.end(),
                [](const PlaylistEntry& a, const PlaylistEntry& b) {
                    return std::filesystem::path(a.path).filename().string()
                         < std::filesystem::path(b.path).filename().string();
                });
            break;
    }

    // Restore current_ to same track
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].path == current_path) { current_ = i; break; }
    }
    rebuildShuffleOrder();
}

// ── Async background loader ───────────────────────────────────────────────────

void PlaylistManager::addDirectoryAsync(const std::string& dir_path) {
    // Collect all audio file paths (fast — no TagLib yet)
    std::vector<std::string> found;
    try {
        for (const auto& de : fs::recursive_directory_iterator(
                dir_path, fs::directory_options::skip_permission_denied)) {
            if (de.is_regular_file() && isSupportedAudio(de.path().string()))
                found.push_back(de.path().string());
        }
        std::sort(found.begin(), found.end());
    } catch (...) {}

    if (found.empty()) return;

    // Push paths onto the work queue — skip any already in playlist
    {
        std::lock_guard<std::mutex> lk(work_mutex_);
        for (auto& p : found) {
            // Check against existing entries (UI thread owns entries_ but
            // we're on UI thread here so this is safe)
            bool dup = false;
            for (const auto& e : entries_)
                if (e.path == p) { dup = true; break; }
            if (!dup) {
                work_queue_.push(std::move(p));
                pending_count_.fetch_add(1);
            }
        }
    }

    // Start worker thread if not already running
    if (!loader_running_.load()) {
        if (loader_thread_.joinable()) loader_thread_.join();
        loader_running_.store(true);
        loader_thread_ = std::thread(&PlaylistManager::loaderWorker, this);
    }
}

void PlaylistManager::loaderWorker() {
    while (true) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(work_mutex_);
            if (work_queue_.empty()) { loader_running_.store(false); break; }
            path = std::move(work_queue_.front());
            work_queue_.pop();
        }
        // Check for duplicate before doing expensive TagLib work
        {
            std::lock_guard<std::mutex> lk(done_mutex_);
            // Can't safely check entries_ from worker thread (UI thread owns it)
            // but we can check done_queue_ for duplicates within this batch
        }
        PlaylistEntry entry;
        entry.path = path;
        entry.display_title = fs::path(path).stem().string();
        populateMetadata(entry);
        pending_count_.fetch_sub(1);
        {
            std::lock_guard<std::mutex> lk(done_mutex_);
            done_queue_.push(std::move(entry));
        }
    }
}

bool PlaylistManager::drainPending() {
    std::queue<PlaylistEntry> local;
    {
        std::lock_guard<std::mutex> lk(done_mutex_);
        std::swap(local, done_queue_);
    }
    if (local.empty()) return false;
    while (!local.empty()) {
        PlaylistEntry e = std::move(local.front());
        local.pop();
        // Dedup check — skip if path already in playlist
        bool dup = false;
        for (const auto& ex : entries_)
            if (ex.path == e.path) { dup = true; break; }
        if (!dup) entries_.push_back(std::move(e));
    }
    rebuildShuffleOrder();
    return true;
}
