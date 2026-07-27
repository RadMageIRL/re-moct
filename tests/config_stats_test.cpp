// config_stats_test - the play-count key normalisation and its migration.
//
// Library slice 13. This is the only slice in the campaign that REWRITES PERSISTED
// USER DATA, so the obligations are different from every other test here: it is not
// enough that the new behaviour is right, the migration must be shown not to lose
// anything and the backup must be shown to be a backup.
//
// The central assertions, in the order they matter:
//
//   1. NO COUNT IS LOST. The reference config had 295 entries summing to 3901 plays,
//      with 17 files holding two case-variant entries. A merge that picked one
//      instead of summing would quietly discard plays - `One of Us` is 22 + 73 and
//      must read 95, not either half.
//   2. NOTHING IS DROPPED. Not keys naming files that no longer exist, not keys
//      outside any library root. Normalising is a RENAME, not a garbage collection.
//   3. THE BACKUP IS NEVER CLOBBERED. The failure mode is a user re-running a build
//      that went wrong and losing the good snapshot.
//   4. PLATFORM SPLIT. Windows folds because NTFS is case-insensitive; Linux must
//      NOT, because two paths differing in case are two different files there and
//      merging them destroys data.
//
// Redirects the config dir to a temp via the env var configPath() reads, and asserts
// the redirect took before writing anything - the real config is never touched.

#include "Config.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <cstdlib>
#endif

namespace fs = std::filesystem;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

// The two real spellings from the reference config: 241 keys carried a lowercase
// drive letter and 54 an uppercase one.
static const char* kUpper = "C:\\Users\\david\\Music\\Joan Osborne\\Relish\\06 One of Us.mp3";
static const char* kLower = "c:\\users\\david\\Music\\Joan Osborne\\Relish\\06 One of Us.mp3";

static std::size_t totalPlays(const DigiConfig& c) {
    std::size_t n = 0;
    for (const auto& kv : c.track_stats) n += (std::size_t)kv.second.play_count;
    return n;
}

// ── 1. The real shape: two case-variants, counts SUMMED ─────────────────────
static void test_merge_sums_counts() {
    DigiConfig c;
    c.track_stats[kUpper] = { 22, 100 };
    c.track_stats[kLower] = { 73, 200 };

    const std::size_t before = totalPlays(c);
    const std::size_t merged = c.mergeStatKeys();

#ifdef _WIN32
    CHECK(merged == 1, "one entry should merge away, got %zu", merged);
    CHECK(c.track_stats.size() == 1, "one file left, got %zu", c.track_stats.size());
    const auto it = c.track_stats.begin();
    CHECK(it->second.play_count == 95, "22 + 73 must be 95, got %d", it->second.play_count);
    CHECK(it->second.last_played == 200,
          "last_played takes the MAXIMUM (not additive), got %lld",
          (long long)it->second.last_played);
#else
    // ON LINUX THESE ARE TWO DIFFERENT FILES. Merging them would invent one play
    // count out of two real ones and destroy data.
    CHECK(merged == 0, "Linux: nothing merges, got %zu", merged);
    CHECK(c.track_stats.size() == 2, "Linux: both entries survive, got %zu",
          c.track_stats.size());
#endif
    CHECK(totalPlays(c) == before, "TOTAL PLAYS MUST BE PRESERVED: %zu -> %zu",
          before, totalPlays(c));
}

// ── The property, over many pairs: sum in == sum out ────────────────────────
// This is the assertion that actually corresponds to the measurement on the real
// config (3901 before, 3901 after) - a per-row check would not catch a merge that
// dropped one group entirely.
static void test_sum_preserved_at_scale() {
    DigiConfig c;
    std::size_t expect = 0;
    for (int i = 0; i < 200; ++i) {
        const std::string up = "C:\\M\\Track " + std::to_string(i) + ".flac";
        std::string lo = up;
        for (char& ch : lo) ch = (char)std::tolower((unsigned char)ch);
        c.track_stats[up] = { i + 1,  1000 + i };
        c.track_stats[lo] = { i + 2,  2000 + i };
        expect += (std::size_t)(i + 1) + (std::size_t)(i + 2);
    }
    const std::size_t before = totalPlays(c);
    CHECK(before == expect, "fixture sanity: %zu vs %zu", before, expect);
    c.mergeStatKeys();
    CHECK(totalPlays(c) == expect, "sum preserved across 200 pairs: %zu vs %zu",
          totalPlays(c), expect);
#ifdef _WIN32
    CHECK(c.track_stats.size() == 200, "200 files after merge, got %zu", c.track_stats.size());
#else
    CHECK(c.track_stats.size() == 400, "Linux: 400 distinct files, got %zu", c.track_stats.size());
#endif
}

// ── 2. Idempotence: a second run changes nothing ────────────────────────────
static void test_idempotent() {
    DigiConfig c;
    c.track_stats[kUpper] = { 22, 100 };
    c.track_stats[kLower] = { 73, 200 };
    c.mergeStatKeys();
    const std::size_t n1 = c.track_stats.size(), t1 = totalPlays(c);

    const std::size_t again = c.mergeStatKeys();
    CHECK(again == 0, "second run merges nothing, got %zu", again);
    CHECK(c.track_stats.size() == n1 && totalPlays(c) == t1,
          "second run changes nothing: %zu/%zu vs %zu/%zu",
          c.track_stats.size(), totalPlays(c), n1, t1);
    CHECK(!c.stats_keys_rewritten, "and reports no keys rewritten the second time");
}

// ── 3. Nothing is dropped ───────────────────────────────────────────────────
// A key naming a file that does not exist, and a key outside any library root, both
// survive. On the reference machine the second case is real: 5 keys point into the
// launch-smoke directory, all 5 files still present. Deciding they no longer count
// is not a thing a user could undo.
static void test_nothing_is_dropped() {
    DigiConfig c;
    c.track_stats["C:\\nowhere\\deleted.flac"]                 = { 7, 100 };
    c.track_stats["C:\\Users\\david\\smoke\\files\\rg.wv"]     = { 3, 100 };
    c.track_stats["D:\\Music\\second-root.flac"]               = { 5, 100 };
    const std::size_t before = c.track_stats.size(), plays = totalPlays(c);

    c.mergeStatKeys();
    CHECK(c.track_stats.size() == before, "no entry dropped: %zu vs %zu",
          c.track_stats.size(), before);
    CHECK(totalPlays(c) == plays, "no plays dropped");
}

// ── 4. recordPlay normalises, so new plays cannot re-split ──────────────────
static void test_record_play_normalises() {
    DigiConfig c;
    c.recordPlay(kUpper);
    c.recordPlay(kLower);
#ifdef _WIN32
    CHECK(c.track_stats.size() == 1, "two spellings, ONE entry, got %zu", c.track_stats.size());
    CHECK(c.track_stats.begin()->second.play_count == 2,
          "count is 2, not two entries of 1, got %d",
          c.track_stats.begin()->second.play_count);
#else
    CHECK(c.track_stats.size() == 2, "Linux: two files, two entries, got %zu",
          c.track_stats.size());
#endif
    // A CD track is still never recorded - pre-existing behaviour, unchanged.
    const std::size_t n = c.track_stats.size();
    c.recordPlay("D:CD Track 3");   // the real CD path shape: <spec>:CD Track N
    CHECK(c.track_stats.size() == n, "CD tracks are still never recorded");
}

// ── 5. Round-trip, the backup, and everything else surviving ────────────────
static void test_migration_on_disk() {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / "remoct_cfgstats_test";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
#ifdef _WIN32
    SetEnvironmentVariableA("APPDATA", tmp.string().c_str());
#else
    setenv("HOME", tmp.string().c_str(), 1);
#endif
    const std::string cp = DigiConfig::configPath();
    const bool redirected = cp.find("remoct_cfgstats_test") != std::string::npos;
    CHECK(redirected, "config dir redirected to temp (the real config is never touched)");
    if (!redirected) return;

    fs::create_directories(fs::path(cp).parent_path(), ec);
    const std::string bak = cp + ".statbak";
    std::remove(cp.c_str());
    std::remove(bak.c_str());

    // A config carrying the split pair AND a representative sample of everything
    // else, so "byte-intact" is asserted rather than hoped for.
    {
        std::ofstream f(cp, std::ios::trunc);
        f << "# RE-MOCT configuration - auto-generated\n";
        f << "volume=0.77\n";       // a gain, clamped to [0,2] on load - not a percentage
        f << "playlist_current=3\n";
        f << "library_root=C:\\Users\\david\\Music\n";
        f << "library_root=D:\\Music\n";
        f << "podcast=https://example.com/feed.xml\tA Show\t\n";
        f << "lastfm-user=dos\n";
        f << "stat=" << kUpper << "|22|100\n";
        f << "stat=" << kLower << "|73|200\n";
        f << "stat=C:\\nowhere\\gone.flac|4|300\n";
    }

    DigiConfig a;
    a.load();

#ifdef _WIN32
    CHECK(a.stats_merged == 1, "load() merged one entry, got %zu", a.stats_merged);
    CHECK(fs::exists(bak), "the backup exists after a migrating load");
#else
    CHECK(a.stats_merged == 0, "Linux: nothing to merge, got %zu", a.stats_merged);
#endif
    CHECK(totalPlays(a) == 99, "22 + 73 + 4 = 99 plays survive load, got %zu", totalPlays(a));

    // Everything else intact, in memory and then through a save/load cycle.
    CHECK(a.volume > 0.76f && a.volume < 0.78f, "volume survives, got %f", (double)a.volume);
    CHECK(a.playlist_current == 3, "playlist position survives");
    CHECK(a.library_roots.size() == 2, "both library_root lines survive, got %zu",
          a.library_roots.size());
    CHECK(a.podcast_feeds.size() == 1, "podcast subscription survives");
    CHECK(a.lastfm_user == "dos", "credentials survive");

    a.save();
    DigiConfig b;
    b.load();
    CHECK(b.stats_merged == 0, "a migrated config does not migrate again, got %zu",
          b.stats_merged);
    CHECK(totalPlays(b) == 99, "plays survive the round trip, got %zu", totalPlays(b));
    CHECK(b.library_roots.size() == 2, "roots survive the round trip");
    CHECK(b.volume > 0.76f && b.volume < 0.78f && b.lastfm_user == "dos",
          "and so does everything else (volume %f, user %s)",
          (double)b.volume, b.lastfm_user.c_str());

    // THE BACKUP IS NEVER CLOBBERED. Enforced by an explicit skip-if-exists rather
    // than by the migration happening to be a no-op the second time - those coincide
    // today but are different guarantees, and this is the one that protects the user.
#ifdef _WIN32
    {
        std::ofstream f(bak, std::ios::trunc);
        f << "SENTINEL - a good pre-migration backup\n";
    }
    // Re-migrate from scratch so the backup path is REACHED, and confirm it is not
    // overwritten even so.
    {
        std::ofstream f(cp, std::ios::trunc);
        f << "stat=" << kUpper << "|1|100\n";
        f << "stat=" << kLower << "|1|100\n";
    }
    DigiConfig d;
    d.load();
    CHECK(d.stats_merged == 1, "the migration ran again on a fresh split config");
    std::string first;
    {   // SCOPED: an open ifstream keeps a Windows file handle, and the remove below
        // would silently fail with the handle still held.
        std::ifstream chk(bak);
        std::getline(chk, first);
    }
    CHECK(first.rfind("SENTINEL", 0) == 0,
          "THE EXISTING BACKUP MUST NOT BE OVERWRITTEN, got [%s]", first.c_str());
#endif

    // No backup is written when there is nothing to migrate - a user whose stats are
    // already clean should not accumulate a stray file.
    fs::remove(bak, ec);
    {
        std::ofstream f(cp, std::ios::trunc);
        f << "volume=50\n";
        f << "stat=c:\\m\\already lower.flac|5|100\n";
    }
    DigiConfig e;
    e.load();
    CHECK(!fs::exists(bak), "no backup when nothing needed rewriting");

    fs::remove_all(tmp, ec);
}

int main() {
    test_merge_sums_counts();
    test_sum_preserved_at_scale();
    test_idempotent();
    test_nothing_is_dropped();
    test_record_play_normalises();
    test_migration_on_disk();

    std::printf("config_stats_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
