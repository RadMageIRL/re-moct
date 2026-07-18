// art_miss_cache_test — the time-bounded art negative cache contract
// (radio-art-refresh-fix, Fix #2). Pins the two properties that justify the
// cache's existence in either direction:
//   - a miss RATE-LIMITS: within the TTL the key stays suppressed (a genuine
//     no-art song is attempted once per window, never hammered per tick);
//   - a miss SELF-HEALS: at/after the TTL the entry expires (and is erased),
//     so a transient network failure costs one rotation window, not the
//     whole session — the pre-fix set poisoned the song permanently.
// Tick math is the tree's uint32 wrap-safe idiom; the wrap case is pinned too.
#include "ArtMissCache.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main() {
    const uint32_t TTL = ArtMissCache::TTL_MS;

    {   // rate-limit: fresh miss suppresses through the window
        ArtMissCache c;
        c.add("Artist\tSong", 1000);
        CHECK( c.hit("Artist\tSong", 1000));
        CHECK( c.hit("Artist\tSong", 1000 + TTL / 2));
        CHECK( c.hit("Artist\tSong", 1000 + TTL - 1));
        CHECK(!c.hit("Other\tKey",  1000));            // unknown keys never hit
    }
    {   // self-heal: expiry releases the key (and erases the entry)
        ArtMissCache c;
        c.add("A\tB", 5000);
        CHECK(!c.hit("A\tB", 5000 + TTL));             // exactly at TTL: expired
        CHECK(!c.hit("A\tB", 5000));                   // erased: even an "earlier"
                                                       // probe no longer hits
        c.add("A\tB", 9000);                           // re-miss re-arms cleanly
        CHECK( c.hit("A\tB", 9000 + 1));
    }
    {   // uint32 tick wrap: an entry added near the wrap still expires correctly
        ArtMissCache c;
        const uint32_t near_wrap = 0xFFFFFFF0u;
        c.add("W\tK", near_wrap);                      // expiry wraps past zero
        CHECK( c.hit("W\tK", near_wrap + 5));          // still fresh across the wrap
        CHECK( c.hit("W\tK", near_wrap + TTL - 1));
        CHECK(!c.hit("W\tK", near_wrap + TTL));        // expired post-wrap
    }

    std::printf(g_fail ? "art_miss_cache_test: %d FAILED\n"
                       : "art_miss_cache_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
