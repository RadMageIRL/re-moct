// MuxWriter.cpp — copy-path cut writers (abi-cluster slice B). See the header.
//
// The M4A layout is the minimal ISO-BMFF shape the in-tree reader
// (AacDecoder.cpp's Mp4Aac) parses: moov -> trak -> mdia -> { mdhd, minf ->
// stbl -> { stsd -> mp4a(+28) -> esds, stts, stsc, stsz, stco } }, one track,
// one chunk (stco[0] = mdat payload start, stsc = "all samples in chunk 1").
// Timescale = the broadcast sample rate; every duration is exact frame math
// (1024 * frame count), so the round-trip sample count is deterministic.
#include "MuxWriter.h"

#include "StringUtils.h"   // utf8_to_wide — Windows paths, same as the encoders

#include <cstring>

namespace {

#ifdef _WIN32
std::FILE* openWide(const std::string& path, const wchar_t* mode) {
    return _wfopen(utf8_to_wide(path).c_str(), mode);
}
#endif

std::FILE* fopenPath(const std::string& path) {
#ifdef _WIN32
    return openWide(path, L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

// Big-endian appenders for the in-memory moov build.
void be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}
void tag4(std::vector<uint8_t>& v, const char* t) {
    v.insert(v.end(), t, t + 4);
}
// Open a box, returning the size-field offset to patch when it closes.
size_t boxOpen(std::vector<uint8_t>& v, const char* type) {
    size_t at = v.size();
    be32(v, 0); tag4(v, type);
    return at;
}
void boxClose(std::vector<uint8_t>& v, size_t at) {
    uint32_t sz = (uint32_t)(v.size() - at);
    v[at]   = (uint8_t)(sz >> 24); v[at+1] = (uint8_t)(sz >> 16);
    v[at+2] = (uint8_t)(sz >> 8);  v[at+3] = (uint8_t)sz;
}

} // namespace

// ─── Mp3AppendWriter ─────────────────────────────────────────────────────────

Mp3AppendWriter::~Mp3AppendWriter() { if (f_) std::fclose(f_); }

bool Mp3AppendWriter::open(const std::string& path) {
    f_ = fopenPath(path);
    return f_ != nullptr;
}

bool Mp3AppendWriter::writeFrame(const uint8_t* frame, uint32_t len,
                                 const framesync::FrameInfo&) {
    if (!f_) return false;
    if (std::fwrite(frame, 1, len, f_) != len) return false;
    ++frames_written_;
    return true;
}

bool Mp3AppendWriter::finalize(bool keep) {
    if (!f_) return false;
    bool ok = true;
    if (keep) ok = (std::fflush(f_) == 0);   // the fflush-and-check discipline
    ok = (std::fclose(f_) == 0) && ok;
    f_ = nullptr;
    return keep ? (ok && frames_written_ > 0) : true;
}

// ─── M4aMuxWriter ────────────────────────────────────────────────────────────

// Fixed offsets: ftyp is written at open as 28 bytes, the mdat header
// directly after it, so the mdat size field lives at 28 and the payload
// (= stco's one chunk offset) starts at 36.
static constexpr long     kMdatHdrOff  = 28;
static constexpr uint32_t kMdatDataOff = 36;

M4aMuxWriter::~M4aMuxWriter() { if (f_) std::fclose(f_); }

bool M4aMuxWriter::open(const std::string& path) {
    f_ = fopenPath(path);
    if (!f_) return false;
    // ftyp: major M4A , minor 0, compatible M4A /mp42/isom (28 bytes), then
    // the mdat header with a placeholder size patched at finalize.
    std::vector<uint8_t> head;
    size_t ft = boxOpen(head, "ftyp");
    tag4(head, "M4A "); be32(head, 0);
    tag4(head, "M4A "); tag4(head, "mp42"); tag4(head, "isom");
    boxClose(head, ft);
    be32(head, 0); tag4(head, "mdat");       // size 0 placeholder
    if (std::fwrite(head.data(), 1, head.size(), f_) != head.size()) return false;
    return true;
}

bool M4aMuxWriter::writeFrame(const uint8_t* frame, uint32_t len,
                              const framesync::FrameInfo& fi) {
    if (!f_ || len <= fi.header_len) return false;
    if (frames_written_ == 0) {
        sample_rate_ = fi.sample_rate;
        profile_     = fi.aac_profile;
        sr_index_    = fi.aac_sr_index;
        chan_cfg_    = fi.aac_chan_cfg;
    }
    // De-ADTS: the access unit is the frame minus its transport header.
    const uint8_t* au = frame + fi.header_len;
    uint32_t n = len - fi.header_len;
    if (std::fwrite(au, 1, n, f_) != n) return false;
    sizes_.push_back(n);
    mdat_bytes_ += n;
    ++frames_written_;
    return true;
}

bool M4aMuxWriter::finalize(bool keep) {
    if (!f_) return false;
    if (!keep) { std::fclose(f_); f_ = nullptr; return true; }
    // A cut with no audio, or one mdat can't address in 32 bits, is a failed
    // cut — declared, not papered over (stco is 32-bit by construction here).
    bool ok = frames_written_ > 0 &&
              mdat_bytes_ + 8 <= 0xFFFFFFFFull && sample_rate_ > 0;

    if (ok) {
        // Patch the mdat size, then append moov.
        ok = (std::fseek(f_, kMdatHdrOff, SEEK_SET) == 0);
        if (ok) {
            uint8_t szb[4];
            uint32_t msz = (uint32_t)(mdat_bytes_ + 8);
            szb[0]=(uint8_t)(msz>>24); szb[1]=(uint8_t)(msz>>16);
            szb[2]=(uint8_t)(msz>>8);  szb[3]=(uint8_t)msz;
            ok = std::fwrite(szb, 1, 4, f_) == 4 &&
                 std::fseek(f_, 0, SEEK_END) == 0;
        }
    }
    if (ok) {
        const uint32_t n        = (uint32_t)sizes_.size();
        const uint64_t duration = 1024ull * n;          // timescale = sample rate
        const uint32_t dur32    = duration > 0xFFFFFFFFull
                                  ? 0xFFFFFFFFu : (uint32_t)duration;
        std::vector<uint8_t> m;
        m.reserve(512 + (size_t)n * 4);

        size_t moov = boxOpen(m, "moov");
        {
            size_t mvhd = boxOpen(m, "mvhd");
            be32(m, 0);                                  // version 0 + flags
            be32(m, 0); be32(m, 0);                      // creation/modification
            be32(m, sample_rate_); be32(m, dur32);       // timescale, duration
            be32(m, 0x00010000); be16(m, 0x0100);        // rate 1.0, volume 1.0
            be16(m, 0); be32(m, 0); be32(m, 0);          // reserved
            be32(m, 0x00010000); be32(m, 0); be32(m, 0); // identity matrix
            be32(m, 0); be32(m, 0x00010000); be32(m, 0);
            be32(m, 0); be32(m, 0); be32(m, 0x40000000);
            for (int i = 0; i < 6; ++i) be32(m, 0);      // pre_defined
            be32(m, 2);                                  // next_track_ID
            boxClose(m, mvhd);

            size_t trak = boxOpen(m, "trak");
            {
                size_t tkhd = boxOpen(m, "tkhd");
                be32(m, 0x00000007);                     // v0, enabled|in-movie|in-preview
                be32(m, 0); be32(m, 0);                  // creation/modification
                be32(m, 1); be32(m, 0);                  // track_ID, reserved
                be32(m, dur32);                          // duration (movie ts == media ts)
                be32(m, 0); be32(m, 0);                  // reserved
                be16(m, 0); be16(m, 0);                  // layer, alternate_group
                be16(m, 0x0100); be16(m, 0);             // volume 1.0 (audio), reserved
                be32(m, 0x00010000); be32(m, 0); be32(m, 0);
                be32(m, 0); be32(m, 0x00010000); be32(m, 0);
                be32(m, 0); be32(m, 0); be32(m, 0x40000000);
                be32(m, 0); be32(m, 0);                  // width, height (audio: 0)
                boxClose(m, tkhd);

                size_t mdia = boxOpen(m, "mdia");
                {
                    size_t mdhd = boxOpen(m, "mdhd");
                    be32(m, 0);                          // version 0 + flags
                    be32(m, 0); be32(m, 0);
                    be32(m, sample_rate_); be32(m, dur32);
                    be16(m, 0x55C4); be16(m, 0);         // language "und"
                    boxClose(m, mdhd);

                    size_t hdlr = boxOpen(m, "hdlr");
                    be32(m, 0); be32(m, 0);
                    tag4(m, "soun");
                    be32(m, 0); be32(m, 0); be32(m, 0);
                    const char* name = "SoundHandler";
                    m.insert(m.end(), name, name + std::strlen(name) + 1);
                    boxClose(m, hdlr);

                    size_t minf = boxOpen(m, "minf");
                    {
                        size_t smhd = boxOpen(m, "smhd");
                        be32(m, 0); be32(m, 0);          // balance + reserved
                        boxClose(m, smhd);

                        size_t dinf = boxOpen(m, "dinf");
                        {
                            size_t dref = boxOpen(m, "dref");
                            be32(m, 0); be32(m, 1);      // 1 entry
                            size_t url = boxOpen(m, "url ");
                            be32(m, 1);                  // flags: self-contained
                            boxClose(m, url);
                            boxClose(m, dref);
                        }
                        boxClose(m, dinf);

                        size_t stbl = boxOpen(m, "stbl");
                        {
                            size_t stsd = boxOpen(m, "stsd");
                            be32(m, 0); be32(m, 1);      // 1 entry
                            {
                                size_t mp4a = boxOpen(m, "mp4a");
                                for (int i = 0; i < 6; ++i) m.push_back(0);
                                be16(m, 1);              // data_reference_index
                                be32(m, 0); be32(m, 0);  // reserved (version..vendor)
                                be16(m, (uint16_t)(chan_cfg_ ? chan_cfg_ : 2));
                                be16(m, 16);             // samplesize
                                be16(m, 0); be16(m, 0);  // pre_defined, reserved
                                be32(m, sample_rate_ << 16);   // 16.16
                                {
                                    size_t esds = boxOpen(m, "esds");
                                    be32(m, 0);          // version + flags
                                    // AudioSpecificConfig from the ADTS header
                                    // fields (aot = profile+1).
                                    uint8_t aot = (uint8_t)(profile_ + 1);
                                    uint8_t asc0 = (uint8_t)((aot << 3) | (sr_index_ >> 1));
                                    uint8_t asc1 = (uint8_t)(((sr_index_ & 1) << 7) | (chan_cfg_ << 3));
                                    // ES_Descriptor (03) > DecoderConfig (04)
                                    // > DecSpecificInfo (05) + SLConfig (06),
                                    // single-byte lengths (tiny payloads).
                                    m.push_back(0x03); m.push_back(25);
                                    be16(m, 0); m.push_back(0);          // ES_ID, flags
                                    m.push_back(0x04); m.push_back(17);
                                    m.push_back(0x40);                   // AAC (14496-3)
                                    m.push_back(0x15);                   // audio stream
                                    m.push_back(0); be16(m, 0x1800);     // bufferSizeDB
                                    be32(m, 0); be32(m, 0);              // max/avg bitrate
                                    m.push_back(0x05); m.push_back(2);
                                    m.push_back(asc0); m.push_back(asc1);
                                    m.push_back(0x06); m.push_back(1); m.push_back(0x02);
                                    boxClose(m, esds);
                                }
                                boxClose(m, mp4a);
                            }
                            boxClose(m, stsd);

                            size_t stts = boxOpen(m, "stts");
                            be32(m, 0); be32(m, 1);
                            be32(m, n); be32(m, 1024);   // every AU is 1024 samples
                            boxClose(m, stts);

                            size_t stsc = boxOpen(m, "stsc");
                            be32(m, 0); be32(m, 1);
                            be32(m, 1); be32(m, n); be32(m, 1);   // one chunk holds all
                            boxClose(m, stsc);

                            size_t stsz = boxOpen(m, "stsz");
                            be32(m, 0); be32(m, 0); be32(m, n);
                            for (uint32_t s : sizes_) be32(m, s);
                            boxClose(m, stsz);

                            size_t stco = boxOpen(m, "stco");
                            be32(m, 0); be32(m, 1);
                            be32(m, kMdatDataOff);
                            boxClose(m, stco);
                        }
                        boxClose(m, stbl);
                    }
                    boxClose(m, minf);
                }
                boxClose(m, mdia);
            }
            boxClose(m, trak);
        }
        boxClose(m, moov);

        ok = std::fwrite(m.data(), 1, m.size(), f_) == m.size() &&
             std::fflush(f_) == 0;
    }
    ok = (std::fclose(f_) == 0) && ok;
    f_ = nullptr;
    return ok;
}
