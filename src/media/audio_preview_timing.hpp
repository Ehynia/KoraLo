#pragma once

#include "../core/project.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace koralo {

// Pure audio-preview bounds. The preview helper must never restart an exhausted
// non-looping source: doing so sounds like repeated clicks at the end of a clip.
struct AudioPreviewWindow {
    double source_offset = 0.0;
    double source_length = 0.0;
    double timeline_length = 0.0;
};

inline std::optional<AudioPreviewWindow> audio_preview_window(
    const Clip& clip, const MediaItem& media, double timeline_time) {
    if (!std::isfinite(timeline_time) || !std::isfinite(clip.rate) ||
        clip.rate <= 0.0 || !clip.contains(timeline_time)) return std::nullopt;
    const double rate = std::clamp(clip.rate, 0.01, 100.0);
    double source = std::max(0.0, clip.source_offset() +
                         (timeline_time - clip.start) * rate);
    if (clip.loop && media.duration > 0.0 && std::isfinite(media.duration))
        source = std::fmod(source, media.duration);
    const double timeline_left = clip.end() - timeline_time;
    if (!clip.loop && media.duration > 0.0 && std::isfinite(media.duration)) {
        if (source >= media.duration - 1e-6) return std::nullopt;
        const double allowed = (media.duration - source) / rate;
        if (allowed <= 1e-6) return std::nullopt;
        return AudioPreviewWindow{source, std::min(timeline_left, allowed) * rate,
                                  std::min(timeline_left, allowed)};
    }
    if (timeline_left <= 1e-6) return std::nullopt;
    return AudioPreviewWindow{source, timeline_left * rate, timeline_left};
}

} // namespace koralo
