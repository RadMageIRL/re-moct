// PluginSource.h — the host-side driver for a source behind the plugin C ABI
// (Phase 4 slice b). A thin wrapper holding the RemoctPlugin op table + one
// instance, exposing the same method names StreamSource did so AudioManager's
// call sites change by rename only.
//
// Lifetime mirrors the by-value stream_source_ member it replaces: the instance
// is created ONCE in the ctor and destroyed ONCE in the dtor; open()/close()
// cycle the source's session inside that instance. `url()` and `paused()` are
// host-tracked (the ABI has no getter — they are host bookkeeping, not source
// state). Platform-free.
#pragma once
#include <atomic>
#include <string>

#include "core/remoct_plugin.h"

namespace core {

class PluginSource {
public:
    // Bind to a descriptor + host services and create the instance once. Both
    // pointers must outlive this object (the ABI service-lifetime contract).
    PluginSource(const RemoctPlugin* plugin, const RemoctHostServices* host);
    ~PluginSource();
    PluginSource(const PluginSource&)            = delete;
    PluginSource& operator=(const PluginSource&) = delete;

    bool valid() const { return self_ != nullptr; }

    // Blocking; true on success (mirrors StreamSource::open). Remembers the url.
    bool open(const std::string& url);
    void close();                              // instance kept; session torn down
    uint32_t readFrames(float* dst, uint32_t frame_count);  // AUDIO thread

    void pause(bool p);
    bool paused() const { return paused_.load(); }          // host-tracked
    bool buffering() const;
    std::string nowPlaying() const;
    std::string currentArtUrl() const;
    const std::string& url() const { return url_; }         // host-tracked
    double positionSec() const;
    void setPreferDigital(bool b);

private:
    const RemoctPlugin* plugin_ = nullptr;
    void*               self_   = nullptr;     // created once in the ctor
    std::string         url_;
    std::atomic<bool>   paused_{false};
};

} // namespace core
