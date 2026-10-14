#include "media/media_engine.hpp"
#include "platform/app_paths.hpp"
#include "platform/process.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace koralo;
namespace fs = std::filesystem;

static void req(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    MediaEngine engine;
    if (!engine.ready()) {
        std::cout << "SKIP: ffmpeg unavailable\n";
        return 77;
    }

    TemporaryFile reservation(temporary_directory(), ".media-concurrency");
    req(reservation.valid(), "reserve media concurrency directory");
    const fs::path root = reservation.release();
    std::error_code ec;
    fs::remove(root, ec);
    req(!ec, "release media concurrency reservation");
    fs::create_directories(root, ec);
    req(!ec, "create media concurrency directory");
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code error; fs::remove_all(root, error); }
    } cleanup{root};

    const fs::path source_path = root / "worker stress source.mp4";
    const std::string source = path_to_utf8(source_path);
    const auto generated = run_process(engine.ffmpeg_path(), {
        "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=30",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
        "-t", "2", "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", source
    });
    req(generated.started && generated.exit_code == 0, "generate media concurrency source");

    std::string error;
    auto probed = engine.probe(source, &error);
    req(probed.has_value(), "probe media concurrency source");

    Project project = Project::make_default();
    MediaItem media = *probed;
    media.id = 0;
    MediaItem& stored = project.add_media(std::move(media));
    const int media_id = stored.id;
    int video_track = 0;
    int audio_track = 0;
    for (const auto& track : project.tracks) {
        if (!video_track && track.kind == TrackKind::Video) video_track = track.id;
        if (!audio_track && track.kind == TrackKind::Audio) audio_track = track.id;
    }
    req(video_track != 0 && audio_track != 0, "default project tracks");
    project.add_clip(media_id, video_track, 0.0, 0.0, 1.8);
    project.add_clip(media_id, audio_track, 0.0, 0.0, 1.8);

    engine.prepare_media(stored);
    const auto initial_thumbnail_revision = engine.thumbnail_revision();
    std::uint64_t completed_previews = 0;

    for (std::uint64_t token = 1; token <= 30; ++token) {
        PreviewRequest preview;
        preview.path = source;
        preview.time = 0.05 + static_cast<double>(token - 1) / 30.0;
        preview.width = 160;
        preview.height = 90;
        preview.token = token;
        preview.continuous = true;
        preview.playback = true;
        preview.playback_epoch = 1;
        preview.requested_fps = 30.0;
        preview.source_rate = 1.0;
        preview.allow_prepared_cache = false;
        engine.request_preview(std::move(preview));

        (void)engine.timeline_thumbnail(source, static_cast<double>(token % 20) / 20.0, 80, 45);
        (void)engine.master_meter(project, static_cast<double>(token % 18) / 10.0);
        (void)engine.preparation_status(source);
        (void)engine.overall_preparation_status();

        std::uint64_t result_token = 0;
        Frame frame;
        std::string preview_error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (engine.poll_preview(result_token, frame, preview_error)) {
                if (!frame.rgb.empty()) ++completed_previews;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if ((token % 7) == 0) engine.clear_composite_cache();
    }

    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool preparation_ready = false;
    bool meter_ready = false;
    while (std::chrono::steady_clock::now() < settle_deadline) {
        preparation_ready = engine.preparation_status(source).ready;
        meter_ready = engine.master_meter(project, 0.5).ready;
        (void)engine.timeline_thumbnail(source, 0.75, 80, 45);
        if (preparation_ready && meter_ready && engine.thumbnail_revision() > initial_thumbnail_revision) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    req(completed_previews > 0, "preview worker produced frames");
    req(engine.thumbnail_revision() > initial_thumbnail_revision, "thumbnail worker produced frames");
    req(preparation_ready, "preparation worker completed");
    req(meter_ready, "meter worker completed");

    std::cout << "KoraLo media concurrency test passed\n";
    return 0;
}
