// hls_prime_test.cpp — pure unit tests for the prime-to-music-boundary scan
// (iHeart re-pin clip fix, Section A). Header-only; returns nonzero on failure.
//
// The decisive property for the FAIL-SAFE requirement: hlsFirstMusicBoundary must
// return -1 (so the caller keeps the unchanged live-edge-minus-2 prime = byte
// identical to today) whenever there is no CLEAN in-window ad->music boundary. It
// may only return a positive index when a music segment is genuinely preceded by a
// non-music segment in the same window.
#include "HlsPrime.h"
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// Build a realistic media playlist from a spot string ('M'/'F' music, 'T' ad,
// '.' = a segment with no song_spot attr). Interleaves URI + a discontinuity line
// so the scan is proven to count only #EXTINF lines. `esc` toggles the escaped
// song_spot=\"X\" form (real body) vs the bare song_spot="X" form.
static std::string manifest(const std::string& spots, bool esc = true) {
    std::string b = "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:100\n";
    for (size_t i = 0; i < spots.size(); ++i) {
        if (i == 0) b += "#EXT-X-DISCONTINUITY\n";
        b += "#EXTINF:10,title=\"seg\",artist=\"a\",url=\"";
        if (spots[i] != '.') {
            b += "song_spot=";
            b += esc ? "\\\"" : "\"";
            b += spots[i];
            b += esc ? "\\\"" : "\"";
            b += " ";
        }
        b += "length=\\\"00:00:10\\\" \"\n";
        b += "https://cloud-proxy-hls.example/zc1/main/" + std::to_string(100 + i) + ".aac\n";
    }
    return b;
}

int main() {
    // --- fallback cases: must return -1 (caller keeps live-edge-minus-2 prime) ---
    CHECK(hlsFirstMusicBoundary("") == -1);                    // empty body
    CHECK(hlsFirstMusicBoundary(manifest("MMM")) == -1);       // window opens on music (start fell off)
    CHECK(hlsFirstMusicBoundary(manifest("TTT")) == -1);       // no music at all
    CHECK(hlsFirstMusicBoundary(manifest("...")) == -1);       // no song_spot attrs (slate)
    CHECK(hlsFirstMusicBoundary(manifest("M"))   == -1);       // single music seg, no boundary

    // --- clean boundary cases: return the first music index preceded by non-music ---
    CHECK(hlsFirstMusicBoundary(manifest("TTMM")) == 2);       // ad pod -> music at index 2
    CHECK(hlsFirstMusicBoundary(manifest("TM"))   == 1);       // one ad -> music at index 1
    CHECK(hlsFirstMusicBoundary(manifest("MTM"))  == 2);       // leading M ignored (not preceded by ad)
    CHECK(hlsFirstMusicBoundary(manifest(".M"))   == 1);       // slate -> music
    CHECK(hlsFirstMusicBoundary(manifest("TTMT")) == 2);       // first music wins, later ad irrelevant
    CHECK(hlsFirstMusicBoundary(manifest("TF"))   == 1);       // song_spot=F counts as music too

    // --- bare (unescaped) song_spot form must parse identically ---
    CHECK(hlsFirstMusicBoundary(manifest("TTMM", false)) == 2);
    CHECK(hlsFirstMusicBoundary(manifest("MMM",  false)) == -1);

    if (g_fail) { std::printf("hls_prime_test: %d FAILED\n", g_fail); return 1; }
    std::printf("hls_prime_test: all passed\n");
    return 0;
}
