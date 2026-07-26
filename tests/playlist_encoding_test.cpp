// playlist_encoding_test - a legacy 8-bit playlist must LOAD, not crash.
//
// A .m3u or .pls written by an older tool on Windows is CP1252, so an accented
// filename inside it is a raw high byte and the line is not valid UTF-8. On
// Windows std::filesystem decodes a narrow path as UTF-8 and THROWS
// ("Illegal byte sequence") on anything else - from the plain fs::path
// constructor and from fs::exists(p, ec) alike, because the conversion runs
// before the error code applies.
//
// Before the fix that threw straight out of loadPlaylist, and nothing in
// UIManager catches, so opening one ordinary legacy playlist terminated the
// application. This test pins both halves of the fix: the loaders no longer
// throw, AND the track actually loads, because the line is rescued to UTF-8
// rather than merely skipped.
//
// Device-free apart from a temp directory; runs on both matrix jobs. On Linux
// none of this ever threw (narrow paths are raw bytes there), so the test is
// really asserting that the rescue is correct on both, not just safe on one.

#include "PlaylistManager.h"
#include "StringUtils.h"

#include <clocale>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

static fs::path P(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(utf8_to_wide(utf8));
#else
    return fs::path(utf8);
#endif
}

static std::string g_dir;
static std::string join(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

static void writeBytes(const std::string& path, const std::string& bytes) {
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

// ── The encoding helpers themselves ─────────────────────────────────────────
static void test_helpers() {
    CHECK(isValidUtf8(""),                          "empty is valid");
    CHECK(isValidUtf8("plain ascii"),               "ascii is valid");
    CHECK(isValidUtf8("caf\xC3\xA9"),               "2-byte sequence valid");
    CHECK(isValidUtf8("\xE2\x80\x9Cq\xE2\x80\x9D"), "3-byte sequence valid");
    CHECK(isValidUtf8("\xF0\x9F\x8E\xB5"),          "4-byte sequence valid");

    CHECK(!isValidUtf8("caf\xE9"),          "raw latin-1 byte is invalid");
    CHECK(!isValidUtf8("x\x92"),            "cp1252 quote byte is invalid");
    CHECK(!isValidUtf8("x\x80"),            "lone continuation is invalid");
    CHECK(!isValidUtf8("x\xC3"),            "truncated sequence is invalid");
    CHECK(!isValidUtf8("\xC0\xAF"),         "overlong form is invalid");
    CHECK(!isValidUtf8("\xED\xA0\x80"),     "surrogate half is invalid");
    CHECK(!isValidUtf8("\xF5\x80\x80\x80"), "out of range is invalid");

    // Valid UTF-8 must pass through completely untouched - a modern playlist
    // has to round-trip byte for byte.
    const char* keep[] = {"", "ascii", "caf\xC3\xA9", "\xF0\x9F\x8E\xB5"};
    for (const char* s : keep) CHECK(ensure_utf8(s) == s, "untouched: [%s]", s);

    // CP1252 rescue produces the right codepoints.
    CHECK(cp1252_to_utf8("caf\xE9") == "caf\xC3\xA9",  "0xE9 -> U+00E9");
    CHECK(cp1252_to_utf8("\x92")    == "\xE2\x80\x99", "0x92 -> U+2019 right quote");
    CHECK(cp1252_to_utf8("\x80")    == "\xE2\x82\xAC", "0x80 -> U+20AC euro");
    CHECK(cp1252_to_utf8("\x81")    == "\xEF\xBF\xBD", "undefined -> U+FFFD, never dropped");
    CHECK(isValidUtf8(cp1252_to_utf8("\x81\x8D\x8F\x90\x9D")),
          "every undefined byte still yields valid UTF-8");

    // Whatever goes in, valid UTF-8 comes out. That is the property the
    // filesystem calls downstream actually depend on.
    for (int b = 0; b < 256; ++b) {
        const std::string s(1, (char)(unsigned char)b);
        CHECK(isValidUtf8(ensure_utf8(s)), "ensure_utf8 output valid for byte 0x%02X", b);
    }
}

// ── A legacy CP1252 .m3u loads the track it names ───────────────────────────
static void test_latin1_m3u() {
    // The real file on disk, named in UTF-8 (which is what the OS reports).
    const std::string audio_utf8 = join(g_dir, "Caf\xC3\xA9.mp3");
    writeBytes(audio_utf8, "not really audio, but the loader only stats it");

    // The playlist names it the way a CP1252 tool would: a single 0xE9 byte.
    const std::string m3u = join(g_dir, "legacy.m3u");
    writeBytes(m3u, "#EXTM3U\nCaf\xE9.mp3\n");

    PlaylistManager pm;
    int n = 0;
    bool threw = false;
    try { n = pm.loadPlaylist(m3u); } catch (...) { threw = true; }

    CHECK(!threw, "legacy .m3u does not throw out of loadPlaylist");
    CHECK(n == 1, "the track LOADED rather than being skipped (n=%d)", n);
    CHECK(pm.size() == 1, "playlist size=%zu", pm.size());
    if (pm.size() == 1)
        CHECK(pm.entries()[0].path.find("\xC3\xA9") != std::string::npos,
              "stored path is UTF-8: [%s]", pm.entries()[0].path.c_str());
}

// ── The same for .pls ───────────────────────────────────────────────────────
static void test_latin1_pls() {
    const std::string pls = join(g_dir, "legacy.pls");
    writeBytes(pls, "[playlist]\nFile1=Caf\xE9.mp3\nTitle1=Caf\xE9\nNumberOfEntries=1\nVersion=2\n");

    PlaylistManager pm;
    int n = 0;
    bool threw = false;
    try { n = pm.loadPlaylist(pls); } catch (...) { threw = true; }
    CHECK(!threw, "legacy .pls does not throw");
    CHECK(n == 1, "pls track loaded (n=%d)", n);
}

// ── And for .xspf ───────────────────────────────────────────────────────────
static void test_latin1_xspf() {
    const std::string x = join(g_dir, "legacy.xspf");
    writeBytes(x, "<?xml version=\"1.0\"?><playlist><trackList><track>"
                  "<location>Caf\xE9.mp3</location></track></trackList></playlist>");

    PlaylistManager pm;
    int n = 0;
    bool threw = false;
    try { n = pm.loadPlaylist(x); } catch (...) { threw = true; }
    CHECK(!threw, "legacy .xspf does not throw");
    CHECK(n == 1, "xspf track loaded (n=%d)", n);
}

// ── A hostile playlist must degrade, never abort the whole load ─────────────
static void test_hostile_entries_skip_not_abort() {
    const std::string good = join(g_dir, "good.mp3");
    writeBytes(good, "x");

    std::string body = "#EXTM3U\n";
    body += "x\x80\x80\x80.mp3\n";        // lone continuations
    body += "y\xC3.mp3\n";                 // truncated sequence
    body += std::string("nul") + '\0' + ".mp3\n";
    body += "good.mp3\n";                  // the one that must survive
    writeBytes(join(g_dir, "hostile.m3u"), body);

    PlaylistManager pm;
    bool threw = false;
    int n = 0;
    try { n = pm.loadPlaylist(join(g_dir, "hostile.m3u")); } catch (...) { threw = true; }
    CHECK(!threw, "hostile playlist does not throw");
    CHECK(n >= 1, "the good entry still loaded despite bad neighbours (n=%d)", n);
}

int main() {
    std::setlocale(LC_ALL, "");   // the app's own call - this is what selects the codepage

    std::error_code ec;
    g_dir = (fs::temp_directory_path(ec) / "remoct_pl_enc_test").string();
    fs::remove_all(P(g_dir), ec);
    fs::create_directories(P(g_dir), ec);
    if (ec) { std::printf("cannot create temp dir\n"); return 1; }

    test_helpers();
    test_latin1_m3u();
    test_latin1_pls();
    test_latin1_xspf();
    test_hostile_entries_skip_not_abort();

    fs::remove_all(P(g_dir), ec);
    std::printf("playlist_encoding_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
