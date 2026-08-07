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
// PlaylistManager::displayTitleFor - the row-label rule, extracted in library slice 6
// so the tag-reading path and the library's index path cannot format differently.
// populateMetadata now CALLS this, so the two are the same code rather than two copies
// that have to agree; these cases pin the rule itself.
static void test_display_title_for() {
    using PM = PlaylistManager;
    CHECK(PM::displayTitleFor("/m/x.flac", "Muse", "Starlight") == "Muse - Starlight",
          "artist and title -> \"artist - title\"");
    CHECK(PM::displayTitleFor("/m/x.flac", "", "Starlight") == "Starlight",
          "title only -> title");
    CHECK(PM::displayTitleFor("/m/x.flac", "Muse", "") == "x",
          "artist but no title -> the stem, NOT a dangling \"Muse - \"");
    CHECK(PM::displayTitleFor("/m/x.flac", "", "") == "x",
          "neither -> the filename stem");
    CHECK(PM::displayTitleFor("C:\\m\\y.mp3", "", "") == "y", "windows separator stem");
    CHECK(PM::displayTitleFor("/m/two.dots.ogg", "", "") == "two.dots",
          "only the last dot is the extension");
    // The library passes RAW index text (the index deliberately stores tags
    // unfolded, because folding on the way in would be lossy), so the TYPOGRAPHIC
    // normalization has to happen here - and it does: a smart quote comes back as
    // ASCII whichever caller supplied it.
    CHECK(PM::displayTitleFor("/m/x.flac", "", "Don\xE2\x80\x99t") == "Don't",
          "typography is folded here, so both callers get the same string");

    // AND the honest limit, pinned rather than left to be discovered: foldForDisplay
    // passes every byte below 0x80 through verbatim, so an ASCII control byte in a
    // tag survives. The control byte survives because it is ASCII - NOT because
    // non-ASCII is folded away, which is what this used to say and is no longer
    // true of anything. That is pre-existing behaviour of the tag path -
    // populateMetadata has always done exactly this - and the point here is that the
    // library path is IDENTICAL to it rather than better or worse. Stripping controls
    // would be a change to every row in the program, not a library slice.
    const std::string ctl = std::string("Ti") + '\t' + "tle";
    CHECK(PM::displayTitleFor("/m/x.flac", "", ctl) == ctl,
          "an ASCII control byte passes through - same as the tag path, by design");

    // THE REGRESSION GUARD. A row label keeps its language. This is the whole
    // reason the fold was rewritten: every 3- and 4-byte sequence used to become
    // one '?' each, so this album's artist drew as "????" in browser and playlist
    // rows while the directory header above them - which never folded - showed it
    // correctly.
    CHECK(PM::displayTitleFor("/m/x.flac", "\xE6\xB0\xB4\xE7\x94\xB0\xE7\x9B\xB4\xE5\xBF\x97",
                              "\xE6\x9D\xB1\xE4\xBA\xAC")
              == "\xE6\xB0\xB4\xE7\x94\xB0\xE7\x9B\xB4\xE5\xBF\x97 - \xE6\x9D\xB1\xE4\xBA\xAC",
          "CJK survives a row label intact");
}

// foldForDisplay itself: the table from the design note, made executable. Each
// surviving group gets one case, so the contract is checkable rather than prose.
// THE RULE: reject malformed UTF-8 as '?', normalize the table below, pass
// everything else through verbatim. Byte length is never a criterion.
static void test_fold_for_display() {
    // ── 3. PASS: language, whatever its byte length ─────────────────────────
    CHECK(foldForDisplay("\xE6\xB0\xB4\xE7\x94\xB0\xE7\x9B\xB4\xE5\xBF\x97")
              == "\xE6\xB0\xB4\xE7\x94\xB0\xE7\x9B\xB4\xE5\xBF\x97", "CJK passes (3-byte)");
    CHECK(foldForDisplay("\xE3\x82\xAA\xE3\x83\xAA") == "\xE3\x82\xAA\xE3\x83\xAA",
          "katakana passes (3-byte)");
    CHECK(foldForDisplay("\xD0\x9C\xD0\xBE") == "\xD0\x9C\xD0\xBE", "Cyrillic passes (2-byte)");
    CHECK(foldForDisplay("\xF0\x9F\x8E\xB5") == "\xF0\x9F\x8E\xB5", "emoji passes (4-byte)");
    // Group J is GONE: a letter keeps its diacritic, and it no longer matters
    // which letter. "Muller" and "Angstrom" used to disagree in the same pane.
    CHECK(foldForDisplay("caf\xC3\xA9")   == "caf\xC3\xA9",   "e-acute keeps its accent");
    CHECK(foldForDisplay("M\xC3\xBCller") == "M\xC3\xBCller", "u-umlaut keeps its accent");

    // ── 2. NORMALIZE: typography, one case per surviving group ──────────────
    CHECK(foldForDisplay("Don\xE2\x80\x99t")        == "Don't", "A: quotes/primes");
    CHECK(foldForDisplay("\xE2\x80\x9Cq\xE2\x80\x9D") == "\"q\"", "B: double quotes");
    CHECK(foldForDisplay("a\xE2\x80\x94" "b")       == "a-b",   "C: dash variants");
    CHECK(foldForDisplay("so\xC2\xAD" "ft")         == "so-ft", "C: soft hyphen");
    CHECK(foldForDisplay("w\xE2\x80\xA6")           == "w.",    "D: ellipsis");
    CHECK(foldForDisplay("\xE2\x80\xA2")            == "*",     "D: bullet");
    CHECK(foldForDisplay("a\xC2\xA0" "b")           == "a b",   "E: nbsp");
    CHECK(foldForDisplay("a\xE3\x80\x80" "b")       == "a b",   "E: ideographic space");
    CHECK(foldForDisplay("a\xE2\x80\x8B" "b")       == "ab",    "F: zero-width dropped");
    CHECK(foldForDisplay("\xEF\xBB\xBF" "x")        == "x",     "F: BOM dropped");
    CHECK(foldForDisplay("\xC5\x93")                == "oe",    "G: oe ligature");
    CHECK(foldForDisplay("\xC3\x86")                == "AE",    "G: AE ligature");
    CHECK(foldForDisplay(std::string("Ti") + '\t' + "tle") == std::string("Ti") + '\t' + "tle",
          "H: ASCII control passes verbatim");

    // ── 1. REJECT: malformed UTF-8 is '?', one per bad byte ─────────────────
    // These used to fall out of the length rule by accident. Now that length
    // decides nothing they are rejected on purpose - without this, invalid bytes
    // would reach the terminal AND the scrobblers.
    CHECK(foldForDisplay("caf\xE9")           == "caf?", "I: invalid lead byte");
    CHECK(foldForDisplay("x\x80" "y")         == "x?y",  "I: lone continuation");
    CHECK(foldForDisplay("x\xC3")             == "x?",   "I: truncated tail");
    CHECK(foldForDisplay("x\xE2\x28\xA1")     == "x?(?", "I: bad continuation, resyncs");
    CHECK(foldForDisplay("\xC0\xAF")          == "??",   "I: overlong form");
    CHECK(foldForDisplay("\xED\xA0\x80")      == "???",  "I: surrogate half");
    CHECK(foldForDisplay("\xF5\x80\x80\x80")  == "????", "I: out of range");

    // Idempotent: folding folded text changes nothing. The info pane folds at the
    // add() site over values that may already have been folded upstream.
    const std::string once = foldForDisplay("Don\xE2\x80\x99t \xE6\x9D\xB1\xE4\xBA\xAC");
    CHECK(foldForDisplay(once) == once, "fold is idempotent");
}

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
    test_fold_for_display();
    test_display_title_for();
    test_latin1_m3u();
    test_latin1_pls();
    test_latin1_xspf();
    test_hostile_entries_skip_not_abort();

    fs::remove_all(P(g_dir), ec);
    std::printf("playlist_encoding_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
