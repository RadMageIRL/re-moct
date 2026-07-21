// podcast_state_test - the slice-3 identity + persistence contract, headless.
//
//   1. podcastEpisodeId() (PodcastFeed.h): the guid -> audio_url -> hash(title+date)
//      precedence used by both resume and played-state.
//   2. DigiConfig podcast_progress round-trip: resume position + played flag survive
//      save/load, "New" (no record) reads as 0/false, and pure-default entries are
//      not persisted. Redirects the config dir to a temp via the env var configPath()
//      reads (APPDATA on Windows, HOME elsewhere), asserting the redirect took first
//      so a run can never touch the real remoct.conf. Pure, both matrix jobs.

#include "PodcastFeed.h"
#include "Config.h"

#include <cstdio>
#include <string>
#include <filesystem>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <cstdlib>
#endif

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool cond, const char* what) {
    std::printf("%-60s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) ++failures;
}

static PodcastEpisode makeEp(const std::string& guid, const std::string& url,
                             const std::string& title, const std::string& date) {
    PodcastEpisode e;
    e.guid = guid; e.audio_url = url; e.title = title; e.pub_date_raw = date;
    return e;
}

int main() {
    // ── podcastEpisodeId precedence ────────────────────────────────────────
    check(podcastEpisodeId(makeEp("g1", "u1", "T", "D")) == "g1", "id: guid wins");
    check(podcastEpisodeId(makeEp("", "u1", "T", "D")) == "u1", "id: audio_url when no guid");
    {
        std::string h = podcastEpisodeId(makeEp("", "", "Title", "Wed, 01 Jan 2025"));
        check(h.rfind("h:", 0) == 0, "id: hash fallback is h:<hex>");
        // Deterministic + distinguishes different (title,date).
        check(podcastEpisodeId(makeEp("", "", "Title", "Wed, 01 Jan 2025")) == h, "id: hash deterministic");
        check(podcastEpisodeId(makeEp("", "", "Other", "Wed, 01 Jan 2025")) != h, "id: hash varies by title");
        check(podcastEpisodeId(makeEp("", "", "Title", "Thu, 02 Jan 2025")) != h, "id: hash varies by date");
    }
    check(podcastEpisodeId(makeEp("", "", "", "")).empty(), "id: empty when nothing to key on");

    // ── DigiConfig podcast_progress round-trip ─────────────────────────────
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec) / "remoct_podstate_test";
    fs::create_directories(tmp, ec);
#ifdef _WIN32
    SetEnvironmentVariableA("APPDATA", tmp.string().c_str());
#else
    setenv("HOME", tmp.string().c_str(), 1);
#endif
    std::string cp = DigiConfig::configPath();
    bool redirected = cp.find("remoct_podstate_test") != std::string::npos;
    check(redirected, "config dir redirected to temp (real config untouched)");

    if (redirected) {
        std::remove(cp.c_str());   // clean slate
        DigiConfig a;
        a.setPodcastEpisodePos("ep-in-progress", 123.5);
        a.setPodcastEpisodePlayed("ep-played", true);
        a.setPodcastEpisodePos("ep-both", 60.0);
        a.setPodcastEpisodePlayed("ep-both", true);
        a.setPodcastEpisodePos("ep-default", 0.0);   // pure default -> must NOT persist
        a.save();

        DigiConfig b;
        b.load();
        check(b.podcastEpisodePos("ep-in-progress") == 123.5, "in-progress pos round-trips");
        check(b.podcastEpisodePlayed("ep-in-progress") == false, "in-progress not played");
        check(b.podcastEpisodePlayed("ep-played") == true, "played flag round-trips");
        check(b.podcastEpisodePos("ep-played") == 0.0, "played episode has no resume pos");
        check(b.podcastEpisodePos("ep-both") == 60.0 && b.podcastEpisodePlayed("ep-both"),
              "pos + played coexist");
        // "New" = absence of a record.
        check(b.podcastEpisodePos("ep-never-seen") == 0.0 &&
              !b.podcastEpisodePlayed("ep-never-seen"), "unseen episode reads new (0/false)");
        check(b.podcast_progress.find("ep-default") == b.podcast_progress.end(),
              "pure-default entry was not persisted");
    }

    std::printf(failures == 0 ? "\npodcast_state_test: ALL PASS\n"
                              : "\npodcast_state_test: %d FAILURE(S)\n", failures);
    return failures ? 1 : 0;
}
