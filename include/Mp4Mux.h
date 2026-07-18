// Mp4Mux.h — minimal hand-rolled MP4/ISO-BMFF writer for AAC-LC access units.
//
// Lifted VERBATIM from the aac-m4a-phase0-probe (probes/aac-m4a/Mp4Mux.h), where
// it was proven end to end (exact length, valid sample table with 25/50/75% seek,
// covr+tags round-trip, opened clean by RE-MOCT's own AacDecoder, FFmpeg, and real
// Apple/WMP players). M4aEncoder is its one production caller. No dependency, no
// DLL - pure byte assembly mirroring the box READER in AacDecoder.cpp / Mp4Chapters.cpp.
//
// It assembles FDK's raw AAC access units (TT_MP4_RAW) into a valid, seekable,
// single-audio-track .m4a, writing exactly and only the boxes RE-MOCT's own
// decoder reads (src/AacDecoder.cpp Mp4Aac::parse):
//
//   ftyp
//   moov
//     mvhd
//     trak
//       tkhd
//       mdia
//         mdhd          (timescale = sampleRate, duration = nAU * frameLength)
//         hdlr          ('soun')
//         minf
//           smhd
//           dinf/dref   (one self-contained 'url ' entry)
//           stbl
//             stsd -> mp4a(28-byte AudioSampleEntry) -> esds(ASC)
//             stts        (1 entry: nAU samples, frameLength delta)
//             stsc        (1 entry: chunk 1, all samples, desc 1)
//             stsz        (per-AU sizes; non-uniform)
//             stco        (1 entry: absolute offset of mdat payload)
//   mdat
//     [access units concatenated]
//
// Single-chunk layout keeps the sample table trivially correct: one stsc run,
// one stco offset. Seeking in RE-MOCT is frameIndex/frameLength -> sample index,
// so a single uniform stts entry makes scrubbing exact.
//
// NO new dependency, NO new DLL: this is plain byte assembly, mirroring the box
// reader already in Mp4Chapters.cpp / AacDecoder.cpp.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mp4mux {

struct Track {
    std::vector<uint8_t>              asc;          // AudioSpecificConfig (from aacEncInfo confBuf)
    uint32_t                          sampleRate  = 44100;
    uint32_t                          channels    = 2;
    uint32_t                          frameLength = 1024;  // AAC-LC = 1024 samples/AU
    std::vector<std::vector<uint8_t>> aus;          // one raw AAC access unit per frame
    uint32_t                          avgBitrate  = 128000; // esds hint only
};

namespace detail {

inline void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}
inline void putStr(std::vector<uint8_t>& v, const char* s) {
    v.insert(v.end(), s, s + std::strlen(s));
}
inline void putBytes(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) {
    v.insert(v.end(), b.begin(), b.end());
}

// Wrap a body in [size(4)][type(4)] ... returning the full box.
inline std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> b;
    put32(b, (uint32_t)(8 + body.size()));
    putStr(b, type);
    putBytes(b, body);
    return b;
}

// MPEG-4 descriptor length: 4-byte 0x80-continued form (fixed width keeps sizing simple).
inline void putDescLen(std::vector<uint8_t>& v, uint32_t len) {
    v.push_back((uint8_t)(0x80 | ((len >> 21) & 0x7f)));
    v.push_back((uint8_t)(0x80 | ((len >> 14) & 0x7f)));
    v.push_back((uint8_t)(0x80 | ((len >> 7)  & 0x7f)));
    v.push_back((uint8_t)(len & 0x7f));
}

inline std::vector<uint8_t> esds(const Track& t) {
    // DecSpecificInfo (0x05) = the ASC bytes
    std::vector<uint8_t> dsi;
    dsi.push_back(0x05); putDescLen(dsi, (uint32_t)t.asc.size()); putBytes(dsi, t.asc);

    // DecoderConfigDescr (0x04): objectType(0x40 AAC) + streamType(0x15) +
    // bufferSizeDB(3) + maxBitrate(4) + avgBitrate(4) = 13 bytes, then DSI.
    std::vector<uint8_t> dcd;
    dcd.push_back(0x40);                 // Audio ISO/IEC 14496-3 (AAC)
    dcd.push_back(0x15);                 // streamType=AudioStream, upStream=0
    dcd.push_back(0x00); dcd.push_back(0x00); dcd.push_back(0x00);  // bufferSizeDB
    put32(dcd, t.avgBitrate ? t.avgBitrate : 128000);              // maxBitrate
    put32(dcd, t.avgBitrate ? t.avgBitrate : 128000);              // avgBitrate
    putBytes(dcd, dsi);
    std::vector<uint8_t> dcdWrapped;
    dcdWrapped.push_back(0x04); putDescLen(dcdWrapped, (uint32_t)dcd.size()); putBytes(dcdWrapped, dcd);

    // SLConfigDescr (0x06): predefined = 0x02 (MP4). Decoder ignores it; players want it.
    std::vector<uint8_t> sl;
    sl.push_back(0x06); putDescLen(sl, 1); sl.push_back(0x02);

    // ES_Descr (0x03): ES_ID(2)=0, flags(1)=0, then DCD + SL.
    std::vector<uint8_t> es;
    es.push_back(0x03);
    std::vector<uint8_t> esBody;
    put16(esBody, 0x0000); esBody.push_back(0x00);
    putBytes(esBody, dcdWrapped); putBytes(esBody, sl);
    putDescLen(es, (uint32_t)esBody.size()); putBytes(es, esBody);

    std::vector<uint8_t> body;
    put32(body, 0);                      // FullBox version + flags
    putBytes(body, es);
    return box("esds", body);
}

inline std::vector<uint8_t> mp4a(const Track& t) {
    std::vector<uint8_t> body;
    for (int i = 0; i < 6; ++i) body.push_back(0);   // reserved[6]
    put16(body, 1);                                  // data_reference_index
    put32(body, 0); put32(body, 0);                  // reserved (8)
    put16(body, (uint16_t)t.channels);               // channelcount
    put16(body, 16);                                 // samplesize
    put16(body, 0);                                  // pre_defined
    put16(body, 0);                                  // reserved
    put32(body, t.sampleRate << 16);                 // samplerate 16.16
    putBytes(body, esds(t));                         // child (decoder finds it at body+28)
    return box("mp4a", body);
}

inline std::vector<uint8_t> stsd(const Track& t) {
    std::vector<uint8_t> body;
    put32(body, 0);                                  // version + flags
    put32(body, 1);                                  // entry_count
    putBytes(body, mp4a(t));
    return box("stsd", body);
}
inline std::vector<uint8_t> stts(const Track& t) {
    std::vector<uint8_t> body;
    put32(body, 0);
    put32(body, 1);                                  // one entry
    put32(body, (uint32_t)t.aus.size());             // sample_count
    put32(body, t.frameLength);                      // sample_delta
    return box("stts", body);
}
inline std::vector<uint8_t> stsc(const Track& t) {
    std::vector<uint8_t> body;
    put32(body, 0);
    put32(body, 1);                                  // one entry
    put32(body, 1);                                  // first_chunk
    put32(body, (uint32_t)t.aus.size());             // samples_per_chunk (all in chunk 1)
    put32(body, 1);                                  // sample_description_index
    return box("stsc", body);
}
inline std::vector<uint8_t> stsz(const Track& t) {
    std::vector<uint8_t> body;
    put32(body, 0);
    put32(body, 0);                                  // sample_size 0 -> table follows
    put32(body, (uint32_t)t.aus.size());
    for (auto& au : t.aus) put32(body, (uint32_t)au.size());
    return box("stsz", body);
}
inline std::vector<uint8_t> stco_placeholder() {
    std::vector<uint8_t> body;
    put32(body, 0);
    put32(body, 1);                                  // one chunk
    put32(body, 0);                                  // offset patched after layout
    return box("stco", body);
}
inline std::vector<uint8_t> stbl(const Track& t) {
    std::vector<uint8_t> body;
    putBytes(body, stsd(t)); putBytes(body, stts(t));
    putBytes(body, stsc(t)); putBytes(body, stsz(t));
    putBytes(body, stco_placeholder());
    return box("stbl", body);
}
inline std::vector<uint8_t> dinf() {
    std::vector<uint8_t> dref;
    put32(dref, 0); put32(dref, 1);                  // version/flags, entry_count
    std::vector<uint8_t> url;
    put32(url, 0x00000001);                          // 'url ' FullBox: flags=1 (self-contained)
    putBytes(dref, box("url ", url));
    return box("dinf", box("dref", dref));
}
inline std::vector<uint8_t> smhd() {
    std::vector<uint8_t> body; put32(body, 0); put16(body, 0); put16(body, 0);
    return box("smhd", body);
}
inline std::vector<uint8_t> minf(const Track& t) {
    std::vector<uint8_t> body;
    putBytes(body, smhd()); putBytes(body, dinf()); putBytes(body, stbl(t));
    return box("minf", body);
}
inline std::vector<uint8_t> hdlr() {
    std::vector<uint8_t> body;
    put32(body, 0); put32(body, 0);                  // version/flags, pre_defined
    putStr(body, "soun");                            // handler_type
    put32(body, 0); put32(body, 0); put32(body, 0);  // reserved[3]
    putStr(body, "SoundHandler"); body.push_back(0); // name (null-terminated)
    return box("hdlr", body);
}
inline std::vector<uint8_t> mdhd(const Track& t) {
    std::vector<uint8_t> body;
    put32(body, 0);                                  // version 0 + flags
    put32(body, 0); put32(body, 0);                  // creation, modification
    put32(body, t.sampleRate);                       // timescale
    put32(body, (uint32_t)t.aus.size() * t.frameLength); // duration
    put16(body, 0x55c4);                             // language 'und'
    put16(body, 0);                                  // pre_defined
    return box("mdhd", body);
}
inline std::vector<uint8_t> mdia(const Track& t) {
    std::vector<uint8_t> body;
    putBytes(body, mdhd(t)); putBytes(body, hdlr()); putBytes(body, minf(t));
    return box("mdia", body);
}
inline std::vector<uint8_t> tkhd(const Track& t) {
    std::vector<uint8_t> body;
    put32(body, 0x00000007);                         // version 0, flags = enabled|inMovie|inPreview
    put32(body, 0); put32(body, 0);                  // creation, modification
    put32(body, 1);                                  // track_ID
    put32(body, 0);                                  // reserved
    put32(body, (uint32_t)t.aus.size() * t.frameLength); // duration (movie timescale = sampleRate below)
    put32(body, 0); put32(body, 0);                  // reserved[2]
    put16(body, 0); put16(body, 0);                  // layer, alternate_group
    put16(body, 0x0100);                             // volume 1.0
    put16(body, 0);                                  // reserved
    // 3x3 unity matrix
    uint32_t m[9] = {0x10000,0,0, 0,0x10000,0, 0,0,0x40000000};
    for (uint32_t x : m) put32(body, x);
    put32(body, 0); put32(body, 0);                  // width, height (audio = 0)
    return box("tkhd", body);
}
inline std::vector<uint8_t> mvhd(const Track& t) {
    std::vector<uint8_t> body;
    put32(body, 0);                                  // version 0 + flags
    put32(body, 0); put32(body, 0);                  // creation, modification
    put32(body, t.sampleRate);                       // timescale
    put32(body, (uint32_t)t.aus.size() * t.frameLength); // duration
    put32(body, 0x00010000);                         // rate 1.0
    put16(body, 0x0100);                             // volume 1.0
    put16(body, 0); put32(body, 0); put32(body, 0);  // reserved
    uint32_t m[9] = {0x10000,0,0, 0,0x10000,0, 0,0,0x40000000};
    for (uint32_t x : m) put32(body, x);
    for (int i = 0; i < 6; ++i) put32(body, 0);      // pre_defined[6]
    put32(body, 2);                                  // next_track_ID
    return box("mvhd", body);
}
inline std::vector<uint8_t> ftyp() {
    std::vector<uint8_t> body;
    putStr(body, "M4A "); put32(body, 0x00000200);   // major brand, minor version
    putStr(body, "M4A "); putStr(body, "mp42");
    putStr(body, "isom"); putStr(body, "\0\0\0\0");
    return box("ftyp", body);
}

} // namespace detail

// Assemble a complete single-track AAC .m4a. Empty return = invalid input.
inline std::vector<uint8_t> muxM4a(const Track& t) {
    using namespace detail;
    if (t.asc.empty() || t.aus.empty()) return {};

    std::vector<uint8_t> moovBody;
    putBytes(moovBody, mvhd(t));
    std::vector<uint8_t> trakBody;
    putBytes(trakBody, tkhd(t)); putBytes(trakBody, mdia(t));
    putBytes(moovBody, box("trak", trakBody));
    std::vector<uint8_t> moov = box("moov", moovBody);

    std::vector<uint8_t> ft = ftyp();

    // Patch the single stco chunk offset to point at the mdat payload.
    // Only one "stco" exists; its offset field sits 12 bytes past the type tag
    // ([type 'stco'][v/f 4][count 4][offset 4]).
    size_t stcoTag = 0;
    for (size_t i = 0; i + 4 <= moov.size(); ++i) {
        if (std::memcmp(&moov[i], "stco", 4) == 0) { stcoTag = i; break; }
    }
    size_t offField = stcoTag + 4 + 4 + 4;           // within moov
    uint32_t mdatPayload = (uint32_t)(ft.size() + moov.size() + 8);
    moov[offField + 0] = (uint8_t)(mdatPayload >> 24);
    moov[offField + 1] = (uint8_t)(mdatPayload >> 16);
    moov[offField + 2] = (uint8_t)(mdatPayload >> 8);
    moov[offField + 3] = (uint8_t)(mdatPayload);

    std::vector<uint8_t> out;
    putBytes(out, ft);
    putBytes(out, moov);
    // mdat
    std::vector<uint8_t> mdatBody;
    for (auto& au : t.aus) putBytes(mdatBody, au);
    putBytes(out, box("mdat", mdatBody));
    return out;
}

} // namespace mp4mux
