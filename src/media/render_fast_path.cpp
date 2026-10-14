#include "render_fast_path.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <cstdint>
#include <limits>

namespace koralo {
namespace {

bool close(double x, double y, double eps = 1e-5) {
    return std::isfinite(x) && std::isfinite(y) && std::abs(x - y) < eps;
}

bool active_effects(const std::vector<EffectInstance>& effects) {
    return std::any_of(effects.begin(),effects.end(),
                       [](const EffectInstance& effect){return effect.enabled;});
}

bool vanilla_video(const RenderGraphVideoLayer& v, const RenderPlan& plan) {
    const auto* c = v.clip;
    const auto* m = v.media;
    return c && m && v.track && m->has_video && !m->path.empty() &&
           !m->generated_text.enabled && !m->generated_vector.enabled &&
           !c->loop && !c->video_motion.enabled &&
           !active_effects(c->video_effects) && !active_effects(m->video_effects) &&
           !active_effects(v.track->video_effects) &&
           close(c->start, 0.0) && close(c->rate, 1.0) &&
           c->source_offset() >= 0 &&
           // A short source would require the general graph to fill the tail.
           std::isfinite(m->duration) &&
           m->duration + 1.0 / plan.fps >= c->source_offset() + c->duration &&
           close(c->duration, plan.duration) &&
           close(v.opacity, 1.0) && close(v.fade_in, 0.0) &&
           close(v.fade_out, 0.0) &&
           m->width > 0 && m->height > 0 &&
           m->fps > 0.0 && close(m->fps, plan.fps, 0.015) &&
           // Alpha-carrying codecs still need compositing over black.
           (m->video_codec == "h264" || m->video_codec == "hevc" ||
            m->video_codec == "mpeg4" || m->video_codec == "mpeg2video" ||
            m->video_codec == "av1");
}

bool vanilla_audio(const RenderGraphAudioLayer& a,
                   const RenderGraphVideoLayer& v, const RenderPlan& plan) {
    const auto* c = a.clip;
    return c && a.media && a.track && v.media && v.clip &&
           a.media->has_audio && a.media->path == v.media->path &&
           !c->loop && close(c->start, 0.0) &&
           close(c->duration, plan.duration) && close(c->rate, 1.0) &&
           close(c->source_offset(), v.clip->source_offset()) &&
           close(a.gain, 1.0) && close(a.pan, 0.0) &&
           close(a.fade_in, 0.0) && close(a.fade_out, 0.0) &&
           !active_effects(c->audio_effects) && !active_effects(a.track->audio_effects) &&
           a.track->imported_audio_fx.empty();
}
} // namespace

std::optional<DirectExportSource> choose_direct_export(
    const Project& project, const RenderPlan& plan, const RenderGraph& graph) {
    if (plan.native_prepared || active_effects(project.video_output_effects) ||
        graph.video_layers.size() != 1 || graph.audio_layers.size() > 1 ||
        plan.inputs.size() != 1 + graph.audio_layers.size()) return std::nullopt;
    const auto& video = graph.video_layers.front();
    if (!vanilla_video(video, plan)) return std::nullopt;
    if (!graph.audio_layers.empty() &&
        !vanilla_audio(graph.audio_layers.front(), video, plan)) return std::nullopt;
    return DirectExportSource{video.media, video.clip, !graph.audio_layers.empty()};
}

std::optional<std::vector<CutExportSegment>> choose_cut_export(
    const Project& project, const RenderPlan& plan, const RenderGraph& graph) {
    if (plan.native_prepared || plan.fps <= 0 || !std::isfinite(plan.fps) ||
        graph.video_layers.size() < 2 ||
        active_effects(project.video_output_effects) ||
        plan.inputs.size() != graph.video_layers.size() + graph.audio_layers.size())
        return std::nullopt;
    std::vector<const RenderGraphVideoLayer*> ordered;
    ordered.reserve(graph.video_layers.size());
    for (const auto& layer : graph.video_layers) ordered.push_back(&layer);
    std::stable_sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){
        return a->clip->start < b->clip->start;
    });
    const auto track_id=ordered.front()->track->id;
    double next_time=0;
    std::vector<CutExportSegment> result;
    result.reserve(ordered.size());
    for (const auto* layer : ordered) {
        const Clip* c=layer->clip;
        const MediaItem* m=layer->media;
        const Track* t=layer->track;
        if (!c || !m || !t || t->id != track_id || !m->has_video ||
            m->path.empty() || m->generated_text.enabled || m->generated_vector.enabled ||
            c->loop || c->video_motion.enabled ||
            active_effects(c->video_effects) || active_effects(m->video_effects) ||
            active_effects(t->video_effects) ||
            !close(layer->opacity,1.0) || !close(layer->fade_in,0) ||
            !close(layer->fade_out,0) || !close(c->rate,1.0) ||
            !std::isfinite(c->start) || !std::isfinite(c->duration) ||
            !std::isfinite(m->duration) || c->duration <= 0 ||
            c->duration > double(std::numeric_limits<std::int64_t>::max())/plan.fps ||
            c->source_offset() < 0 || m->duration+1.0/plan.fps < c->source_offset()+c->duration ||
            m->width <= 0 || m->height <= 0 || m->fps <= 0 ||
            !close(m->fps,plan.fps,0.015) ||
            (m->video_codec!="h264" && m->video_codec!="hevc" &&
             m->video_codec!="mpeg4" && m->video_codec!="mpeg2video" &&
             m->video_codec!="av1")) return std::nullopt;
        // Restrict to exact frame boundaries: concat must preserve the project
        // duration, not round away a partial frame or hide an intentional gap.
        const auto frames_ll=std::llround(c->duration*plan.fps);
        if (frames_ll<=0 || std::abs(c->duration-double(frames_ll)/plan.fps) > 1e-4 ||
            std::abs(c->start-next_time)>1e-4) return std::nullopt;
        result.push_back({c,m,static_cast<std::uint64_t>(frames_ll)});
        next_time+=double(frames_ll)/plan.fps;
    }
    if (std::abs(next_time-plan.duration)>1e-4) return std::nullopt;
    return result;
}
} // namespace koralo
