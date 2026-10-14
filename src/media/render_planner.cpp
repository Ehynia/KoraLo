#include "render_planner.hpp"

#include "../platform/process.hpp"

#include <algorithm>
#include <cmath>

namespace koralo {
namespace fs = std::filesystem;

std::optional<RenderPlan> plan_render_project(
    const Project& project,
    const std::string& output,
    const RenderOptions* options,
    bool gpu_enabled,
    const std::atomic<bool>* cancel,
    std::string* error) {
    auto fail = [&](std::string message) -> std::optional<RenderPlan> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };

    if (project.clips.empty()) return fail("timeline is empty");

    RenderPlan plan;
    plan.width = options && options->width > 0 ? options->width : project.settings.width;
    plan.height = options && options->height > 0 ? options->height : project.settings.height;
    plan.fps = options && options->fps > 0.0 ? options->fps : project.settings.fps;
    plan.video_bitrate_kbps = std::clamp(options ? options->video_bitrate_kbps : 12000, 500, 200000);
    plan.audio_bitrate_kbps = std::clamp(options ? options->audio_bitrate_kbps : 192, 64, 1024);
    plan.allow_gpu = (!options || options->use_gpu) && gpu_enabled;
    plan.native_prepared = options && options->native_prepared;
    plan.duration = std::max(0.1, project.duration());

    if (plan.width < 2 || plan.height < 2 || plan.width > 16384 || plan.height > 16384 ||
        plan.width % 2 || plan.height % 2 || !std::isfinite(plan.fps) ||
        plan.fps < 1.0 || plan.fps > 240.0) {
        return fail("Invalid H.264 size or frame rate");
    }
    if(options&&options->fsr_upscale){
        if(!std::isfinite(options->fsr_sharpness)||options->fsr_sharpness<0||options->fsr_sharpness>1)
            return fail("FSR sharpness must be between 0 and 1");
        if(project.settings.width<2||project.settings.height<2||project.settings.width%2||project.settings.height%2||
           plan.width<project.settings.width||plan.height<project.settings.height||
           std::uint64_t(plan.width)*plan.height>16*1024*1024)
            return fail("FSR: output must be at least the project size, both dimensions even, and <= 16 megapixels. Check resolution in Project Settings and Export.");
        if(std::abs(double(plan.width)/plan.height-double(project.settings.width)/project.settings.height)>.01)
            return fail("FSR output must preserve the project aspect ratio");
    }
    if (cancel && cancel->load()) return fail("Render cancelled");

    std::error_code output_error;
    plan.destination = fs::absolute(path_from_utf8(output), output_error);
    if (output_error || !fs::is_directory(plan.destination.parent_path(), output_error)) {
        return fail("Export directory unavailable");
    }

    const auto same_destination = [&](const std::string& source) {
        if (source.empty()) return false;
        std::error_code ec;
        return fs::equivalent(path_from_utf8(source), plan.destination, ec);
    };
    if (same_destination(project.path)) return fail("Export cannot overwrite the project");
    for (const auto& media : project.media) {
        if (!media.generated_text.enabled && same_destination(media.path)) {
            return fail("Export cannot overwrite source media");
        }
    }

    for (const auto& track : project.tracks) {
        if (track.kind == TrackKind::Video && track.solo) plan.video_solo = true;
        if (track.kind == TrackKind::Audio && track.solo) plan.audio_solo = true;
    }

    for (const auto& clip : project.clips) {
        const Track* track = project.track_by_id(clip.track_id);
        if (!track) return fail("Missing event track");
        const bool solo_filter =
            (track->kind == TrackKind::Video ? plan.video_solo : plan.audio_solo) && !track->solo;
        if (track->mute || clip.muted || solo_filter) continue;

        const MediaItem* media = project.media_by_id(clip.media_id());
        if (!media) return fail("Missing event media");
        if (clip.duration <= 0 || clip.rate <= 0 || clip.rate > 100) {
            return fail("Invalid event rate or duration");
        }
        plan.inputs.push_back({&clip, track, media});
    }

    std::stable_sort(plan.inputs.begin(), plan.inputs.end(), [&](const auto& a, const auto& b) {
        const auto order = [&](const Track* track) {
            return std::find_if(project.tracks.begin(), project.tracks.end(),
                                [&](const Track& item) { return item.id == track->id; }) -
                   project.tracks.begin();
        };
        const auto ta = order(a.track);
        const auto tb = order(b.track);
        return ta != tb ? ta < tb : a.clip->start < b.clip->start;
    });

    return plan;
}

} // namespace koralo
