// secret_store_test - pure unit test of the secret-at-rest layer:
//   1. base64 (include/Base64.h): byte-exact round-trip + DEFENSIVE decode
//      (malformed/truncated/out-of-alphabet -> nullopt, never a crash).
//   2. secret::protect / secret::unprotect (include/SecretStore.h + the platform
//      backend): same-machine round-trip, legacy-plaintext passthrough, and the
//      "corrupt or foreign value -> nullopt -> re-auth" contract.
//
// Runs in BOTH matrix jobs: on Windows it exercises the DPAPI backend (CI runners
// have a user profile, so CryptProtectData round-trips); on Linux the machine-salt
// obfuscation backend. The live Last.fm / ListenBrainz auth flows are the on-the-
// wire layer above this.

#include "Base64.h"
#include "SecretStore.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;

#define CHECK(cond) do {                                                      \
    if (!(cond)) {                                                            \
        ++g_fail;                                                             \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                         \
} while (0)

static std::string bytes(std::initializer_list<int> v) {
    std::string s;
    for (int b : v) s += (char)(unsigned char)b;
    return s;
}

int main() {
    // ── base64 round-trip: every tail length (0/1/2 mod 3) + full binary range ──
    std::vector<std::string> samples = {
        "", "f", "fo", "foo", "foob", "fooba", "foobar",
        bytes({0x00}), bytes({0x00, 0x00}), bytes({0xff, 0xff, 0xff}),
        bytes({0x00, 0x01, 0x02, 0xfd, 0xfe, 0xff}),
    };
    // Also every single byte value, and a 0..255 run.
    std::string allbytes;
    for (int i = 0; i < 256; ++i) allbytes += (char)(unsigned char)i;
    samples.push_back(allbytes);

    for (const auto& s : samples) {
        std::string enc = base64::encode(s);
        // Standard base64 is 4-char aligned and only uses the RFC alphabet + '='.
        CHECK(enc.size() % 4 == 0);
        auto dec = base64::decode(enc);
        CHECK(dec.has_value());
        CHECK(dec && *dec == s);
    }

    // Known-answer (standard alphabet + padding): "foobar" -> "Zm9vYmFy",
    // "foo" -> "Zm9v", "fo" -> "Zm8=", "f" -> "Zg==".
    CHECK(base64::encode("foobar") == "Zm9vYmFy");
    CHECK(base64::encode("foo")    == "Zm9v");
    CHECK(base64::encode("fo")     == "Zm8=");
    CHECK(base64::encode("f")      == "Zg==");

    // ── DEFENSIVE decode: malformed input must yield nullopt, never crash ──
    const char* bad[] = {
        "abc",        // length not a multiple of 4
        "ab=c",       // padding mid-quad
        "a===",       // too much padding
        "====",       // all padding
        "Zm9v!===",   // out-of-alphabet '!' + bad padding
        "Zm.v",       // '.' is the MusicBrainz variant, NOT standard -> reject
        "Zg=A",       // char after a pad
        "aaaaa",      // 5 chars
        " Zm9v",      // leading space (whitespace not accepted)
    };
    for (const char* b : bad) CHECK(!base64::decode(b).has_value());

    // ── secret round-trip through the platform backend ──
    std::vector<std::string> secrets = {
        "",                                   // empty
        "a",                                  // 1 byte
        "d41d8cd98f00b204e9800998ecf8427e",   // md5-hex-ish (lastfm session shape)
        "0123456789abcdef0123456789ABCDEF-somereallylongopaquescrobbletoken==",
        std::string(1, '\0') + "with\nnewline\tand\x01\x02binary",  // control/binary
    };
    for (const auto& plain : secrets) {
        std::string stored = secret::protect(plain);
        // Non-empty inputs must produce a self-describing enc: line, never raw plaintext.
        if (!plain.empty()) {
            CHECK(stored.rfind("enc:", 0) == 0);
            CHECK(stored.find(plain) == std::string::npos ||  // no accidental leak of the
                  plain.size() < 4);                          // (short values may coincide in b64)
        }
        auto back = secret::unprotect(stored);
        CHECK(back.has_value());
        CHECK(back && *back == plain);
    }

    // ── legacy plaintext passthrough (case 1 of the dispatch contract) ──
    CHECK(secret::unprotect("plainoldtoken").value_or("<none>") == "plainoldtoken");
    CHECK(secret::unprotect("").value_or("<none>") == "");        // empty -> empty
    CHECK(secret::unprotect("abcdef123456").value_or("") == "abcdef123456");

    // ── corrupt / malformed enc values -> nullopt (case 3) ──
    CHECK(!secret::unprotect("enc:obf").has_value());        // no second ':' -> malformed
    CHECK(!secret::unprotect("enc:obf:!!!!").has_value());   // payload not valid base64
    CHECK(!secret::unprotect("enc:dpapi:@@@@").has_value()); // payload not valid base64
    CHECK(!secret::unprotect("enc:bogus:Zm9v").has_value()); // unknown backend tag

    // ── foreign backend (the other platform's prefix) -> nullopt, not a crash ──
    // enc:dpapi: on Linux, and enc:obf: on Windows, both decode-fail into re-auth.
    // Use a valid-base64 but wrong-backend payload so we exercise backendDecode's
    // tag rejection, not a base64 failure.
#if defined(_WIN32)
    CHECK(!secret::unprotect("enc:obf:Zm9vYmFy").has_value());
#else
    CHECK(!secret::unprotect("enc:dpapi:Zm9vYmFy").has_value());
#endif

    if (g_fail == 0) std::printf("secret_store_test: ALL PASS\n");
    else             std::printf("secret_store_test: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
