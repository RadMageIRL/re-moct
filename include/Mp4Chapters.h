// Mp4Chapters.h — Nero 'chpl' chapter table reader for MP4/M4A/M4B.
//
// Audiobooks (AAX→m4b) carry a flat Nero chapter list at moov/udta/chpl:
// a per-chapter (start_time, title). TagLib's MP4 chapter handling is not
// first-class, so we parse the box ourselves — reusing the same ISO-BMFF
// box-walk shape as AacDecoder, but as a standalone navigation concern that
// never touches the decode path.
//
// Note on scale: chpl timestamps are fixed 100-nanosecond units
// (10,000,000 per second), NOT the mdhd timescale. start_sec = raw / 1e7.
//
// The reader streams top-level boxes and loads only 'moov' into memory, so it
// is cheap even on a 400 MB book (it never reads 'mdat').

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Mp4Chapter {
    double      start_sec = 0.0;   // chapter start, seconds
    std::string title;             // UTF-8
};

// Returns chapters sorted by start_sec. Empty if the file has no chpl table,
// is not an MP4 container, or cannot be opened.
std::vector<Mp4Chapter> parseMp4Chapters(const std::string& utf8Path);

// Returns the true decoded channel count of an AAC track in an MP4/M4A/M4B
// container, read from the AudioSpecificConfig channelConfiguration
// (1..6 → that many channels, 7 → 8). Returns 0 when the file is not mp4/aac,
// carries an in-band (0) or reserved channel config, or cannot be parsed — in
// which case callers should keep their existing value (e.g. TagLib's). Exists
// because the legacy stsd AudioSampleEntry.channelcount field is widely
// hardcoded to 2 even for mono content, which TagLib reports verbatim.
int mp4AacChannelCount(const std::string& utf8Path);
