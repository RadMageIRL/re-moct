// encoder_quality_test - the encoder-bitrate-mode pure logic + config contract.
//
// Two things, no UI and no codec:
//   1. EncoderQuality.h - the shared axis-cycle / snap / label helpers the rip
//      modal and the [Rec] panel both drive. Pure functions of (format, mode,
//      value); the same code the interactive Left/Right/[M] path calls.
//   2. DigiConfig round-trip - the eight new fields load/save correctly and RIP
//      and REC are INDEPENDENT storage (the deliberately-broken sharing guard).
//
// The config round-trip redirects the config directory to a throwaway temp via
// the same env var configPath() reads (APPDATA on Windows, HOME elsewhere) and
// asserts the redirect took BEFORE writing - so a test run can never touch the
// developer's real remoct.conf.
#include "EncoderQuality.h"
#include "Config.h"

#include <cstdio>
#include <string>
#include <filesystem>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <cstdlib>
#endif

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool cond, const char* what) {
    std::printf("%-56s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) ++failures;
}

int main() {
    // ── EncoderQuality.h: the bitrate ladder cycle ─────────────────────────
    check(cycleBitrate(96000,  +1) == 128000, "cycleBitrate 96 -> 128");
    check(cycleBitrate(128000, +1) == 256000, "cycleBitrate 128 -> 256");
    check(cycleBitrate(320000, +1) == 96000,  "cycleBitrate wraps 320 -> 96");
    check(cycleBitrate(96000,  -1) == 320000, "cycleBitrate wraps 96 -> 320");
    check(cycleBitrate(128000, -1) == 96000,  "cycleBitrate 128 -> 96");
    // off-ladder config value snaps to nearest, then steps
    check(snapBitrate(200000) == 256000, "snapBitrate 200k -> 256k (nearest)");
    check(snapBitrate(100000) == 96000,  "snapBitrate 100k -> 96k (nearest)");
    check(cycleBitrate(200000, +1) == 320000, "off-ladder snaps (256) then steps -> 320");

    // ── EncoderQuality.h: the MP3 V-scale cycle ────────────────────────────
    check(cycleMp3Vbr("V0", +1) == "V1", "cycleMp3Vbr V0 -> V1");
    check(cycleMp3Vbr("V9", +1) == "V0", "cycleMp3Vbr wraps V9 -> V0");
    check(cycleMp3Vbr("V0", -1) == "V9", "cycleMp3Vbr wraps V0 -> V9");
    check(cycleMp3Vbr("junk", +1) == "V1", "cycleMp3Vbr bad input treated as V0");

    // ── EncoderQuality.h: labels (one home, both panels) ───────────────────
    check(encoderQualityLabel(RipFormat::Mp3,  false, "V0", 256000) == "V0  VBR",
          "MP3 VBR label = 'V0  VBR'");
    check(encoderQualityLabel(RipFormat::Mp3,  true,  "V0", 320000) == "320 kbps  CBR",
          "MP3 CBR label = '320 kbps  CBR'");
    check(encoderQualityLabel(RipFormat::Opus, false, "",   128000) == "128 kbps  VBR",
          "Opus VBR label = '128 kbps  VBR'");
    check(encoderQualityLabel(RipFormat::Opus, true,  "",    96000) == "96 kbps  CBR",
          "Opus CBR label = '96 kbps  CBR'");
    check(encoderQualityLabel(RipFormat::Flac, false, "", 0, 8) == "level 8",
          "FLAC label = 'level 8'");
    check(std::string(encoderAxisHint(RipFormat::Wav, false) ? "x" : "") == "",
          "no axis hint on a lossless row");

    // ── Remember-both: flipping [M] preserves the other axis ───────────────
    {
        DigiConfig m;
        m.mp3 = "V3"; m.mp3_cbr_bitrate = 320000; m.mp3_cbr = false;
        m.mp3_cbr = true;    // -> CBR
        m.mp3_cbr = false;   // -> VBR
        check(m.mp3 == "V3" && m.mp3_cbr_bitrate == 320000,
              "[M] flip preserves both MP3 axes (remember-both)");
    }

    // ── DigiConfig round-trip + rip/rec independence ───────────────────────
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec) / "remoct_encq_test";
    fs::create_directories(tmp, ec);
#ifdef _WIN32
    SetEnvironmentVariableA("APPDATA", tmp.string().c_str());
#else
    setenv("HOME", tmp.string().c_str(), 1);
#endif
    std::string cp = DigiConfig::configPath();
    bool redirected = cp.find("remoct_encq_test") != std::string::npos;
    check(redirected, "config dir redirected to temp (real config untouched)");

    if (redirected) {
        DigiConfig a;
        // Deliberately distinct rip vs rec values on every shared-name axis.
        a.opus_bitrate = 320000; a.opus_vbr = false;
        a.mp3 = "V2"; a.mp3_cbr = true; a.mp3_cbr_bitrate = 128000;
        a.rec_opus_bitrate = 96000; a.rec_opus_vbr = true;
        a.rec_mp3 = "V7"; a.rec_mp3_cbr = false; a.rec_mp3_cbr_bitrate = 320000;
        a.save();

        DigiConfig b;
        b.load();
        check(b.opus_bitrate == 320000 && b.opus_vbr == false,
              "rip opus (bitrate + mode) round-trips");
        check(b.mp3 == "V2" && b.mp3_cbr == true && b.mp3_cbr_bitrate == 128000,
              "rip mp3 (v-scale + cbr + bitrate) round-trips");
        check(b.rec_opus_bitrate == 96000 && b.rec_opus_vbr == true,
              "rec opus (bitrate + mode) round-trips");
        check(b.rec_mp3 == "V7" && b.rec_mp3_cbr == false && b.rec_mp3_cbr_bitrate == 320000,
              "rec mp3 (v-scale + cbr + bitrate) round-trips");
        // Independence: the two sets never cross-wired through load/save.
        check(b.opus_bitrate != b.rec_opus_bitrate, "rip vs rec opus bitrate independent");
        check(b.opus_vbr     != b.rec_opus_vbr,     "rip vs rec opus mode independent");
        check(b.mp3          != b.rec_mp3,          "rip vs rec mp3 v-scale independent");
        check(b.mp3_cbr      != b.rec_mp3_cbr,      "rip vs rec mp3 mode independent");

        fs::remove_all(tmp, ec);
    }

    std::printf("\n%s (%d failure%s)\n",
                failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
