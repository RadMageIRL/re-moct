// PluginSource.cpp — host-side plugin driver (Phase 4 slice b). See PluginSource.h.
#include "PluginSource.h"

namespace core {

// Pull a plugin string via the snprintf-grow contract: try a stack buffer, and
// grow+retry once if the full length didn't fit. fn may be null (optional
// metadata capability) -> empty.
static std::string pullString(void* self, std::size_t (*fn)(void*, char*, std::size_t)) {
    if (!self || !fn) return {};
    char buf[256];
    std::size_t need = fn(self, buf, sizeof(buf));
    if (need < sizeof(buf)) return std::string(buf, need);
    std::string s(need, '\0');                 // room for `need` chars + the NUL slot
    fn(self, s.data(), need + 1);
    return s;
}

PluginSource::PluginSource(const RemoctPlugin* plugin, const RemoctHostServices* host)
    : plugin_(plugin) {
    if (plugin_ && plugin_->create) self_ = plugin_->create(host, nullptr);
}

PluginSource::~PluginSource() {
    if (self_ && plugin_->destroy) plugin_->destroy(self_);
}

bool PluginSource::open(const std::string& url) {
    url_ = url;
    return (self_ && plugin_->open) ? plugin_->open(self_, url.c_str()) == 0 : false;
}

void PluginSource::close() {
    if (self_ && plugin_->close) plugin_->close(self_);
}

uint32_t PluginSource::readFrames(float* dst, uint32_t frame_count) {
    return (self_ && plugin_->read_frames) ? plugin_->read_frames(self_, dst, frame_count) : 0;
}

void PluginSource::pause(bool p) {
    paused_.store(p);
    if (self_ && plugin_->set_paused) plugin_->set_paused(self_, p ? 1 : 0);
}

bool PluginSource::buffering() const {
    return (self_ && plugin_->is_buffering) ? plugin_->is_buffering(self_) != 0 : false;
}

std::string PluginSource::nowPlaying() const {
    return self_ ? pullString(self_, plugin_->now_playing) : std::string();
}

std::string PluginSource::currentArtUrl() const {
    return self_ ? pullString(self_, plugin_->art_url) : std::string();
}

double PluginSource::positionSec() const {
    return (self_ && plugin_->position_sec) ? plugin_->position_sec(self_) : 0.0;
}

void PluginSource::setPreferDigital(bool b) {
    if (self_ && plugin_->set_config) plugin_->set_config(self_, "prefer_digital", b ? "1" : "0");
}

} // namespace core
