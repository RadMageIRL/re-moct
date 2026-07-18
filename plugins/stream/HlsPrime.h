// HlsPrime.h — pure prime-to-music-boundary scan for the iHeart re-pin clip fix.
//
// Header-inline (like StringUtils.h) so it is unit-testable off the StreamSource TU.
// PURE: no windows.h, no threads, no network. Given a media playlist body, returns
// the 0-based segment index of the FIRST song_spot=M/F segment that is PRECEDED by a
// non-music segment in the same window -- i.e. a visible ad->music boundary. On a
// fresh connect the segment order equals hls_.pending order, so this index is where
// to prime from to land at the song start. Returns -1 when there is no clean in-window
// boundary (the window opens on music, so the real start fell off the window; or there
// is no music at all): the caller then keeps the unchanged live-edge-minus-2 prime.
// Fails toward -1 (current behaviour) on any ambiguity.
#pragma once
#include <string>
#include <cstddef>

// Extracts the song_spot letter shape used in iHeart EXTINF lines (song_spot=\"M\"),
// tolerating the backslash-escaped and bare quote forms. Reused from hlsPollMedia.
inline int hlsFirstMusicBoundary(const std::string& body) {
    int boundary = -1, seg = 0;
    bool sawNonMusic = false;
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        if (body.compare(i, 8, "#EXTINF:") == 0) {          // one #EXTINF == one segment
            char spot = 0;
            std::size_t sp = body.find("song_spot=", i);
            if (sp != std::string::npos && sp < e) {
                std::size_t c = sp + 10;
                while (c < e && (body[c] == '\\' || body[c] == '"')) ++c;
                if (c < e) spot = body[c];
            }
            const bool music = (spot == 'M' || spot == 'F');
            if (music) { if (sawNonMusic && boundary < 0) boundary = seg; }
            else       { sawNonMusic = true; }
            ++seg;
        }
        i = e + 1;
    }
    return boundary;
}
