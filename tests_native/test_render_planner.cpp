#include "media/render_planner.hpp"

#include "platform/process.hpp"
#include "platform/app_paths.hpp"

#include <atomic>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace koralo;
namespace fs = std::filesystem;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static Project fixture(const fs::path& root, fs::path& source) {
    Project p = Project::make_default();
    p.settings.width = 1920;
    p.settings.height = 1080;
    p.settings.fps = 30.0;
    source = root / "source.mp4";
    std::ofstream(source, std::ios::binary) << "not-decoded-by-planner";

    MediaItem media;
    media.path = path_to_utf8(source);
    media.name = "source.mp4";
    media.duration = 10.0;
    media.has_video = true;
    media.width = 1920;
    media.height = 1080;
    media.fps = 30.0;
    const int media_id = p.add_media(std::move(media)).id;

    int video_track = 0;
    for (const auto& track : p.tracks) {
        if (track.kind == TrackKind::Video) {
            video_track = track.id;
            break;
        }
    }
    require(video_track != 0, "fixture video track");
    p.add_clip(media_id, video_track, 3.0, 0.5, 2.0);
    p.add_clip(media_id, video_track, 1.0, 0.0, 1.0);
    return p;
}

int main() {
    try {
        const auto root = temporary_directory() / ("koralo-render-planner-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(root, ec);
        require(!ec, "temporary planner directory");
        struct Cleanup { fs::path path; ~Cleanup(){ std::error_code e; fs::remove_all(path,e); } } cleanup{root};
        fs::path source;
        Project p = fixture(root, source);
        const auto output = root / "export.mp4";

        RenderOptions options;
        options.width = 1280;
        options.height = 720;
        options.fps = 24.0;
        options.video_bitrate_kbps = 2500;
        options.audio_bitrate_kbps = 128;
        options.use_gpu = true;
        std::string error;
        auto plan = plan_render_project(p, path_to_utf8(output), &options, true, nullptr, &error);
        require(plan.has_value(), "valid project produces render plan");
        require(plan->width == 1280 && plan->height == 720 && plan->fps == 24.0,
                "render plan resolves preset dimensions and fps");
        require(plan->video_bitrate_kbps == 2500 && plan->audio_bitrate_kbps == 128,
                "render plan resolves preset bitrates");
        require(plan->allow_gpu, "render plan preserves requested GPU capability");
        require(plan->inputs.size() == 2, "render plan includes visible clips");
        require(plan->inputs[0].clip->start == 1.0 && plan->inputs[1].clip->start == 3.0,
                "render plan stabilizes clip order within a track");

        auto* track = p.track_by_id(p.clips.front().track_id);
        require(track != nullptr, "planner fixture track lookup");
        track->mute = true;
        plan = plan_render_project(p, path_to_utf8(output), &options, true, nullptr, &error);
        require(plan && plan->inputs.empty(), "muted track is excluded from render inputs");
        track->mute = false;

        options.width = 1279;
        require(!plan_render_project(p, path_to_utf8(output), &options, true, nullptr, &error) &&
                    error == "Invalid H.264 size or frame rate",
                "odd H.264 width rejected during planning");
        options.width = 1280;

        std::atomic<bool> cancel{true};
        require(!plan_render_project(p, path_to_utf8(output), &options, true, &cancel, &error) &&
                    error == "Render cancelled",
                "pre-cancel rejected during planning");
        cancel = false;

        require(!plan_render_project(p, path_to_utf8(source), &options, true, nullptr, &error) &&
                    error == "Export cannot overwrite source media",
                "planner protects source media from overwrite");

        p.path = path_to_utf8(root / "project.kora");
        std::ofstream(path_from_utf8(p.path)) << "KORALO";
        require(!plan_render_project(p, p.path, &options, true, nullptr, &error) &&
                    error == "Export cannot overwrite the project",
                "planner protects project file from overwrite");

        p.path.clear();
        p.clips.front().track_id = 999999;
        require(!plan_render_project(p, path_to_utf8(output), &options, true, nullptr, &error) &&
                    error == "Missing event track",
                "planner rejects dangling track reference");

        std::cout << "KoraLo render planner tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
