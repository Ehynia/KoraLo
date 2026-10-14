#pragma once

#include "../core/project.hpp"
#include "frame_cache.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace koralo {

struct PreviewRequest {
    std::string path;
    double time = 0.0;
    int width = 640;
    int height = 360;
    std::uint64_t token = 0;
    bool apply_motion = false;
    VideoMotionSample motion;
    std::vector<EffectInstance> effects;
    double effect_time = 0.0;
    std::vector<EffectInstance> output_effects;
    double output_effect_time = 0.0;
    bool allow_prepared_cache = true;
    bool continuous = false;
    double requested_fps = 30.0;
    double source_rate = 1.0;
    double opacity = 1.0;
    bool has_text_overlay = false;
    GeneratedText text_overlay;
    double text_opacity = 1.0;
    double text_event_time = 0.0;
    double text_event_duration = 5.0;
    bool has_background = false;
    std::string background_path;
    double background_time = 0.0;
    bool background_apply_motion = false;
    VideoMotionSample background_motion;
    std::vector<EffectInstance> background_effects;
    double background_effect_time = 0.0;
    double background_opacity = 1.0;
    bool background_allow_prepared_cache = true;
    std::uint64_t playback_epoch = 0;
    bool playback = false;
    VectorScene vector_source;
    VectorScene background_vector_source;
    double background_source_rate = 1.0;
    std::vector<PreviewRequest> layers; // Bottom to top; each layer owns its source/effects.
    int stream_id = 0; // Timeline event id, not file path (takes can share a file).
    bool preserve_alpha = false;
};

std::vector<PreviewRequest> build_preview_layers(const Project& project, double time, bool prepared_cache);

class PlaybackController {
public:
    void note_request(const PreviewRequest& request);
    void queue_request(PreviewRequest request, std::string key);
    bool wait_next(const std::atomic<bool>& stop, PreviewRequest& request);
    void wake();

    bool should_publish(const PreviewRequest& request) const;
    void publish_worker_result(const PreviewRequest& request, Frame frame, std::string error);
    void publish_cached(std::uint64_t token, Frame frame);
    bool poll(std::uint64_t& token, Frame& frame, std::string& error);

    std::uint64_t latest_token() const { return latest_preview_token_.load(std::memory_order_acquire); }
    std::uint64_t latest_epoch() const { return latest_playback_epoch_.load(std::memory_order_relaxed); }

private:
    struct Result {
        std::uint64_t token = 0;
        Frame frame;
        std::string error;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<PreviewRequest> pending_;
    std::string pending_key_;
    std::deque<Result> results_;
    std::atomic<std::uint64_t> latest_preview_token_{0};
    std::atomic<std::uint64_t> latest_playback_epoch_{0};
};

} // namespace koralo
