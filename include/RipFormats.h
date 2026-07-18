// RipFormats.h — the rip output-format table (rip-format-select slice).
//
// ONE data-driven table feeds everything: the modal rows (label, digit =
// index+1, quality text, the * master marker), the output extensions, and
// the encoder factory in CDRipper.cpp. Later slices (WAV/Opus/WavPack) append
// a row here + an IEncoder and touch no modal logic.
//
// `lossless` drives the * master marker (the AR/CTDB-verifiable master vs
// derived lossy copies) and the CUE/M3U8 target choice — a property, never a
// row index, so appended rows can't skew it.
#pragma once

#include <vector>

enum class RipFormat { Flac, Mp3, Wav, Opus, WavPack };

struct RipFormatRow {
    RipFormat   id;
    const char* label;      // modal row + rip_formats config token (case-insensitive)
    const char* ext;        // output extension, dot included
    bool        lossless;
    bool        taggable;   // false -> the tag/R128 pass skips this output
    const char* note;       // row annotation shown after the marker ("" = none)
};

// APPEND-ONLY: the digit key is the row index + 1 and must stay stable once
// shipped (WAV is 3 forever; Opus takes 4, WavPack 5). Do not reorder.
inline constexpr RipFormatRow kRipFormats[] = {
    { RipFormat::Flac, "FLAC", ".flac", true,  true,  ""           },
    { RipFormat::Mp3,  "MP3",  ".mp3",  false, true,  ""           },
    { RipFormat::Wav,  "WAV",  ".wav",  true,  false, "(untagged)" },
    { RipFormat::Opus, "Opus", ".opus", false, true,  ""           },
    { RipFormat::WavPack, "WavPack", ".wv", true, true, ""         },
};
inline constexpr int kRipFormatCount = (int)(sizeof(kRipFormats) / sizeof(kRipFormats[0]));

inline const RipFormatRow* ripFormatRow(RipFormat f) {
    for (const auto& r : kRipFormats) if (r.id == f) return &r;
    return nullptr;
}

// The rip request the UI hands to CDRipper::start — copied into the worker
// like every other start argument (no mutable cross-thread state). Defaults
// are EXACTLY the pre-slice-2 behavior: both formats, FLAC level 5, LAME V0
// (the seam oracle pins the encoder-side defaults against the frozen inline
// reference; these must stay in lockstep with those ctor defaults).
struct RipOptions {
    std::vector<RipFormat> formats { RipFormat::Flac, RipFormat::Mp3 };
    int flac_level   = 5;        // FLAC compression 0-8
    int mp3_vbr_q    = 0;        // LAME VBR quality 0-9 (0 = V0)
    bool mp3_cbr     = false;    // MP3 mode: false = VBR (V-scale), true = CBR (bitrate)
    int mp3_cbr_bitrate = 256000; // MP3 CBR bitrate (bps) when mp3_cbr
    int opus_bitrate = 128000;   // = kOpusDefaultBitrate; clamped 6k-510k at parse
    bool opus_vbr    = true;     // Opus mode: true = VBR (default), false = CBR
    int wavpack_mode = 1;        // = kWavPackDefaultMode; 0 fast / 1 normal / 2 high / 3 very high
};
