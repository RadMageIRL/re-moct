#pragma once
#include <string>
#include <vector>
#include <cstdint>

// CoverArt — album-cover resolution shared across the app.
//
// External cover URLs (Discord Rich Presence takes a URL) and downloaded image
// bytes (the CD ripper embeds covers into tags). Sources: Cover Art Archive by
// MusicBrainz id, iTunes and Deezer by free text. Extracted from CDRipper so a
// consumer (e.g. the radio/Discord path) can resolve art without depending on the
// ripper. Every function below performs BLOCKING network I/O — call off the UI
// thread. Empty return ("" / {}) always means "no confident match" — callers keep
// their own fallback (the ripper skips embedding; Discord keeps the RE-MOCT logo).
namespace CoverArt {

// ─── The Cover Art Archive INDEX ────────────────────────────────────────────
// bytesByMbid asks /front-500, a redirect endpoint that returns ONE image by
// construction, so 34 of the 35 images on a box set - including the artwork of
// the disc actually in the drive - were never reachable. This is the listing.
//
// MEASURED on the FFXI PREMIUM BOX release, and the shape matters to anything
// that displays it:
//   * There are NO pixel dimensions in the index. None. Only which thumbnail
//     sizes are published, and all 35 entries publish the same {250,500,1200},
//     so that field discriminates nothing.
//   * `types` DISAGREES with `comment` on the same entry - 19 are typed "Medium"
//     while their comments read "4 Booklet Front", "5 Tracklist". Both are
//     hand-entered. The comment is the informative one; the type is the
//     machine-readable one; neither is authoritative.
//   * There is no relation from an image to a MEDIUM. "3" is a human's note, not
//     a link. Per-disc art can be OFFERED and never resolved.
struct CaaImage {
    std::string id;          // stable per-image id, used as the thumbnail cache key
    std::string comment;     // the uploader's words - the only discriminating field
    std::string type;        // FIRST type only ("Front"/"Medium"/...), "" if none
    std::string image_url;   // full size, fetched only when chosen
    std::string thumb_url;   // the 250px variant, for the preview
    bool        front = false;   // what /front-500 would have returned
};

// The index for a release, in the order the archive returns it. Empty on any
// failure, which the caller treats as "nothing to choose from". BLOCKING.
std::vector<CaaImage> indexByMbid(const std::string& mb_id);

// The small front cover, for previewing the automatic pick without fetching the
// index at all. ~10 KB against front-500's ~30 KB. BLOCKING.
std::vector<uint8_t> frontThumbByMbid(const std::string& mb_id);

// Downloaded image bytes. Used by the ripper to embed a front cover.
std::vector<uint8_t> bytesByMbid(const std::string& mb_id);                  // Cover Art Archive
std::vector<uint8_t> bytesByText(const std::string& artist,
                                 const std::string& album);                  // iTunes -> Deezer fallback

// Download an already-resolved cover URL to image bytes. Returns {} unless the
// body is a real raster image (guards against HTML/JSON error pages served 200).
// Used by the radio Info-pane art path: urlBySong resolves a URL, this fetches
// it. BLOCKING network I/O — call off the UI thread.
std::vector<uint8_t> bytesByUrl(const std::string& url);

// External cover-art URL. Used by Discord Rich Presence.
std::string urlByText(const std::string& artist, const std::string& album); // album-oriented (file/CD)
std::string urlBySong(const std::string& artist, const std::string& title); // song-oriented (radio fallback)

}  // namespace CoverArt
