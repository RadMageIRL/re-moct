// Mp4Chapters.cpp — see header. Reads chapters from MP4/M4A/M4B in two forms:
//   1. Nero 'chpl'            : flat list at moov/udta/chpl (start in 100ns units)
//   2. QuickTime text track   : audio trak -> tref/chap -> a 'text' trak whose
//                               samples ARE the titles, timed by its stts against
//                               the text track's mdhd timescale, located via
//                               stsc/stco/stsz, bytes read from mdat.
// chpl is tried first; if absent, the QuickTime path is used.
//
// Streams the top-level boxes and loads only 'moov' into memory; for the
// QuickTime path it then reads the (small, scattered) title samples directly
// from the file by offset, so it never loads 'mdat'.

#include "Mp4Chapters.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#  define FSEEK64 _fseeki64
#  define FTELL64 _ftelli64
using off64 = __int64;
#else
#  include <sys/types.h>
#  define FSEEK64 fseeko
#  define FTELL64 ftello
using off64 = off_t;
#endif

namespace {

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint64_t be64(const uint8_t* p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

FILE* openRb(const std::string& utf8Path) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring w((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, &w[0], wlen);
    return _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(utf8Path.c_str(), "rb");
#endif
}

// Find a child box within an in-memory parent range [p,end).
bool findBox(const uint8_t* p, const uint8_t* end, const char* type,
             const uint8_t** outBeg, const uint8_t** outEnd) {
    while (p + 8 <= end) {
        uint64_t sz = be32(p);
        const uint8_t* hdr  = p;
        const uint8_t* body = p + 8;
        if (sz == 1) { if (p + 16 > end) break; sz = be64(p + 8); body = p + 16; }
        else if (sz == 0) sz = (uint64_t)(end - p);
        if (sz < (uint64_t)(body - hdr) || sz > (uint64_t)(end - hdr)) break;  // size-compare avoids hdr+sz pointer overflow on crafted 64-bit boxes
        if (std::memcmp(hdr + 4, type, 4) == 0) { *outBeg = body; *outEnd = hdr + sz; return true; }
        p = hdr + sz;
    }
    return false;
}

// ── Nero chpl ────────────────────────────────────────────────────────────────
std::vector<Mp4Chapter> parseChpl(const uint8_t* p, const uint8_t* end) {
    std::vector<Mp4Chapter> out;
    if (p + 5 > end) return out;
    uint8_t version = p[0];
    p += 4;                                       // version(1) + flags(3)
    if (version != 0) { if (p + 4 > end) return out; p += 4; }   // reserved dword
    if (p >= end) return out;
    uint32_t count = *p++;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 9 > end) break;                   // start(8) + len(1)
        uint64_t raw = be64(p); p += 8;
        uint8_t  tlen = *p++;
        if (p + tlen > end) break;
        out.push_back({ (double)raw / 1.0e7, std::string((const char*)p, (const char*)p + tlen) });
        p += tlen;
    }
    return out;
}

// ── QuickTime text-track chapters ─────────────────────────────────────────────
uint32_t trakId(const uint8_t* tb, const uint8_t* te) {
    const uint8_t *kb, *ke;
    if (findBox(tb, te, "tkhd", &kb, &ke) && kb + 4 <= ke) {
        uint8_t ver = kb[0];
        const uint8_t* q = (ver == 1) ? kb + 20 : kb + 12;
        if (q + 4 <= ke) return be32(q);
    }
    return 0;
}

uint32_t mdiaTimescale(const uint8_t* mb, const uint8_t* me) {
    const uint8_t *hb, *he;
    if (findBox(mb, me, "mdhd", &hb, &he) && hb + 4 <= he) {
        uint8_t ver = hb[0];
        if (ver == 1) { if (hb + 24 <= he) return be32(hb + 20); }
        else          { if (hb + 16 <= he) return be32(hb + 12); }
    }
    return 0;
}

// Audio trak references the chapter trak via tref/chap; return that track id.
uint32_t findChapTrackId(const uint8_t* moovB, const uint8_t* moovE) {
    const uint8_t* p = moovB;
    const uint8_t *tb, *te;
    while (findBox(p, moovE, "trak", &tb, &te)) {
        const uint8_t *rb, *re;
        if (findBox(tb, te, "tref", &rb, &re)) {
            const uint8_t *cb, *ce;
            if (findBox(rb, re, "chap", &cb, &ce) && cb + 4 <= ce) return be32(cb);
        }
        p = te;
    }
    return 0;
}

struct QtStbl {
    std::vector<uint64_t> offset;   // per sample, absolute file offset
    std::vector<uint32_t> size;     // per sample byte size
    std::vector<uint64_t> start;    // per sample start, in timescale ticks
    bool ok = false;
};

bool buildQtStbl(const uint8_t* b, const uint8_t* e, QtStbl& T) {
    const uint8_t *sb, *se;

    std::vector<uint32_t> sizes;
    if (findBox(b, e, "stsz", &sb, &se) && sb + 12 <= se) {
        uint32_t uniform = be32(sb + 4), count = be32(sb + 8);
        if (uniform != 0) sizes.assign(count, uniform);
        else { const uint8_t* q = sb + 12; for (uint32_t i = 0; i < count && q + 4 <= se; ++i, q += 4) sizes.push_back(be32(q)); }
    }

    struct Stsc { uint32_t first, per; };
    std::vector<Stsc> stsc;
    if (findBox(b, e, "stsc", &sb, &se) && sb + 8 <= se) {
        uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8;
        for (uint32_t i = 0; i < n && q + 12 <= se; ++i, q += 12) stsc.push_back({ be32(q), be32(q + 4) });
    }

    std::vector<uint64_t> chunks;
    if (findBox(b, e, "stco", &sb, &se) && sb + 8 <= se) {
        uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8;
        for (uint32_t i = 0; i < n && q + 4 <= se; ++i, q += 4) chunks.push_back(be32(q));
    } else if (findBox(b, e, "co64", &sb, &se) && sb + 8 <= se) {
        uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8;
        for (uint32_t i = 0; i < n && q + 8 <= se; ++i, q += 8) chunks.push_back(be64(q));
    }

    std::vector<uint64_t> starts;
    if (findBox(b, e, "stts", &sb, &se) && sb + 8 <= se) {
        uint32_t n = be32(sb + 4); const uint8_t* q = sb + 8; uint64_t t = 0;
        // Cap expansion at the sample count: a crafted stts run-count could
        // otherwise balloon `starts` to billions of entries before it is
        // truncated to ns below. We never need more starts than samples.
        const size_t cap = sizes.size();
        for (uint32_t i = 0; i < n && q + 8 <= se && starts.size() < cap; ++i, q += 8) {
            uint32_t cnt = be32(q), delta = be32(q + 4);
            for (uint32_t k = 0; k < cnt && starts.size() < cap; ++k) { starts.push_back(t); t += delta; }
        }
    }

    if (sizes.empty() || stsc.empty() || chunks.empty()) return false;

    uint32_t sIdx = 0;
    // stsc first_chunk is strictly increasing and c increases monotonically, so the
    // matching run only ever moves forward — a single advancing cursor makes this
    // O(chunks + stsc) instead of O(chunks * stsc). Mirrors the AacDecoder fix; guards
    // against a text-track muxed with ~one stsc entry per chunk freezing chapter parse.
    size_t run = 0;
    for (uint32_t c = 0; c < chunks.size(); ++c) {
        while (run + 1 < stsc.size() && stsc[run + 1].first <= (c + 1)) ++run;
        uint32_t per = stsc[run].per;
        uint64_t off = chunks[c];
        for (uint32_t s = 0; s < per && sIdx < sizes.size(); ++s, ++sIdx) {
            T.offset.push_back(off);
            T.size.push_back(sizes[sIdx]);
            off += sizes[sIdx];
        }
    }

    size_t ns = T.offset.size();
    if (starts.size() < ns) starts.resize(ns, starts.empty() ? 0 : starts.back());
    T.start.assign(starts.begin(), starts.begin() + ns);
    T.ok = !T.offset.empty();
    return T.ok;
}

std::string utf16ToUtf8(const uint8_t* p, uint32_t n, bool bigEndian) {
    std::string s;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = bigEndian ? (((uint32_t)p[i] << 8) | p[i + 1])
                               : (((uint32_t)p[i + 1] << 8) | p[i]);
        if (u < 0x80) s += (char)u;
        else if (u < 0x800) { s += (char)(0xC0 | (u >> 6)); s += (char)(0x80 | (u & 0x3F)); }
        else { s += (char)(0xE0 | (u >> 12)); s += (char)(0x80 | ((u >> 6) & 0x3F)); s += (char)(0x80 | (u & 0x3F)); }
    }
    return s;
}

// QuickTime text sample: u16 length prefix, then that many text bytes, then
// optional style/encoding atoms we ignore.
std::string decodeTextSample(const uint8_t* b, uint32_t n) {
    if (n < 2) return std::string((const char*)b, (const char*)b + n);
    uint32_t len = ((uint32_t)b[0] << 8) | b[1];
    const uint8_t* t = b + 2;
    uint32_t avail = n - 2;
    if (len > avail) len = avail;
    if (len >= 2 && t[0] == 0xFE && t[1] == 0xFF) return utf16ToUtf8(t + 2, len - 2, true);
    if (len >= 2 && t[0] == 0xFF && t[1] == 0xFE) return utf16ToUtf8(t + 2, len - 2, false);
    while (len > 0 && t[len - 1] == 0) --len;        // strip trailing NULs
    return std::string((const char*)t, (const char*)t + len);
}

bool parseQtChapters(const uint8_t* moovB, const uint8_t* moovE, FILE* f,
                     std::vector<Mp4Chapter>& out) {
    uint32_t cid = findChapTrackId(moovB, moovE);

    // Locate the chapter track: prefer the tref/chap id, else first 'text' trak.
    const uint8_t *mdB = nullptr, *mdE = nullptr;
    const uint8_t* p = moovB;
    const uint8_t *tb, *te;
    while (findBox(p, moovE, "trak", &tb, &te)) {
        const uint8_t *mb, *me;
        if (findBox(tb, te, "mdia", &mb, &me)) {
            uint32_t id = trakId(tb, te);
            bool isText = false;
            const uint8_t *hb, *he;
            if (findBox(mb, me, "hdlr", &hb, &he) && hb + 12 <= he)
                isText = (std::memcmp(hb + 8, "text", 4) == 0);
            if ((cid && id == cid) || (!cid && isText)) { mdB = mb; mdE = me; break; }
        }
        p = te;
    }
    if (!mdB) return false;

    uint32_t ts = mdiaTimescale(mdB, mdE);
    if (ts == 0) ts = 1000;

    const uint8_t *nb, *ne;
    if (!findBox(mdB, mdE, "minf", &nb, &ne)) return false;
    const uint8_t *xb, *xe;
    if (!findBox(nb, ne, "stbl", &xb, &xe)) return false;

    QtStbl T;
    if (!buildQtStbl(xb, xe, T)) return false;

    for (size_t i = 0; i < T.offset.size(); ++i) {
        if (T.size[i] < 2 || T.size[i] > 4096) continue;       // title sanity
        std::vector<uint8_t> buf(T.size[i]);
        if (FSEEK64(f, (off64)T.offset[i], SEEK_SET) != 0) continue;
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) continue;
        std::string title = decodeTextSample(buf.data(), (uint32_t)buf.size());
        out.push_back({ (double)T.start[i] / (double)ts, std::move(title) });
    }
    return !out.empty();
}

// Walk an esds descriptor chain (ES_Descr 0x03 → DecoderConfig 0x04 →
// DecoderSpecificInfo 0x05) to the AudioSpecificConfig and return its 4-bit
// channelConfiguration. [p,e) is the esds body *after* its 4-byte version/flags.
// Returns 0 on any malformed/short input.
int ascChannelConfig(const uint8_t* p, const uint8_t* e) {
    auto readLen = [&](const uint8_t*& q) -> uint32_t {   // expandable size (<=4 bytes)
        uint32_t v = 0;
        for (int i = 0; i < 4 && q < e; ++i) {
            uint8_t c = *q++;
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) break;
        }
        return v;
    };
    // ES_Descriptor
    if (p >= e || *p++ != 0x03) return 0;
    readLen(p);
    if (p + 3 > e) return 0;
    p += 2;                              // ES_ID
    uint8_t esFlags = *p++;
    if (esFlags & 0x80) { if (p + 2 > e) return 0; p += 2; }                                 // dependsOn_ES_ID
    if (esFlags & 0x40) { if (p >= e) return 0; uint8_t ul = *p++; if (p + ul > e) return 0; p += ul; } // URL
    if (esFlags & 0x20) { if (p + 2 > e) return 0; p += 2; }                                 // OCR_ES_ID
    // DecoderConfigDescriptor
    if (p >= e || *p++ != 0x04) return 0;
    readLen(p);
    if (p + 13 > e) return 0;
    p += 13;                             // objType + streamType/bufferSizeDB(3) + max/avgBitrate(8)
    // DecoderSpecificInfo == AudioSpecificConfig
    if (p >= e || *p++ != 0x05) return 0;
    uint32_t ascLen = readLen(p);
    const uint8_t* ae = (ascLen && p + ascLen <= e) ? p + ascLen : e;
    if (p >= ae) return 0;

    // MSB-first bit reader over the ASC.
    const uint8_t* bp = p; int bit = 0;
    auto getBits = [&](int n) -> uint32_t {
        uint32_t v = 0;
        while (n-- > 0) {
            uint32_t b = (bp < ae) ? ((*bp >> (7 - bit)) & 1u) : 0u;
            v = (v << 1) | b;
            if (bp < ae && ++bit == 8) { bit = 0; ++bp; }
        }
        return v;
    };
    uint32_t aot = getBits(5);
    if (aot == 31) aot = 32 + getBits(6);          // AOT escape
    uint32_t sfi = getBits(4);
    if (sfi == 0xF) getBits(24);                    // explicit sampling frequency
    return (int)getBits(4);                          // channelConfiguration
}

} // namespace

std::vector<Mp4Chapter> parseMp4Chapters(const std::string& utf8Path) {
    std::vector<Mp4Chapter> out;

    FILE* f = openRb(utf8Path);
    if (!f) return out;

    // Streamed top-level scan: find 'moov' wherever it lives, load only it.
    std::vector<uint8_t> moov;
    for (;;) {
        off64 boxStart = FTELL64(f);
        uint8_t hdr[16];
        if (std::fread(hdr, 1, 8, f) != 8) break;
        uint64_t sz = be32(hdr);
        uint32_t hdrLen = 8;
        if (sz == 1) {
            if (std::fread(hdr + 8, 1, 8, f) != 8) break;
            sz = be64(hdr + 8);
            hdrLen = 16;
        } else if (sz == 0) {
            if (FSEEK64(f, 0, SEEK_END) != 0) break;
            off64 fend = FTELL64(f);
            if (fend <= boxStart) break;
            sz = (uint64_t)(fend - boxStart);
            if (FSEEK64(f, boxStart + hdrLen, SEEK_SET) != 0) break;
        }
        if (sz < hdrLen) break;

        if (std::memcmp(hdr + 4, "moov", 4) == 0) {
            uint64_t bodyLen = sz - hdrLen;
            if (bodyLen == 0 || bodyLen > (uint64_t)128 * 1024 * 1024) break;
            moov.resize((size_t)bodyLen);
            if (std::fread(moov.data(), 1, (size_t)bodyLen, f) != bodyLen) moov.clear();
            break;
        }
        if (FSEEK64(f, boxStart + (off64)sz, SEEK_SET) != 0) break;   // skip (incl. mdat)
    }

    if (moov.empty()) { std::fclose(f); return out; }

    const uint8_t* mb = moov.data();
    const uint8_t* me = mb + moov.size();

    // 1) Nero chpl first.
    const uint8_t *ub, *ue;
    if (findBox(mb, me, "udta", &ub, &ue)) {
        const uint8_t *cb, *ce;
        if (findBox(ub, ue, "chpl", &cb, &ce)) out = parseChpl(cb, ce);
    }

    // 2) Else QuickTime text-track chapters (needs file reads for the titles).
    if (out.empty()) parseQtChapters(mb, me, f, out);

    std::fclose(f);

    std::sort(out.begin(), out.end(),
              [](const Mp4Chapter& a, const Mp4Chapter& b) { return a.start_sec < b.start_sec; });
    return out;
}

int mp4AacChannelCount(const std::string& utf8Path) {
    FILE* f = openRb(utf8Path);
    if (!f) return 0;

    // Stream top-level boxes; load only 'moov' (never reads mdat). Mirrors the
    // parseMp4Chapters scan so this stays cheap even on a 300+ MB book.
    std::vector<uint8_t> moov;
    for (;;) {
        off64 boxStart = FTELL64(f);
        uint8_t hdr[16];
        if (std::fread(hdr, 1, 8, f) != 8) break;
        uint64_t sz = be32(hdr);
        uint32_t hdrLen = 8;
        if (sz == 1) {
            if (std::fread(hdr + 8, 1, 8, f) != 8) break;
            sz = be64(hdr + 8);
            hdrLen = 16;
        } else if (sz == 0) {
            if (FSEEK64(f, 0, SEEK_END) != 0) break;
            off64 fend = FTELL64(f);
            if (fend <= boxStart) break;
            sz = (uint64_t)(fend - boxStart);
            if (FSEEK64(f, boxStart + hdrLen, SEEK_SET) != 0) break;
        }
        if (sz < hdrLen) break;
        if (std::memcmp(hdr + 4, "moov", 4) == 0) {
            uint64_t bodyLen = sz - hdrLen;
            if (bodyLen == 0 || bodyLen > (uint64_t)128 * 1024 * 1024) break;
            moov.resize((size_t)bodyLen);
            if (std::fread(moov.data(), 1, (size_t)bodyLen, f) != bodyLen) moov.clear();
            break;
        }
        if (FSEEK64(f, boxStart + (off64)sz, SEEK_SET) != 0) break;
    }
    std::fclose(f);
    if (moov.empty()) return 0;

    const uint8_t* mb = moov.data();
    const uint8_t* me = mb + moov.size();

    // Find the first trak whose stsd sample entry is 'mp4a' (skips PNG cover /
    // chapter-text traks), descend to its esds, and read the ASC channel config.
    const uint8_t* p = mb;
    const uint8_t *tb, *te;
    while (findBox(p, me, "trak", &tb, &te)) {
        const uint8_t *mdB, *mdE, *nb, *ne, *xb, *xe, *db, *de;
        if (findBox(tb, te, "mdia", &mdB, &mdE) &&
            findBox(mdB, mdE, "minf", &nb, &ne) &&
            findBox(nb, ne, "stbl", &xb, &xe) &&
            findBox(xb, xe, "stsd", &db, &de) && db + 8 <= de) {
            const uint8_t* entry = db + 8;                 // skip version/flags + entry_count
            if (entry + 8 <= de) {
                uint32_t entrySz = be32(entry);
                const uint8_t* entryEnd = entry + entrySz;
                if (entryEnd > de) entryEnd = de;
                if (std::memcmp(entry + 4, "mp4a", 4) == 0 && entry + 36 <= entryEnd) {
                    const uint8_t *eb, *ee;            // esds is a child of the AudioSampleEntry (36-byte fixed part)
                    if (findBox(entry + 36, entryEnd, "esds", &eb, &ee) && eb + 4 <= ee) {
                        int cc = ascChannelConfig(eb + 4, ee);
                        if (cc >= 1 && cc <= 6) return cc;
                        if (cc == 7)            return 8;
                        return 0;                  // in-band (0) or reserved → caller falls back
                    }
                }
            }
        }
        p = te;
    }
    return 0;
}
