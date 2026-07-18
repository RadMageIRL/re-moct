// NoopMediaControl.h - the inert IMediaControl (osmedia-seam). The production
// default on a Windows build without the wingui window (no HWND) and on any
// unsupported platform, and the internal fallback when a platform impl cannot
// bind (e.g. no session bus). Every call is a no-op; it never invokes a handler.
#pragma once
#include "core/IMediaControl.h"

namespace core {

class NoopMediaControl : public IMediaControl {
public:
    void updateNowPlaying(const MediaMeta&, MediaStatus, double, double) override {}
    void updatePosition(double, double, MediaStatus) override {}
    void clear() override {}
    void setDefaultArt(std::vector<uint8_t>) override {}
    void setCommandHandler(std::function<void(MediaEvent)>) override {}
    void pump() override {}
};

} // namespace core
