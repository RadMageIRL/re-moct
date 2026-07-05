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
