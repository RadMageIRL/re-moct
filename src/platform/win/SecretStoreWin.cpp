// secret-at-rest (Windows backend): DPAPI user-bound encryption of the sensitive
// Last.fm / ListenBrainz conf fields. CryptProtectData ties the blob to the
// current user's login secret, so a copied conf will not decrypt for another user
// or on another machine - it fails cleanly into the app's re-auth path.
//
// Prefix "enc:dpapi:". The payload is the base64 of the DPAPI DATA_BLOB.

#include "SecretStore.h"
#include "Base64.h"

#include <windows.h>
#include <wincrypt.h>   // CryptProtectData / CryptUnprotectData (needs crypt32)

#include <string>
#include <optional>

namespace {

// A fixed, compiled-in entropy blob passed to DPAPI. This is TIDINESS ONLY, not a
// security boundary: it namespaces our blobs so they cannot be cross-decrypted by
// another app's DPAPI call, but it lives in the binary and adds no real secrecy.
// The actual protection is DPAPI's per-user login secret.
const BYTE kEntropy[] = {
    'R','E','-','M','O','C','T','/','s','e','c','r','e','t','-','a','t','-','r','e','s','t'
};

DATA_BLOB entropyBlob() {
    DATA_BLOB b;
    b.pbData = const_cast<BYTE*>(kEntropy);
    b.cbData = (DWORD)sizeof(kEntropy);
    return b;
}

} // namespace

std::string secret::protect(const std::string& plain) {
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = (DWORD)plain.size();
    DATA_BLOB ent = entropyBlob();
    DATA_BLOB out{};

    // CRYPTPROTECT_UI_FORBIDDEN: never prompt, ever (this runs on a background save).
    if (!CryptProtectData(&in, L"RE-MOCT secret", &ent, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        // Should not happen for a logged-in user. Persist nothing sensitive rather
        // than falling back to plaintext; the field re-auths on next launch.
        return std::string();
    }

    std::string raw(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return "enc:dpapi:" + base64::encode(raw);
}

std::optional<std::string> secret::detail::backendDecode(const std::string& tag,
                                                         const std::string& raw) {
    if (tag != "dpapi") return std::nullopt;   // enc:obf: or unknown -> not this platform

    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(raw.data()));
    in.cbData = (DWORD)raw.size();
    DATA_BLOB ent = entropyBlob();
    DATA_BLOB out{};

    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        // Different user/machine, tampered, or truncated blob -> re-auth.
        return std::nullopt;
    }

    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return plain;
}
