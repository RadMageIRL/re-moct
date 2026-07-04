#pragma once

// ─── LocalFileSource ──────────────────────────────────────────────────────────
// Local-file playback source (Phase 2 slice B) — the third core::ISource
// implementation. Extracted from AudioManager, where the file path lived as
// inline state (ma_decoder decoder_/next_decoder_ + free functions): the
// decoder, the open/prime sequence, TagLib metadata, the seek-with-prime logic,
// and the cursor now live here. The decoder is opened at a fixed heap address
// for the object's whole life — the audio-thread track swap in AudioManager is
// a pointer move, no longer a by-value copy of a library struct.
//
// Threading contract (inherited from the baseline it was extracted from):
//   - open()/close()/seekTo()/seekToFrame() are main-thread calls, made only in
//     device-stopped windows (AudioManager's discipline; ma_device_stop/start
//     are the fences) — EXCEPT seekToFrame(0), which the audio thread calls on
//     the repeat-one path exactly as the baseline seeked decoder_ there.
//   - readFrames()/positionSec() are called from the audio callback;
//     positionSec() is also read from the UI thread (the baseline's latent
//     cursor race, unchanged in kind — see AudioManager's retirement scheme,
//     which guarantees the raced dereference always hits a live object).
//
// File-specific surface (open/info/path/channels/sampleRate/seekToFrame) is
// deliberately NOT part of core::ISource — the approved open()/metadata
// exclusions; metadata is upfront TagLib and stays a concrete accessor.

#include "core/ISource.h"
#include "TrackInfo.h"
#include "miniaudio.h"   // declarations only — MINIAUDIO_IMPLEMENTATION lives in AudioManager.cpp

#include <cstdint>
#include <string>

class LocalFileSource : public core::ISource {
public:
    LocalFileSource() = default;
    ~LocalFileSource() override { close(); }
    LocalFileSource(const LocalFileSource&)            = delete;
    LocalFileSource& operator=(const LocalFileSource&) = delete;

    // Open the file: decoder (forced f32/2ch/44100, FDK-AAC custom backend) +
    // bad-first-frame priming + TagLib metadata into info(). Returns false if
    // the decoder can't be opened or reports a zero format (the baseline
    // play()-path guard). Main thread, device stopped.
    bool open(const std::string& path);

    const TrackInfo&   info() const { return info_; }   // TagLib metadata, upfront
    const std::string& path() const { return path_; }
    uint32_t channels()   const { return decoder_.outputChannels; }    // always 2 (forced)
    uint32_t sampleRate() const { return decoder_.outputSampleRate; }  // always 44100 (forced)

    // RAW seek, no reservoir priming — the baseline's repeat-one loop
    // (ma_decoder_seek_to_pcm_frame(0) on the audio thread) and the setDevice
    // playback-restart use exactly this shape.
    void seekToFrame(uint64_t frame);

    // ── core::ISource ────────────────────────────────────────────────────────
    // Pull-decoded source: returns the frames actually produced; a short return
    // means end-of-source (the caller pads the remainder), per the contract.
    uint32_t readFrames(float* dst, uint32_t frame_count) override;
    core::SourceCaps caps() const override {
        return { .seekable = true, .finite = true, .live = false };
    }
    double positionSec() const override;   // decoder cursor / rate (baseline math)
    double durationSec() const override {  // TagLib duration — deliberately NOT the
        return (double)info_.duration_sec; // decoder length (no-Xing cursor race,
    }                                      // the baseline cached_duration_ decision)
    bool   seekTo(double seconds) override;  // seek + ~180ms decode-discard prime
    void   close() override;

private:
    ma_decoder  decoder_ {};
    bool        ready_ = false;
    TrackInfo   info_;
    std::string path_;
};
