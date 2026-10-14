#pragma once

#include "render_planner.hpp"

#include <vector>

namespace koralo {

enum class RenderStageKind {
    Source,
    Transform,
    MediaEffects,
    ClipEffects,
    TrackEffects,
    OpacityAndFades,
    Blend,
    AudioRate,
    AudioClipEffects,
    AudioTrackEffects,
    AudioGainPan,
    AudioMix,
    OutputEffects
};

struct RenderGraphVideoLayer {
    const Clip* clip = nullptr;
    const Track* track = nullptr;
    const MediaItem* media = nullptr;
    double opacity = 1.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
    std::vector<RenderStageKind> stages;
};

struct RenderGraphAudioLayer {
    const Clip* clip = nullptr;
    const Track* track = nullptr;
    const MediaItem* media = nullptr;
    double gain = 1.0;
    double pan = 0.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
    std::vector<RenderStageKind> stages;
};

struct RenderGraph {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration = 0.0;
    std::vector<RenderGraphVideoLayer> video_layers;
    std::vector<RenderGraphAudioLayer> audio_layers;
    bool has_output_effects = false;
    std::vector<RenderStageKind> output_stages;
};

// Builds the renderer-neutral semantic graph from an already validated plan.
// Backends (currently FFmpeg; later CPU/GPU preview/encode) consume this graph
// instead of rediscovering visibility, fades and stage ordering independently.
RenderGraph build_render_graph(const Project& project, const RenderPlan& plan);

} // namespace koralo
