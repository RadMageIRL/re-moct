// library_scanner_test - proves LibraryScanner.cpp: the walk, the tag read, the
// incremental revalidation, the atomic write, and the one invariant that makes
// deletion-on-unseen safe.
//
// THE INVARIANT UNDER TEST: a cancelled scan never commits. The refresh treats
// "not seen during the walk" as "deleted", which is only true of a walk that ran
// to completion. A partial result that committed would mass-delete records for
// files the walk simply had not reached yet, so the cancellation cases here
// assert BOTH that the outcome is marked incomplete AND that the index file on
// disk is byte-for-byte what it was before.
//
// Uses a real temporary directory tree, because a scanner that is not exercised
// against a real filesystem is not exercised at all. The optional real-collection
// pass is gated on REMOCT_LIBRARY_SCAN_ROOT so this stays green on a CI runner
// with no music on it.

#include "LibraryScanner.h"
#include "AudioExts.h"
#include "PlaylistManager.h"
#include "StringUtils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace libidx;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

// Same wide-on-Windows construction the scanner uses: never a narrow-to-wide
// conversion, so a non-ASCII temp path cannot throw.
static fs::path P(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(utf8_to_wide(utf8));
#else
    return fs::path(utf8);
#endif
}

static std::string g_root;

static void writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

static std::string readFile(const std::string& path) {
    std::ifstream f(P(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string join(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

// A tiny but structurally valid WAV, so TagLib can open it and report a duration
// rather than failing outright. 1 frame of silence, 44.1k stereo 16-bit.
static std::string tinyWav() {
    auto u32 = [](uint32_t v) {
        std::string s(4, '\0');
        s[0]=(char)(v&0xFF); s[1]=(char)((v>>8)&0xFF); s[2]=(char)((v>>16)&0xFF); s[3]=(char)((v>>24)&0xFF);
        return s;
    };
    auto u16 = [](uint16_t v) {
        std::string s(2, '\0'); s[0]=(char)(v&0xFF); s[1]=(char)((v>>8)&0xFF); return s;
    };
    const std::string data(4, '\0');           // one stereo 16-bit frame
    std::string fmt = u16(1) + u16(2) + u32(44100) + u32(176400) + u16(4) + u16(16);
    std::string body = "WAVE" "fmt " + u32((uint32_t)fmt.size()) + fmt
                     + "data" + u32((uint32_t)data.size()) + data;
    return "RIFF" + u32((uint32_t)body.size()) + body;
}

// ── The extension list is shared, and both readers agree ────────────────────
// This is the playlist-add check: PlaylistManager is user-facing and this slice
// otherwise is not, so the lift has to be provably behaviour-preserving.
static void test_extension_lift() {
    const char* audio[] = {"a.mp3","a.flac","a.ogg","a.opus","a.wav","a.aiff","a.aif",
                           "a.m4a","a.m4b","a.aac","a.wma","a.mp4","a.wv"};
    for (const char* p : audio) {
        CHECK(audioext::isSupportedAudio(p), "audioext accepts %s", p);
        CHECK(PlaylistManager::isSupportedAudio(p), "playlist accepts %s", p);
    }
    const char* notAudio[] = {"a.txt","a.jpg","a.m3u","a","a.","a.mp3.txt",".mp3",".hidden",
                              "dir.mp3/file","a.mp3x","",".","..","a.MP3.bak"};
    for (const char* p : notAudio) {
        CHECK(!audioext::isSupportedAudio(p), "audioext rejects [%s]", p);
        CHECK(!PlaylistManager::isSupportedAudio(p), "playlist rejects [%s]", p);
    }
    // Case-insensitive, and the two agree on every shape above.
    const char* mixed[] = {"A.MP3","b.FlAc","c.Wv","D:\\Music\\x.OPUS","/m/y.M4B"};
    for (const char* p : mixed) {
        CHECK(audioext::isSupportedAudio(p), "audioext accepts mixed case %s", p);
        CHECK(PlaylistManager::isSupportedAudio(p) == audioext::isSupportedAudio(p),
              "readers agree on %s", p);
    }
}

// ── First scan over a known tree ────────────────────────────────────────────
static void test_first_scan() {
    const std::string dir = join(g_root, "coll");
    fs::create_directories(P(join(dir, "sub")));
    writeFile(join(dir, "one.wav"),           tinyWav());
    writeFile(join(dir, "two.mp3"),           "not really an mp3");   // tagless
    writeFile(join(dir, "notes.txt"),         "ignored");             // wrong extension
    writeFile(join(dir, "empty.flac"),        "");                    // zero-byte
    writeFile(join(dir, "caf\xC3\xA9.wav"),   tinyWav());             // non-ASCII name
    writeFile(join(join(dir, "sub"), "three.wav"), tinyWav());        // recursion

    ScanProgress pr;
    ScanOutcome out = scanCollection({dir}, LibraryIndex{}, pr);

    CHECK(out.completed, "completed");
    CHECK(out.index.roots == std::vector<std::string>{dir}, "roots recorded");
    CHECK(out.counts.seen == 5, "seen=%u (5 audio, txt ignored)", out.counts.seen);
    CHECK(out.index.tracks.size() == 5, "indexed=%zu", out.index.tracks.size());
    CHECK(out.counts.added == 5, "added=%u", out.counts.added);
    CHECK(out.counts.unchanged == 0, "unchanged=%u", out.counts.unchanged);
    CHECK(out.counts.removed == 0, "removed=%u", out.counts.removed);
    CHECK(out.counts.tagless >= 2, "tagless=%u (bad mp3 + empty flac)", out.counts.tagless);
    CHECK(pr.files_read.load() == 5, "files_read=%u", pr.files_read.load());

    // The non-ASCII file is present, with its bytes intact.
    bool found_nonascii = false;
    for (const auto& t : out.index.tracks)
        if (t.path.find("\xC3\xA9") != std::string::npos) found_nonascii = true;
    CHECK(found_nonascii, "non-ASCII path indexed and byte-exact");

    // A tagless file is still indexed, with a real path/mtime/size.
    for (const auto& t : out.index.tracks) {
        CHECK(!t.path.empty(), "path non-empty");
        CHECK(t.mtime != 0, "mtime set for %s", t.path.c_str());
    }
}

// ── Rescan with nothing changed re-reads NOTHING ────────────────────────────
static void test_rescan_reads_nothing() {
    const std::string dir = join(g_root, "coll");
    ScanProgress p1; ScanOutcome first = scanCollection({dir}, LibraryIndex{}, p1);
    ScanProgress p2; ScanOutcome again = scanCollection({dir}, first.index, p2);

    CHECK(again.completed, "completed");
    CHECK(again.counts.unchanged == first.counts.seen,
          "unchanged=%u of %u", again.counts.unchanged, first.counts.seen);
    CHECK(again.counts.added == 0 && again.counts.updated == 0,
          "added=%u updated=%u", again.counts.added, again.counts.updated);
    CHECK(p2.files_read.load() == 0, "ZERO tag reads on an unchanged rescan (got %u)",
          p2.files_read.load());
    CHECK(again.index.tracks.size() == first.index.tracks.size(), "same track count");
}

// ── Add, modify, delete produce exactly the three deltas ────────────────────
static void test_deltas() {
    const std::string dir = join(g_root, "coll");
    ScanProgress p0; ScanOutcome base = scanCollection({dir}, LibraryIndex{}, p0);
    const std::size_t n0 = base.index.tracks.size();

    writeFile(join(dir, "added.wav"), tinyWav());                 // add
    writeFile(join(dir, "two.mp3"), "not really an mp3 - longer"); // modify (size changes)
    fs::remove(P(join(dir, "one.wav")));                           // delete

    ScanProgress p1; ScanOutcome out = scanCollection({dir}, base.index, p1);
    CHECK(out.completed, "completed");
    CHECK(out.counts.added == 1,   "added=%u",   out.counts.added);
    CHECK(out.counts.updated == 1, "updated=%u", out.counts.updated);
    CHECK(out.counts.removed == 1, "removed=%u", out.counts.removed);
    CHECK(out.index.tracks.size() == n0, "count steady at %zu (got %zu)", n0, out.index.tracks.size());
    CHECK(p1.files_read.load() == 2, "only the added+modified were read (got %u)",
          p1.files_read.load());

    bool gone = true;
    for (const auto& t : out.index.tracks)
        if (t.path.find("one.wav") != std::string::npos) gone = false;
    CHECK(gone, "deleted file dropped from the index");
}

// ── THE INVARIANT: a cancelled scan is incomplete and never commits ─────────
static void test_cancel_never_commits() {
    const std::string dir  = join(g_root, "many");
    fs::create_directories(P(dir));
    for (int i = 0; i < 400; ++i)
        writeFile(join(dir, "t" + std::to_string(i) + ".wav"), tinyWav());

    const std::string idxp = join(g_root, "cancel.idx");

    // Establish a good index first, and remember its bytes.
    {
        ScanProgress p; ScanOutcome full = scanCollection({dir}, LibraryIndex{}, p);
        CHECK(full.completed && full.index.tracks.size() == 400, "seeded 400");
        CHECK(saveIndexFileAtomic(idxp, full.index), "seed index written");
    }
    const std::string before = readFile(idxp);
    CHECK(!before.empty(), "seed index non-empty");

    // Cancel already set: the walk must report incomplete immediately.
    {
        ScanProgress p; p.cancel.store(true);
        ScanOutcome out = scanCollection({dir}, LibraryIndex{}, p);
        CHECK(!out.completed, "pre-cancelled scan is incomplete");
    }

    // Cancel mid-flight through the threaded scanner, which is the path that
    // would actually commit. Delete half the files first, so that a partial
    // result committing would be VISIBLE as mass deletion.
    for (int i = 0; i < 200; ++i) fs::remove(P(join(dir, "t" + std::to_string(i) + ".wav")));

    LibraryScanner sc;
    sc.start({dir}, idxp);
    sc.cancel();
    for (int i = 0; i < 500 && !sc.done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(sc.done(), "worker finished after cancel");
    CHECK(!sc.active(), "worker not active");

    ScanOutcome res = sc.take();
    CHECK(!res.completed, "cancelled outcome marked incomplete");
    CHECK(readFile(idxp) == before, "INDEX FILE UNTOUCHED by a cancelled scan");
}

// ── The threaded scanner commits a completed scan ───────────────────────────
static void test_worker_commits_when_complete() {
    const std::string dir  = join(g_root, "coll");
    const std::string idxp = join(g_root, "worker.idx");

    LibraryScanner sc;
    sc.start({dir}, idxp);
    for (int i = 0; i < 1000 && !sc.done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(sc.done(), "worker finished");
    ScanOutcome res = sc.take();
    CHECK(res.completed, "completed");

    LibraryIndex loaded;
    CHECK(loadIndexFile(idxp, loaded), "index file readable");
    CHECK(loaded.tracks.size() == res.index.tracks.size(), "committed all records");
    CHECK(loaded.roots == std::vector<std::string>{dir}, "roots persisted");

    // Destructor on a running scan must not hang - start and let it fall out of
    // scope immediately.
    { LibraryScanner s2; s2.start({join(g_root, "many")}, join(g_root, "d.idx")); }
    CHECK(true, "destructor cancelled and joined without hanging");
}

// ── Index I/O: missing, corrupt, partial, and atomic replace ────────────────
static void test_index_io() {
    const std::string p = join(g_root, "io.idx");
    LibraryIndex out;

    CHECK(!loadIndexFile(join(g_root, "nope.idx"), out), "missing file -> false");

    writeFile(p, "not an index at all\n");
    CHECK(!loadIndexFile(p, out), "corrupt header -> false");
    CHECK(out.tracks.empty(), "corrupt yields empty index");

    // Partial corruption is still usable, and says how much it dropped.
    LibraryIndex good; good.roots = { "/m" };
    LibraryTrack t; t.path = "/m/a.flac"; t.mtime = 5; t.size = 7;
    good.tracks.push_back(t);
    writeFile(p, serialiseIndex(good) + "garbage-line\n");
    std::size_t skipped = 0;
    CHECK(loadIndexFile(p, out, &skipped), "partial corruption still loads");
    CHECK(out.tracks.size() == 1 && skipped == 1, "n=%zu skipped=%zu", out.tracks.size(), skipped);

    // ATOMIC REPLACE over an existing file - asserted on both toolchains rather
    // than assumed, since a rename that refused to replace would leave a stale
    // index and make every scan look like a no-op.
    LibraryIndex v2; v2.roots = { "/m2" };
    CHECK(saveIndexFileAtomic(p, v2), "atomic write over an existing file");
    CHECK(loadIndexFile(p, out) && out.roots == std::vector<std::string>{"/m2"}, "replaced content");
    std::error_code ec;
    CHECK(!fs::exists(P(p + ".tmp"), ec), "no .tmp left behind");

    // A stray .tmp from a previous crash must not block the next write.
    writeFile(p + ".tmp", "leftover");
    LibraryIndex v3; v3.roots = { "/m3" };
    CHECK(saveIndexFileAtomic(p, v3), "stray .tmp does not block the write");
    CHECK(loadIndexFile(p, out) && out.roots == std::vector<std::string>{"/m3"}, "replaced again");
}

// ── An unreadable root does not fake an empty library ───────────────────────
static void test_bad_root() {
    ScanProgress p;
    ScanOutcome out = scanCollection({join(g_root, "does-not-exist")}, LibraryIndex{}, p);
    CHECK(!out.completed, "missing root -> incomplete, so nothing commits");
    CHECK(out.index.tracks.empty(), "no tracks invented");

    // Slice 6: an unreadable root must leave a PRE-EXISTING index exactly as it was.
    // Same obligation as a cancelled scan, and for the same reason - the walk did not
    // finish, so deletion-on-unseen would read as mass deletion - but reached by a
    // different route, which is the whole point: ScanOutcome reports only "did not
    // complete" for both, so both have to be safe.
    const std::string dir  = join(g_root, "coll");
    const std::string idxp = join(g_root, "badroot.idx");
    {
        ScanProgress q; ScanOutcome good = scanCollection({dir}, LibraryIndex{}, q);
        CHECK(good.completed, "seeded a good index");
        CHECK(saveIndexFileAtomic(idxp, good.index), "seed written");
    }
    const std::string before = readFile(idxp);
    CHECK(!before.empty(), "seed non-empty");

    // Through the THREADED scanner, which is the path that would actually commit.
    LibraryScanner sc;
    sc.start({join(g_root, "does-not-exist-either")}, idxp);
    for (int i = 0; i < 500 && !sc.done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(sc.done(), "worker finished on an unreadable root");
    ScanOutcome res = sc.take();
    CHECK(!res.completed, "unreadable root -> incomplete");
    CHECK(readFile(idxp) == before, "INDEX FILE UNTOUCHED by an unreadable root");
}

// ── Optional: the real collection, measured ─────────────────────────────────
static void test_real_collection() {
    const char* env = std::getenv("REMOCT_LIBRARY_SCAN_ROOT");
    if (!env || !*env) { std::printf("  [real scan] skipped (set REMOCT_LIBRARY_SCAN_ROOT)\n"); return; }
    const std::string root = env;
    std::error_code ec;
    if (!fs::exists(P(root), ec)) { std::printf("  [real scan] skipped (root missing)\n"); return; }

    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    ScanProgress p1;
    auto t0 = clk::now();
    ScanOutcome first = scanCollection({root}, LibraryIndex{}, p1);
    auto t1 = clk::now();

    ScanProgress p2;
    auto t2 = clk::now();
    ScanOutcome again = scanCollection({root}, first.index, p2);
    auto t3 = clk::now();

    CHECK(first.completed && again.completed, "real scans completed");
    CHECK(again.counts.unchanged == first.counts.seen, "rescan revalidated everything");
    CHECK(p2.files_read.load() == 0, "rescan re-read nothing (got %u)", p2.files_read.load());

    std::printf("  [real scan] root=%s\n", root.c_str());
    std::printf("  [real scan] files=%u tagless=%u errors=%u  first=%.0f ms  rescan=%.0f ms  index=%zu bytes\n",
                first.counts.seen, first.counts.tagless, first.counts.errors,
                ms(t0, t1), ms(t2, t3), serialiseIndex(first.index).size());
}

// ═══ Slice 11: several roots, and the one that is not there ═════════════════

static void test_multi_root_scan() {
    const std::string a = join(g_root, "mr_a");
    const std::string b = join(g_root, "mr_b");
    fs::create_directories(P(a));
    fs::create_directories(P(b));
    writeFile(join(a, "a1.wav"), tinyWav());
    writeFile(join(a, "a2.wav"), tinyWav());
    writeFile(join(b, "b1.wav"), tinyWav());

    ScanProgress pr;
    ScanOutcome out = scanCollection({a, b}, LibraryIndex{}, pr);
    CHECK(out.completed, "two readable roots complete");
    CHECK(out.index.tracks.size() == 3, "records from BOTH roots, got %zu",
          out.index.tracks.size());
    CHECK(out.index.roots.size() == 2, "both roots recorded on the index");
    CHECK(out.skipped_roots.empty(), "nothing was skipped");

    std::size_t from_a = 0, from_b = 0;
    for (const auto& t : out.index.tracks) {
        if (libidx::detail::isPathUnder(t.path, a)) ++from_a;
        if (libidx::detail::isPathUnder(t.path, b)) ++from_b;
    }
    CHECK(from_a == 2 && from_b == 1, "2 from A and 1 from B, got %zu and %zu", from_a, from_b);
}

// THE ONE THAT MATTERS MOST. A root that is offline must be SKIPPED, not read as
// empty: deletion here is implicit, so a root contributing no records would lose all
// of them. On the reference machine that is 619 tracks vanishing because a drive was
// unplugged.
static void test_offline_root_records_survive() {
    const std::string a    = join(g_root, "mr2_a");
    const std::string gone = join(g_root, "mr2_gone");
    fs::create_directories(P(a));
    fs::create_directories(P(gone));
    writeFile(join(a, "a1.wav"),    tinyWav());
    writeFile(join(a, "doomed.wav"), tinyWav());
    writeFile(join(gone, "g1.wav"), tinyWav());
    writeFile(join(gone, "g2.wav"), tinyWav());

    ScanProgress p0;
    ScanOutcome base = scanCollection({a, gone}, LibraryIndex{}, p0);
    CHECK(base.completed && base.index.tracks.size() == 4, "baseline has all four");

    // The drive goes away, AND a file under the readable root is deleted, so this
    // proves both halves at once: the offline root is preserved and the live root's
    // deletions still apply.
    std::error_code ec;
    fs::remove_all(P(gone), ec);
    fs::remove(P(join(a, "doomed.wav")), ec);

    ScanProgress p1;
    ScanOutcome out = scanCollection({a, gone}, base.index, p1);
    CHECK(out.completed, "a scan with one root offline still COMPLETES and commits");
    CHECK(out.skipped_roots.size() == 1, "the offline root is reported, got %zu",
          out.skipped_roots.size());

    std::size_t survivors = 0, live = 0;
    for (const auto& t : out.index.tracks) {
        if (libidx::detail::isPathUnder(t.path, gone)) ++survivors;
        if (libidx::detail::isPathUnder(t.path, a))    ++live;
    }
    CHECK(survivors == 2, "the offline root's records SURVIVE, got %zu", survivors);
    CHECK(live == 1, "and the live root's deletion still applies, got %zu", live);
    // The removed count must describe the WALKED roots only, or it reports the size of
    // the offline drive as deletions.
    CHECK(out.counts.removed == 1, "removed counts the walked root only, got %u",
          out.counts.removed);
}

static void test_no_root_readable() {
    const std::string x = join(g_root, "mr3_missing_a");
    const std::string y = join(g_root, "mr3_missing_b");
    ScanProgress pr;
    ScanOutcome out = scanCollection({x, y}, LibraryIndex{}, pr);
    // Not one root could be walked, so nothing is committed - which with a SINGLE
    // root is byte-for-byte the shipped behaviour slice 6 relies on.
    CHECK(!out.completed, "no readable root means the scan did not complete");

    ScanProgress p2;
    ScanOutcome one = scanCollection({x}, LibraryIndex{}, p2);
    CHECK(!one.completed, "and a single unreadable root behaves exactly as before");
}

static void test_cancel_with_two_roots() {
    const std::string a = join(g_root, "mr4_a");
    const std::string b = join(g_root, "mr4_b");
    fs::create_directories(P(a));
    fs::create_directories(P(b));
    for (int i = 0; i < 40; ++i) writeFile(join(a, "a" + std::to_string(i) + ".wav"), tinyWav());
    for (int i = 0; i < 40; ++i) writeFile(join(b, "b" + std::to_string(i) + ".wav"), tinyWav());

    ScanProgress pr;
    pr.cancel.store(true);                      // cancelled before the first file
    ScanOutcome out = scanCollection({a, b}, LibraryIndex{}, pr);
    CHECK(!out.completed, "a cancelled multi-root walk never completes");
    // Cancellation returns BEFORE any carry-forward bookkeeping, so a partial
    // multi-root scan cannot commit a half-deleted index any more than a single-root
    // one could.
    CHECK(out.skipped_roots.empty(), "and reports no skipped roots, it simply stopped");
}

int main() {
    std::error_code ec;
    g_root = (fs::temp_directory_path(ec) / "remoct_libscan_test").string();
    fs::remove_all(P(g_root), ec);
    fs::create_directories(P(g_root), ec);
    if (ec) { std::printf("cannot create temp dir\n"); return 1; }

    test_extension_lift();
    test_first_scan();
    test_rescan_reads_nothing();
    test_deltas();
    test_cancel_never_commits();
    test_worker_commits_when_complete();
    test_index_io();
    test_bad_root();
    test_multi_root_scan();
    test_offline_root_records_survive();
    test_no_root_readable();
    test_cancel_with_two_roots();
    test_real_collection();

    fs::remove_all(P(g_root), ec);
    std::printf("library_scanner_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
