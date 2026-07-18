// ArtMissCache.h — a time-bounded negative cache for cover-art lookups
// (radio-art-refresh-fix, Fix #2).
//
// The old shape (a session-lifetime unordered_set) permanently poisoned a
// song on ONE transient failure: urlBySong returns "" for both "provider has
// no art" AND "the network hiccupped", the two are indistinguishable at the
// miss site, so a single timeout blanked that song for the whole session
// (only a non-empty station-art URL bypassed it). Time-bounding is the
// smallest correct fix: a miss suppresses re-lookups for TTL_MS (a genuine
// no-art song is still rate-limited to one attempt per window, never
// hammered per tick), then expires — so a transient failure self-heals on
// the song's next rotation. Shared by the radio Info-pane path and the
// Discord art path (they already shared the old set).
//
// Tick math: uint32 wrap-safe via a SIGNED 32-BIT cast — (int32_t)(a-b),
// deliberately NOT (long): long is 64-bit on Linux/LP64, which turns every
// wrapped difference positive and expires everything instantly (caught by
// art_miss_cache_test's first Linux run). Header-only so the test pins the
// expiry contract on both toolchains without UI linkage.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

struct ArtMissCache {
    static constexpr uint32_t TTL_MS = 15u * 60u * 1000u;   // one attempt / 15 min

    void add(const std::string& key, uint32_t now_ms) {
        m_[key] = now_ms + TTL_MS;
    }
    // True while the miss is still fresh. An expired entry is erased on the
    // way out (self-healing lookup — no sweep pass needed).
    bool hit(const std::string& key, uint32_t now_ms) {
        auto it = m_.find(key);
        if (it == m_.end()) return false;
        if ((int32_t)(now_ms - it->second) >= 0) { m_.erase(it); return false; }
        return true;
    }

private:
    std::unordered_map<std::string, uint32_t> m_;
};
