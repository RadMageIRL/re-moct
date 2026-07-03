// md5_test — pure unit test of the vendored public-domain MD5 (lib/md5.{h,c})
// behind LastFm's api_sig signing (Phase 3 slice 2, one code path on BOTH
// platforms — replaced Windows CryptoAPI + the slice-1 Linux placeholder).
//
// Layers of proof: this file = the RFC 1321 reference vectors (mathematical
// identity with any correct MD5, CryptoAPI included), block-boundary lengths
// (55/56/64/65 — the padding edge cases), and a raw-UTF-8 multibyte vector in
// the exact api_sig concatenation shape (sorted name+value pairs + secret —
// the "Björk" wire-fidelity class). The live scrobble round-trips on each
// platform are the on-the-wire layer. Runs in both matrix jobs.
extern "C" {
#include "md5.h"   // vendored upstream kept byte-verbatim -> extern "C" at the include
}

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK_EQ(input, want) do {                                            \
    std::string got_ = md5Hex_(input);                                        \
    if (got_ != (want)) {                                                     \
        ++g_fail;                                                             \
        std::printf("FAIL %s:%d\n  in   = %s\n  want = %s\n  got  = %s\n",    \
                    __FILE__, __LINE__, #input, (want), got_.c_str());        \
    }                                                                         \
} while (0)

// Mirror of LastFm::md5Hex's wrapper (kept private there by design — this
// test proves the vendored primitive + the hex shape it relies on).
static std::string md5Hex_(const std::string& s) {
    MD5_CTX ctx;
    MD5_Init(&ctx);
    if (!s.empty()) MD5_Update(&ctx, s.data(), (unsigned long)s.size());
    unsigned char digest[16];
    MD5_Final(digest, &ctx);
    static const char* hx = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; ++i) {
        out += hx[digest[i] >> 4];
        out += hx[digest[i] & 0x0F];
    }
    return out;
}

int main() {
    // ── RFC 1321 §A.5 reference vectors ─────────────────────────────────────
    CHECK_EQ(std::string(""),  "d41d8cd98f00b204e9800998ecf8427e");
    CHECK_EQ(std::string("a"), "0cc175b9c0f1b6a831c399e269772661");
    CHECK_EQ(std::string("abc"), "900150983cd24fb0d6963f7d28e17f72");
    CHECK_EQ(std::string("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK_EQ(std::string("abcdefghijklmnopqrstuvwxyz"),
             "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK_EQ(std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"),
             "d174ab98d277d9f5a5611c2c9f419d9f");
    {
        std::string eighty;
        for (int i = 0; i < 8; ++i) eighty += "1234567890";
        CHECK_EQ(eighty, "57edf4a22be3c955ac49da2e2107b67a");
    }

    // ── Padding edge cases (55/56/64/65 bytes straddle the length-field and
    //    block boundaries — where a broken final() shows itself) ─────────────
    CHECK_EQ(std::string(55, 'x'), "04364420e25c512fd958a70738aa8f72");
    CHECK_EQ(std::string(56, 'x'), "668a72d5ba17f08e62dabcafad6db14b");
    CHECK_EQ(std::string(64, 'x'), "c1bb4f81d892b2d57947682aeb252456");
    CHECK_EQ(std::string(65, 'x'), "1bc932052302d074bdec39795fe00cf6");

    // ── Raw-UTF-8 multibyte input in the api_sig concatenation shape
    //    (sorted param name+value pairs, then the secret; bytes hashed as-is,
    //    never widened — the group-(b) wire-fidelity rule). Expected value
    //    computed with an independent implementation (Python hashlib). ───────
    CHECK_EQ(std::string("artistBj\xC3\xB6rktrackJ\xC3\xB3gasecret\xC3\x9F"
                         "e\xC3\xA7ret"),
             "8ff0d8b04879c7e437374fecc99f1a7d");

    if (!g_fail) std::printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
