#pragma once

// ─── The canonical list of audio extensions RE-MOCT will open ────────────────
//
// One list, two readers. PlaylistManager::isSupportedAudio delegates here, and
// the library scanner uses it to decide what to index.
//
// WHY IT IS SHARED RATHER THAN COPIED. Before the library slice this list was a
// file-static inside PlaylistManager.cpp, which no test can link cheaply. A
// second copy in the scanner would therefore have been the only TESTABLE one,
// so the two could drift with nothing able to catch it - and the symptom of
// drift is the library either indexing files the player refuses to open, or
// hiding files it would have played perfectly well. Lifting the list is the
// only arrangement where one test covers both readers.
//
// PURE, and no std::filesystem. Extension extraction is a string operation here
// deliberately: fs::path throws on Windows for a path that is not valid UTF-8
// (measured - see docs/library-index-plan.md section 6), and a scanner that
// walks a real collection must be able to ask "is this audio?" about a name it
// cannot convert. The semantics below match std::filesystem::path::extension
// exactly, which is what the original implementation used.

#include <algorithm>
#include <cctype>
#include <string>

namespace audioext {

// Kept in the order the original PlaylistManager list used, so a diff of the
// lift is obviously a lift.
inline const char* const kAudioExts[] = {
    ".mp3", ".flac", ".ogg", ".opus", ".wav", ".aiff", ".aif",
    ".m4a", ".m4b", ".aac", ".wma", ".mp4", ".wv"
};
inline constexpr std::size_t kAudioExtCount = sizeof(kAudioExts) / sizeof(kAudioExts[0]);

// The lowercased extension INCLUDING the dot, or "" when there is none.
// Mirrors std::filesystem::path::extension():
//   - only the final component counts ("dir.mp3/file" has no extension)
//   - "." and ".." have none
//   - a leading dot is not an extension (".hidden" has none)
inline std::string extensionOf(const std::string& path) {
    const std::size_t sep = path.find_last_of("/\\");
    const std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
    if (name.empty() || name == "." || name == "..") return {};
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

inline bool isSupportedAudio(const std::string& path) {
    const std::string ext = extensionOf(path);
    if (ext.empty()) return false;
    for (std::size_t i = 0; i < kAudioExtCount; ++i)
        if (ext == kAudioExts[i]) return true;
    return false;
}

} // namespace audioext
