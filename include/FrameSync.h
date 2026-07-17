// FrameSync.h — shared MP3/ADTS frame-header parsing (abi-cluster slice B).
//
// The copy path never decodes: the recorder's copy pump and both MuxWriters
// work on WHOLE encoded frames, and this header is the one home for knowing
// where a frame starts, how long it is, and how much time it carries. The
// ADTS math mirrors AacDecoder.cpp's buildAdts() walker (the in-tree
// reference); the ID3v2 skipper is the syncsafe logic from the mp3-rg-write
// scratch tool, now with a home. Header-only per the linkage norm (engines
// and tests consume it without pulling any TU).
//
// All parsers are pure and bounds-honest: they read only the header bytes
// they document needing and return false rather than guessing. Resync is the
// CALLER's loop (scan forward a byte at a time until parse succeeds) — the
// same discipline both decoders already use on a mid-stream join.
#pragma once

#include <cstddef>
#include <cstdint>

namespace framesync {

// One parsed frame header, either codec. frame_len INCLUDES the header
// itself (ADTS: the 7/9-byte header; MP3: the 4-byte header), so a caller
// advances exactly frame_len bytes to the next sync point.
struct FrameInfo {
    uint32_t frame_len   = 0;   // whole frame, header included (bytes)
    uint32_t samples     = 0;   // PCM frames this frame decodes to (per channel)
    uint32_t sample_rate = 0;   // Hz, from the header — never assumed
    uint32_t channels    = 0;   // MP3: 1/2 from the mode field; ADTS: chan cfg
    // ADTS only (zero for MP3): what the M4A muxer needs to build the
    // AudioSpecificConfig without re-deriving header fields.
    uint8_t  aac_profile   = 0; // ADTS profile field (audioObjectType - 1)
    uint8_t  aac_sr_index  = 0; // sampling_frequency_index
    uint8_t  aac_chan_cfg  = 0; // channel_configuration
    uint32_t header_len    = 0; // bytes of header inside frame_len (ADTS 7/9, MP3 4)
};

// ── ADTS ──────────────────────────────────────────────────────────────────
// Needs 7 bytes. Sync = 0xFFF with layer 00 (the exact predicate
// AacDecoder.cpp and hlsProbeTest use: b[0]==0xFF && (b[1]&0xF6)==0xF0).
inline constexpr uint32_t kAdtsSampleRates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

inline bool adtsParse(const uint8_t* p, size_t n, FrameInfo& out) {
    if (n < 7) return false;
    if (!(p[0] == 0xFF && (p[1] & 0xF6) == 0xF0)) return false;
    uint32_t len = (((uint32_t)(p[3] & 0x03)) << 11) |
                   ((uint32_t)p[4] << 3) | (p[5] >> 5);
    uint8_t  sr_index = (p[2] >> 2) & 0x0F;
    uint32_t sr = kAdtsSampleRates[sr_index];
    bool protection_absent = (p[1] & 0x01) != 0;
    uint32_t hdr = protection_absent ? 7u : 9u;   // CRC adds 2 bytes
    if (len < hdr || sr == 0) return false;
    out.frame_len   = len;
    out.samples     = 1024;                        // AAC frame length (LC/HE core)
    out.sample_rate = sr;
    out.aac_profile  = (p[2] >> 6) & 0x03;
    out.aac_sr_index = sr_index;
    out.aac_chan_cfg = (uint8_t)(((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03));
    out.channels     = out.aac_chan_cfg;
    out.header_len   = hdr;
    return true;
}

// ── MP3 (MPEG-1/2/2.5 Layer III + Layer II) ───────────────────────────────
// Needs 4 bytes. Free-format (bitrate index 0) and bad/reserved fields are
// rejected — a broadcast stream is always a fixed-table bitrate.
inline bool mp3Parse(const uint8_t* p, size_t n, FrameInfo& out) {
    if (n < 4) return false;
    if (!(p[0] == 0xFF && (p[1] & 0xE0) == 0xE0)) return false;
    uint32_t ver_bits   = (p[1] >> 3) & 0x03;   // 0=2.5, 2=MPEG2, 3=MPEG1 (1 reserved)
    uint32_t layer_bits = (p[1] >> 1) & 0x03;   // 3=Layer I, 2=Layer II, 1=Layer III
    uint32_t br_index   = (p[2] >> 4) & 0x0F;
    uint32_t sr_index   = (p[2] >> 2) & 0x03;
    uint32_t padding    = (p[2] >> 1) & 0x01;
    if (ver_bits == 1 || layer_bits == 0 || layer_bits == 3 ||
        br_index == 0 || br_index == 15 || sr_index == 3)
        return false;                            // reserved / free-format / Layer I out

    static constexpr uint32_t kSr[3][3] = {      // [ver] 0=MPEG1,1=MPEG2,2=MPEG2.5
        { 44100, 48000, 32000 },
        { 22050, 24000, 16000 },
        { 11025, 12000,  8000 },
    };
    static constexpr uint32_t kBrV1L3[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
    static constexpr uint32_t kBrV1L2[16] = { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0 };
    static constexpr uint32_t kBrV2L3[16] = { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0 };  // also L2

    bool mpeg1 = (ver_bits == 3);
    int  ver_row = mpeg1 ? 0 : (ver_bits == 2 ? 1 : 2);
    uint32_t sr = kSr[ver_row][sr_index];
    uint32_t br_kbps = mpeg1 ? (layer_bits == 1 ? kBrV1L3[br_index] : kBrV1L2[br_index])
                             : kBrV2L3[br_index];
    if (br_kbps == 0) return false;

    // samples per frame: L3 = 1152 (MPEG1) / 576 (MPEG2/2.5); L2 = 1152 always.
    uint32_t samples = (layer_bits == 1) ? (mpeg1 ? 1152u : 576u) : 1152u;
    // frame length = samples/8 * bitrate / samplerate + padding (1 byte for L2/L3)
    uint32_t len = (samples / 8u) * (br_kbps * 1000u) / sr + padding;
    if (len <= 4) return false;
    out.frame_len   = len;
    out.samples     = samples;
    out.sample_rate = sr;
    out.channels    = (((p[3] >> 6) & 0x03) == 3) ? 1u : 2u;   // mode 3 = mono
    out.header_len  = 4;
    return true;
}

// ── ID3v2 block skipper ───────────────────────────────────────────────────
// Returns the whole-tag byte count (10-byte header + syncsafe size + the
// 10-byte footer when flagged) if p starts an ID3v2 tag, else 0. Callers
// skip this many bytes before frame-syncing — an in-stream metadata tag must
// never reach a MuxWriter (transport metadata is not broadcast audio).
inline uint32_t id3v2Size(const uint8_t* p, size_t n) {
    if (n < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;
    if ((p[6] | p[7] | p[8] | p[9]) & 0x80) return 0;   // not syncsafe -> not a tag
    uint32_t sz = ((uint32_t)(p[6] & 0x7f) << 21) | ((uint32_t)(p[7] & 0x7f) << 14) |
                  ((uint32_t)(p[8] & 0x7f) <<  7) |  (uint32_t)(p[9] & 0x7f);
    uint32_t total = 10 + sz;
    if (p[5] & 0x10) total += 10;                       // footer present
    return total;
}

} // namespace framesync
