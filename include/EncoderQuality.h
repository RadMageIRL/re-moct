// EncoderQuality.h - the ONE home for encoder quality display + modal editing
// (encoder-bitrate-mode slice).
//
// Two panels edit and render lossy quality now: the rip-confirm modal (rip
// config fields) and the [Rec] panel (its own rec_* fields). To keep the
// "V0  VBR" / "256 kbps  CBR" formatting from drifting between them, the label
// is built HERE, once, and fed each panel's own values - the label is shared,
// the values are not. The Left/Right axis-cycle and the {96,128,256,320}k snap
// live here too so the UI handler and the unit test drive the same pure logic.
//
// Nothing in this header knows about Config, UIManager, or curses - it is a
// pure function of (format, mode flag, value), unit-testable in isolation.
#pragma once

#include "RipFormats.h"

#include <string>

// The CBR / Opus bitrate ladder the modal cycles (kbps). MP3 CBR and Opus both
// pick from this set; MP3 VBR uses the V-scale instead (no bitrate axis).
inline constexpr int kEncoderBitratesKbps[] = { 96, 128, 256, 320 };
inline constexpr int kEncoderBitrateCount =
    (int)(sizeof(kEncoderBitratesKbps) / sizeof(kEncoderBitratesKbps[0]));

// Nearest ladder index to an arbitrary bitrate (config may load any 6k-510k
// value; the modal snaps to the ladder on first edit - F4).
inline int snapBitrateIndex(int bps) {
    const int kbps = bps / 1000;
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < kEncoderBitrateCount; ++i) {
        int d = kEncoderBitratesKbps[i] - kbps;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// Snap an arbitrary bitrate (bps) to the nearest ladder value (bps).
inline int snapBitrate(int bps) {
    return kEncoderBitratesKbps[snapBitrateIndex(bps)] * 1000;
}

// Step the bitrate along the ladder by dir (-1/+1), wrapping. Snaps first, so a
// config-loaded off-ladder value lands on the ladder on the first press.
inline int cycleBitrate(int bps, int dir) {
    int i = snapBitrateIndex(bps);
    i = ((i + dir) % kEncoderBitrateCount + kEncoderBitrateCount) % kEncoderBitrateCount;
    return kEncoderBitratesKbps[i] * 1000;
}

// Step the LAME V-scale "V0".."V9" by dir (-1/+1), wrapping. Unparseable input
// resets to V0 (the pre-config literal), matching parseMp3VbrQ's defense.
inline std::string cycleMp3Vbr(const std::string& v, int dir) {
    int q = 0;
    if (v.size() == 2 && (v[0] == 'V' || v[0] == 'v') && v[1] >= '0' && v[1] <= '9')
        q = v[1] - '0';
    q = ((q + dir) % 10 + 10) % 10;
    return std::string("V") + (char)('0' + q);
}

// Step the AAC VBR quality level 1..5 (5 best) by dir (-1/+1), wrapping. Out-of-
// range input clamps into the ladder first (config may load anything).
inline int cycleAacVbr(int level, int dir) {
    if (level < 1) level = 1; else if (level > 5) level = 5;
    int i = (level - 1);                                  // 0..4
    i = ((i + dir) % 5 + 5) % 5;
    return i + 1;
}

// The quality string shown in a format row. alt_mode is "is CBR" for both lossy
// codecs (MP3 default VBR, Opus default VBR, so alt_mode=false is each default).
// mp3_v is the LAME V-scale ("V0".."V9"); bitrate_bps is the CBR/Opus bitrate.
// flac_level/wavpack_mode only matter for those formats (the rec panel never
// asks for them, so they default).
inline std::string encoderQualityLabel(RipFormat f, bool alt_mode,
                                       const std::string& mp3_v, int bitrate_bps,
                                       int flac_level = 5,
                                       const std::string& wavpack_mode = "normal",
                                       int aac_vbr_level = 4) {
    switch (f) {
        case RipFormat::Flac:    return "level " + std::to_string(flac_level);
        case RipFormat::Wav:     return "16-bit PCM";
        case RipFormat::WavPack: return wavpack_mode;
        case RipFormat::Mp3:
            return alt_mode ? std::to_string(bitrate_bps / 1000) + " kbps  CBR"
                            : mp3_v + "  VBR";
        case RipFormat::Opus:
            return std::to_string(bitrate_bps / 1000) + " kbps  "
                 + (alt_mode ? "CBR" : "VBR");
        case RipFormat::M4a:
            return alt_mode ? std::to_string(bitrate_bps / 1000) + " kbps  CBR"
                            : "VBR " + std::to_string(aac_vbr_level);
    }
    return "";
}

// The axis-hint line naming what Left/Right edits on the focused row (or nullptr
// when the row has no editable axis - FLAC/WAV/WavPack/Copy). MP3 swaps hint
// with mode; Opus stays on bitrate.
inline const char* encoderAxisHint(RipFormat f, bool alt_mode) {
    switch (f) {
        case RipFormat::Mp3:
            return alt_mode ? "<-/-> bitrate (96/128/256/320)   [M] mode"
                            : "<-/-> quality (V0-V9)   [M] mode";
        case RipFormat::Opus:
            return "<-/-> bitrate (96/128/256/320)   [M] mode";
        case RipFormat::M4a:
            return alt_mode ? "<-/-> bitrate (96/128/256/320)   [M] mode"
                            : "<-/-> quality (VBR 1-5)   [M] mode";
        default:
            return nullptr;
    }
}
