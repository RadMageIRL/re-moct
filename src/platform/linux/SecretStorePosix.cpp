// secret-at-rest (Linux backend): machine-salt keystream XOR of the sensitive
// Last.fm / ListenBrainz conf fields.
//
// THIS IS OBFUSCATION, NOT ENCRYPTION. It raises the bar against CASUAL disclosure
// (synced folders, backups, pasted logs, screen shares) - a value that used to be
// grep-able plaintext is now machine-salted base64. Anyone who can run code as this
// user can trivially reverse it; defending against that is explicitly out of scope
// (that is DPAPI's job on Windows, and the deferred keyring backend on Linux).
// Do not call this "encryption" anywhere.
//
// Keystream = MD5(appSalt || machineId || counter) blocks (vendored lib/md5.c,
// already compiled in - no new dependency), XORed over the plaintext. Prefix
// "enc:obf:", payload is base64.

#include "SecretStore.h"
#include "Base64.h"
extern "C" {
#include "md5.h"        // vendored public-domain MD5 (Openwall) - C linkage, wrap the include
}

#include <string>
#include <optional>
#include <fstream>
#include <unistd.h>     // gethostname

namespace {

// Fixed application salt. Combined with a per-machine id below.
const std::string kAppSalt = "RE-MOCT/secret-at-rest/v1";

// Best-effort machine binding. Prefer /etc/machine-id; fall back to the hostname;
// fall back to a fixed constant if neither is readable (e.g. a locked-down
// container). NEVER fails - the obfuscation still applies, it is just not
// machine-bound in the fallback case.
std::string machineId() {
    std::ifstream f("/etc/machine-id");
    if (f) {
        std::string id;
        std::getline(f, id);
        if (!id.empty()) return id;
    }
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0')
        return std::string(host);
    return "remoct-fixed-machine-fallback";
}

// XOR data against the MD5-block keystream. Symmetric, so this both obfuscates
// (on protect) and de-obfuscates (on decode).
std::string xorStream(const std::string& data) {
    const std::string base = kAppSalt + "|" + machineId() + "|";
    std::string out(data.size(), '\0');
    std::size_t off = 0;
    unsigned counter = 0;
    while (off < data.size()) {
        MD5_CTX ctx;
        MD5_Init(&ctx);
        const std::string block = base + std::to_string(counter++);
        MD5_Update(&ctx, block.data(), (unsigned long)block.size());
        unsigned char digest[16];
        MD5_Final(digest, &ctx);
        for (std::size_t i = 0; i < 16 && off < data.size(); ++i, ++off)
            out[off] = (char)((unsigned char)data[off] ^ digest[i]);
    }
    return out;
}

} // namespace

std::string secret::protect(const std::string& plain) {
    return "enc:obf:" + base64::encode(xorStream(plain));
}

std::optional<std::string> secret::detail::backendDecode(const std::string& tag,
                                                         const std::string& raw) {
    if (tag != "obf") return std::nullopt;   // enc:dpapi: or unknown -> not this platform
    // XOR is symmetric and unauthenticated: it always yields bytes. A conf carried
    // from a different machine de-obfuscates to garbage, the scrobble auth then
    // fails, and the user re-authenticates - the same graceful outcome as a hard
    // decode failure, without needing an integrity tag.
    return xorStream(raw);
}
