#ifndef REMOCT_BASE64_H
#define REMOCT_BASE64_H

#include <string>
#include <optional>
#include <cstddef>
#include <cstdint>

// Standard Base64 (RFC 4648 alphabet A-Za-z0-9+/, '=' padded). Byte-exact
// round-trip: decode(encode(x)) == x for any byte string x. This is deliberately
// the STANDARD alphabet (unlike MBLookup's MusicBrainz variant) so encrypted
// DPAPI blobs and the obfuscation keystream output survive a round-trip intact.
//
// decode() is DEFENSIVE: any malformed, truncated, or out-of-alphabet input
// returns nullopt - no throw, no undefined behaviour, no out-of-bounds. It runs
// on whatever bytes happen to sit in the conf file, so a corrupt or partial value
// must flow into the caller's "return nullopt -> re-auth" path, never crash.
namespace base64 {

inline std::string encode(const std::string& in) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    const std::size_t n = in.size();
    std::size_t i = 0;
    while (i + 3 <= n) {
        std::uint32_t v = (std::uint32_t)(std::uint8_t)in[i]   << 16
                        | (std::uint32_t)(std::uint8_t)in[i+1] << 8
                        | (std::uint32_t)(std::uint8_t)in[i+2];
        out += T[(v >> 18) & 0x3f];
        out += T[(v >> 12) & 0x3f];
        out += T[(v >> 6)  & 0x3f];
        out += T[ v        & 0x3f];
        i += 3;
    }
    if (n - i == 1) {
        std::uint32_t v = (std::uint32_t)(std::uint8_t)in[i] << 16;
        out += T[(v >> 18) & 0x3f];
        out += T[(v >> 12) & 0x3f];
        out += '=';
        out += '=';
    } else if (n - i == 2) {
        std::uint32_t v = (std::uint32_t)(std::uint8_t)in[i]   << 16
                        | (std::uint32_t)(std::uint8_t)in[i+1] << 8;
        out += T[(v >> 18) & 0x3f];
        out += T[(v >> 12) & 0x3f];
        out += T[(v >> 6)  & 0x3f];
        out += '=';
    }
    return out;
}

inline std::optional<std::string> decode(const std::string& in) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;                       // out of alphabet (padding handled separately)
    };
    const std::size_t n = in.size();
    if (n == 0) return std::string{};    // empty round-trips to empty
    if (n % 4 != 0) return std::nullopt; // standard base64 is 4-char aligned

    std::string out;
    out.reserve(n / 4 * 3);
    for (std::size_t i = 0; i < n; i += 4) {
        const bool last = (i + 4 == n);
        const char c0 = in[i], c1 = in[i+1], c2 = in[i+2], c3 = in[i+3];
        const int v0 = val((unsigned char)c0);
        const int v1 = val((unsigned char)c1);
        if (v0 < 0 || v1 < 0) return std::nullopt;   // first two are never padding
        if (c2 == '=') {                              // "xx==" : one output byte
            if (!last || c3 != '=') return std::nullopt;
            out += (char)((v0 << 2) | (v1 >> 4));
        } else {
            const int v2 = val((unsigned char)c2);
            if (v2 < 0) return std::nullopt;
            if (c3 == '=') {                          // "xxx=" : two output bytes
                if (!last) return std::nullopt;       // padding only in the final quad
                out += (char)((v0 << 2) | (v1 >> 4));
                out += (char)((v1 << 4) | (v2 >> 2));
            } else {                                  // "xxxx" : three output bytes
                const int v3 = val((unsigned char)c3);
                if (v3 < 0) return std::nullopt;
                out += (char)((v0 << 2) | (v1 >> 4));
                out += (char)((v1 << 4) | (v2 >> 2));
                out += (char)((v2 << 6) |  v3);
            }
        }
    }
    return out;
}

} // namespace base64

#endif // REMOCT_BASE64_H
