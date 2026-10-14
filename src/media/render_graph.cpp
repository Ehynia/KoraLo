#include "render_graph.hpp"

#include <algorithm>

namespace koralo {
namespace {

double overlap_fade_in(const Project& project, const Clip& clip) {
    if (!project.auto_crossfade) return clip.fade_in;
    double fade = clip.fade_in;
    for (const Clip* other : project.clips_on_track(clip.track_id)) {
        if (other->id == clip.id || other->start > clip.start) continue;
        const double overlap = other->end() - clip.start;
        if (overlap > 0) fade = std::max(fade, std::min(clip.duration, overlap));
    }
    return std::clamp(fade, 0.0, clip.duration);
}

double overlap_fade_out(const Project& project, const Clip& clip) {
    if (!project.auto_crossfade) return clip.fade_out;
    double fade = clip.fade_out;
    for (const Clip* other : project.clips_on_track(clip.track_id)) {
        if (other->id == clip.id || other->start <= clip.start || other->start >= clip.end()) continue;
        const double overlap = clip.end() - other->start;
        if (overlap > 0) fade = std::max(fade, std::min(clip.duration, overlap));
    }
    return std::clamp(fade, 0.0, clip.duration);
}

} // namespace

RenderGraph build_render_graph(const Project& project, const RenderPlan& plan) {
    RenderGraph graph;
    graph.width = plan.width;
    graph.height = plan.height;
    graph.fps = plan.fps;
    graph.duration = plan.duration;

    for (const auto& input : plan.inputs) {
        if (!input.clip || !input.track || !input.media) continue;
        const Clip& clip = *input.clip;
        const Track& track = *input.track;
        const MediaItem& media = *input.media;
        const double fade_in = overlap_fade_in(project, clip);
        const double fade_out = overlap_fade_out(project, clip);

        if (track.kind == TrackKind::Video && media.has_video) {
            RenderGraphVideoLayer layer;
            layer.clip = &clip;
            layer.track = &track;
            layer.media = &media;
            layer.opacity = std::clamp(clip.opacity * track.opacity, 0.0, 1.0);
            layer.fade_in = fade_in;
            layer.fade_out = fade_out;
            layer.stages = {
                RenderStageKind::Source,
                RenderStageKind::Transform,
                RenderStageKind::MediaEffects,
                RenderStageKind::ClipEffects,
                RenderStageKind::TrackEffects,
                RenderStageKind::OpacityAndFades,
                RenderStageKind::Blend};
            graph.video_layers.push_back(std::move(layer));
        }

        if (track.kind == TrackKind::Audio && media.has_audio) {
            RenderGraphAudioLayer layer;
            layer.clip = &clip;
            layer.track = &track;
            layer.media = &media;
            layer.gain = clip.gain * track.gain;
            layer.pan = std::clamp(track.pan, -1.0, 1.0);
            layer.fade_in = fade_in;
            layer.fade_out = fade_out;
            layer.stages = {
                RenderStageKind::Source,
                RenderStageKind::AudioRate,
                RenderStageKind::AudioClipEffects,
                RenderStageKind::AudioTrackEffects,
                RenderStageKind::AudioGainPan,
                RenderStageKind::AudioMix};
            graph.audio_layers.push_back(std::move(layer));
        }
    }

    graph.has_output_effects = !project.video_output_effects.empty();
    if (graph.has_output_effects) graph.output_stages.push_back(RenderStageKind::OutputEffects);
    return graph;
}

} // namespace koralo
