#include "frame_cache.hpp"

#include <utility>

namespace koralo {

std::optional<Frame> FrameCache::get(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    it->second.stamp = ++stamp_;
    return it->second.frame;
}

void FrameCache::put(std::string key, Frame frame) {
    const auto bytes = frame.rgb.size() + frame.alpha.size();
    auto existing = map_.find(key);
    if (existing != map_.end()) {
        used_bytes_ -= existing->second.frame.rgb.size() + existing->second.frame.alpha.size();
        map_.erase(existing);
    }
    if (bytes > max_bytes_) return;

    used_bytes_ += bytes;
    map_.emplace(std::move(key), Entry{std::move(frame), ++stamp_});
    while (map_.size() > max_items_ || used_bytes_ > max_bytes_) {
        auto victim = map_.begin();
        for (auto it = map_.begin(); it != map_.end(); ++it) {
            if (it->second.stamp < victim->second.stamp) victim = it;
        }
        used_bytes_ -= victim->second.frame.rgb.size() + victim->second.frame.alpha.size();
        map_.erase(victim);
    }
}

void FrameCache::clear() {
    map_.clear();
    stamp_ = 0;
    used_bytes_ = 0;
}

} // namespace koralo
