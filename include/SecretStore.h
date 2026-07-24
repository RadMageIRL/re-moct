#ifndef REMOCT_SECRETSTORE_H
#define REMOCT_SECRETSTORE_H

#include <string>
#include <optional>
#include <cstddef>

#include "Base64.h"

// At-rest protection for the handful of sensitive Last.fm / ListenBrainz fields so
// they do not sit as grep-able plaintext in remoct.conf. Reasonable protection
// against CASUAL disclosure (synced folders, backups, pasted bug reports, screen
// shares) - explicitly NOT a defence against code running as the user.
//
// Backends (selected at write time by platform):
//   Windows -> DPAPI (user-bound), prefix "enc:dpapi:"  (real encryption)
//   Linux   -> machine-salt keystream XOR, prefix "enc:obf:"  (OBFUSCATION, not
//              encryption - never describe it as encryption)
//
// Storage line format (self-describing, backward + forward compatible):
//   lastfm-session=enc:dpapi:<base64>   written on Windows
//   lastfm-session=enc:obf:<base64>     written on Linux
//   lastfm-session=<barevalue>          legacy plaintext (pre-upgrade), still read
//
// protect()   -> platform backend (SecretStoreWin.cpp / SecretStorePosix.cpp).
// unprotect() -> shared dispatch below; only the backend decode is per-platform.
namespace secret {

// Produce a self-describing "enc:<backend>:<base64>" string for a plaintext value.
// Defined per platform. On the (rare) failure to protect, returns "" - the caller
// then persists nothing sensitive and the app falls back to its re-auth path.
std::string protect(const std::string& plain);

namespace detail {
// Given a recognized backend tag and the raw (already base64-decoded) bytes,
// return the plaintext, or nullopt if THIS platform cannot handle that tag or the
// decode fails (wrong user/machine, corrupt blob). Defined per platform.
std::optional<std::string> backendDecode(const std::string& tag,
                                         const std::string& raw);
} // namespace detail

// Dispatch contract (platform-agnostic, hence shared here):
//   1. No recognized "enc:" prefix, any value -> LEGACY plaintext, returned as-is
//      (includes the empty string).
//   2. "enc:<tag>:<base64>", base64 + backend decode succeed -> plaintext.
//   3. Recognized shape but base64 or backend decode FAILS -> nullopt. The caller
//      stores "" and the user re-authenticates. Never throws, never wipes the file.
inline std::optional<std::string> unprotect(const std::string& stored) {
    static constexpr char kPfx[] = "enc:";
    static constexpr std::size_t kPfxLen = sizeof(kPfx) - 1;   // 4, minus the NUL

    // (1) Legacy plaintext: nothing with our prefix -> hand it back untouched.
    if (stored.rfind(kPfx, 0) != 0) return stored;

    // Parse "enc:<tag>:<payload>". A prefix with no second ':' is malformed.
    const std::size_t tagEnd = stored.find(':', kPfxLen);
    if (tagEnd == std::string::npos) return std::nullopt;
    const std::string tag     = stored.substr(kPfxLen, tagEnd - kPfxLen);
    const std::string payload = stored.substr(tagEnd + 1);

    // (3a) Corrupt / truncated base64 -> re-auth path.
    std::optional<std::string> raw = base64::decode(payload);
    if (!raw) return std::nullopt;

    // (2)/(3b) Backend decodes it, or nullopt for an unknown/foreign tag or a
    // failed decrypt (e.g. an enc:dpapi: conf opened on Linux, or a foreign user).
    return detail::backendDecode(tag, *raw);
}

} // namespace secret

#endif // REMOCT_SECRETSTORE_H
