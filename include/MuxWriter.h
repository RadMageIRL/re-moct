// MuxWriter.h — whole-frame cut writers for the copy path (abi-cluster
// slice B). The copy pump hands each parsed encoded frame (FrameSync.h) to
// exactly one of these per cut:
//
//   Mp3AppendWriter — raw frame append. An MPEG stream IS its own container;
//                     the byte-fidelity claim is literal (the file's audio
//                     bytes are a subset of the broadcast bytes). Tags ride
//                     the existing ID3v2 pass afterwards.
//   M4aMuxWriter    — de-ADTS the access units into an ISO-BMFF .m4a:
//                     ftyp + mdat (raw AUs) + moov built at finalize from
//                     the per-frame size table. The AudioSpecificConfig is
//                     derived from the FIRST frame's ADTS header fields —
//                     never assumed. Correctness oracle: the app's own
//                     AacDecoder (Mp4Aac box parser) round-trips the file.
//
// Same discipline as IEncoder: open/write/finalize(bool), fflush-and-check
// at finalize so an incomplete file is a FAILED cut, never a silent one.
// Worker-thread only, like the encoders.
#pragma once

#include "FrameSync.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class MuxWriter {
public:
    virtual ~MuxWriter() = default;
    virtual bool open(const std::string& path) = 0;
    // One WHOLE frame, header included, with its parsed header info.
    virtual bool writeFrame(const uint8_t* frame, uint32_t len,
                            const framesync::FrameInfo& fi) = 0;
    // keep=true: flush, complete the container, verify (false = failed cut).
    // keep=false: abandon — close only; the caller removes the file.
    virtual bool finalize(bool keep) = 0;
    uint64_t framesWritten() const { return frames_written_; }

protected:
    uint64_t frames_written_ = 0;
};

// ── MP3: the container is the stream ─────────────────────────────────────────
class Mp3AppendWriter final : public MuxWriter {
public:
    ~Mp3AppendWriter() override;
    bool open(const std::string& path) override;
    bool writeFrame(const uint8_t* frame, uint32_t len,
                    const framesync::FrameInfo& fi) override;
    bool finalize(bool keep) override;

private:
    std::FILE* f_ = nullptr;
};

// ── ADTS -> M4A ───────────────────────────────────────────────────────────────
class M4aMuxWriter final : public MuxWriter {
public:
    ~M4aMuxWriter() override;
    bool open(const std::string& path) override;
    bool writeFrame(const uint8_t* frame, uint32_t len,
                    const framesync::FrameInfo& fi) override;
    bool finalize(bool keep) override;

private:
    std::FILE*            f_ = nullptr;
    std::vector<uint32_t> sizes_;        // per-AU byte sizes (stsz)
    uint64_t              mdat_bytes_ = 0;
    // From the first frame's ADTS header — the ASC/stsd source of truth.
    uint32_t sample_rate_ = 0;
    uint8_t  profile_ = 0, sr_index_ = 0, chan_cfg_ = 0;
};
