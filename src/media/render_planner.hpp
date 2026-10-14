#pragma once

#include "../core/project.hpp"

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace koralo {

class RenderControlSession;

struct RenderOptions {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int video_bitrate_kbps = 12000;
    int audio_bitrate_kbps = 192;
    bool use_gpu = true;
    bool native_prepared = false;
    bool lossless_intermediate = false;
    std::string preset_name = "KoraLo High Quality";
    bool fsr_upscale = false;
    float fsr_sharpness = .5f;
    // Internal, non-owning pointer to the active export-control session.
    RenderControlSession* control = nullptr;
    bool verify_output = true;
};

struct PlannedRenderInput {
    const Clip* clip = nullptr;
    const Track* track = nullptr;
    const MediaItem* media = nullptr;
};

struct RenderPlan {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int video_bitrate_kbps = 12000;
    int audio_bitrate_kbps = 192;
    bool allow_gpu = false;
    bool native_prepared = false;
    bool video_solo = false;
    bool audio_solo = false;
    double duration = 0.1;
    std::filesystem::path destination;
    std::vector<PlannedRenderInput> inputs;
};

// Pure render preflight/planning. It validates project/output references and
// resolves the visible, ordered clip inputs without launching FFmpeg or
// allocating render intermediates. This is the seam that a future RenderGraph
// can consume without depending on MediaEngine internals.
std::optional<RenderPlan> plan_render_project(
    const Project& project,
    const std::string& output,
    const RenderOptions* options,
    bool gpu_enabled,
    const std::atomic<bool>* cancel,
    std::string* error = nullptr);

} // namespace koralo
