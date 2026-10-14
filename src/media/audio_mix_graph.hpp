#pragma once

#include "../core/project.hpp"
#include "audio_preview_timing.hpp"
#include <cstddef>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace koralo {

// One FFmpeg lavfi graph, one ffplay audio device for all audible timeline
// sources. This is a bounded transition architecture until the audio device
// supplies a sample-accurate master clock and native PCM callbacks.
struct AudioMixSegment {
    const Clip* clip = nullptr;
    const Track* track = nullptr;
    const MediaItem* media = nullptr;
    double source_offset = 0.0;
    double source_seconds = 0.0;
    double timeline_seconds = 0.0;
    double delay_seconds = 0.0;
    double event_time = 0.0;
    double duck_gain = 1.0;
    std::vector<std::pair<double,double>> duck_windows;
};
struct AudioMixPlan {
    double anchor = 0.0;
    double horizon = 0.0;
    std::vector<AudioMixSegment> segments;
    bool complete = true;
};

AudioMixPlan plan_audio_mix(const Project& project, double anchor,
                           double horizon_seconds = 90.0,
                           std::size_t max_sources = 48);

// Extra audio FX are deliberately injected via the same filter serializer as
// final export: preview and render must not silently use different presets.
using AudioEffectAppender = std::function<void(std::ostringstream&,
                                               const std::vector<EffectInstance>&)>;
std::optional<std::string> build_audio_mix_lavfi(
    const AudioMixPlan& plan, const AudioEffectAppender& append_effects);

} // namespace koralo
