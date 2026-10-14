#include "media/media_engine.hpp"
#include "platform/process.hpp"
#include "platform/app_paths.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace koralo;
namespace fs = std::filesystem;

static void req(bool ok,const char* m){ if(!ok) throw std::runtime_error(m); }

static std::vector<int> ensure_video_tracks(Project& p, int count) {
    std::vector<int> ids;
    for (const auto& t : p.tracks) if (t.kind == TrackKind::Video) ids.push_back(t.id);
    while (static_cast<int>(ids.size()) < count) ids.push_back(p.add_track(TrackKind::Video, "Video " + std::to_string(ids.size()+1)).id);
    return ids;
}

static EffectInstance make_anime_restore_instance(int id) {
    auto fx = make_effect_instance("koralo.anime_line_restore", id);
    // Keep defaults intentionally mild; the goal of this regression is long-run
    // stability under a real CPU Anime path, not aesthetic tuning.
    return fx;
}

int main(){
    MediaEngine e;
    if(!e.ready()){ std::cout << "SKIP: ffmpeg unavailable\n"; return 77; }

    const char* image_env = std::getenv("KORALO_STABILITY_IMAGE");
    if(!image_env || std::string(image_env).empty()){
        std::cout << "SKIP: KORALO_STABILITY_IMAGE not set\n";
        return 77;
    }
    const fs::path image_src(image_env);
    if(!fs::exists(image_src)){
        std::cout << "SKIP: stability image missing\n";
        return 77;
    }

    TemporaryFile reservation(temporary_directory(), ".stability-tests");
    req(reservation.valid(), "reserve isolated stability test directory");
    const fs::path test_root = reservation.release();
    std::error_code reserve_error; fs::remove(test_root, reserve_error); req(!reserve_error, "release reservation");
    struct Cleanup { fs::path root; ~Cleanup(){ std::error_code ec; fs::remove_all(root, ec);} } cleanup{test_root};

    const fs::path dir = test_root / path_from_utf8("koralo stability тест");
    std::error_code ec; fs::create_directories(dir, ec); req(!ec, "create stability temp directory");
    const fs::path image_copy = dir / path_from_utf8("koralo plush stability source.jpg");
    fs::copy_file(image_src, image_copy, fs::copy_options::overwrite_existing, ec); req(!ec, "copy source image");

    const fs::path src_video_path = dir / path_from_utf8("koralo stability long source.mp4");
    const fs::path out_video_path = dir / path_from_utf8("koralo stability long render.mp4");
    const fs::path short_out_video_path = dir / path_from_utf8("koralo stability short render.mp4");
    const fs::path project_path = dir / path_from_utf8("koralo stability scenario.kora");

    const std::string src_video = path_to_utf8(src_video_path);
    const std::string out_video = path_to_utf8(out_video_path);
    const std::string short_out_video = path_to_utf8(short_out_video_path);
    const std::string image = path_to_utf8(image_copy);

    // 15-minute source video from the supplied image.  1 fps keeps the run
    // practical while still exercising a genuinely long timeline and long GOP
    // decode/seek paths.  A silent audio track is not required for this test.
    const auto generated = run_process(e.ffmpeg_path(), {
        "-hide_banner","-loglevel","error","-y",
        "-loop","1","-framerate","1","-i",image,
        "-t","900",
        "-vf","scale=160:90:force_original_aspect_ratio=decrease,pad=160:90:(ow-iw)/2:(oh-ih)/2:white,format=yuv420p",
        "-c:v","libx264","-preset","ultrafast","-pix_fmt","yuv420p",
        src_video
    });
    if(generated.exit_code != 0 && !generated.output.empty()) std::cerr << generated.output;
    req(generated.started && generated.exit_code == 0, "generate 15-minute test video from supplied image");

    std::string err;
    auto src_media = e.probe(src_video, &err);
    if(!src_media && !err.empty()) std::cerr << err;
    req(src_media.has_value(), "probe generated long source video");
    req(src_media->has_video && !src_media->has_audio, "generated source is video-only");
    req(src_media->width == 160 && src_media->height == 90, "generated source dimensions");
    req(src_media->duration > 899.0, "generated source duration is about 15 minutes");

    // Seek around the long source before building the stress project.  This is
    // a cheap stability signal that the long source itself decodes correctly.
    for(const double t : {0.0, 59.0, 60.0, 300.0, 600.0, 899.0}){
        PreviewRequest reqp; reqp.path = src_video; reqp.time = t; reqp.width = 160; reqp.height = 90;
        reqp.token = static_cast<std::uint64_t>(1000 + t); reqp.continuous = false; reqp.allow_prepared_cache = false;
        e.request_preview(reqp); Frame f; std::uint64_t tok = 0; err.clear();
        for(int i=0;i<500;++i){ if(e.poll_preview(tok, f, err) && tok == reqp.token) break; std::this_thread::sleep_for(std::chrono::milliseconds(4)); }
        if(f.rgb.empty() && !err.empty()) std::cerr << err;
        req(tok == reqp.token && !f.rgb.empty(), "preview frame from long source");
    }

    Project p = Project::make_default();
    p.name = "stability_long_9_layers.kora";
    p.settings.width = 160; p.settings.height = 90; p.settings.fps = 1.0; p.settings.preview_fps_limit = 1; p.settings.use_prepared_cache = false;
    p.auto_crossfade = true;
    const int mid = p.add_media(*src_media).id;
    const std::vector<int> tracks = ensure_video_tracks(p, 9);
    req(static_cast<int>(tracks.size()) == 9, "nine video tracks ready");

    constexpr int kSegments = 15;
    constexpr double kStep = 60.0;
    constexpr double kOverlap = 10.0;
    for(int layer = 0; layer < 9; ++layer){
        Track* track = p.track_by_id(tracks[layer]);
        req(track != nullptr, "valid stress track");
        track->opacity = std::clamp(1.0 - layer * 0.06, 0.45, 1.0);
        for(int seg = 0; seg < kSegments; ++seg){
            const double start = seg * kStep;
            const bool last = seg == kSegments - 1;
            const double duration = last ? kStep : (kStep + kOverlap);
            Clip& clip = p.add_clip(mid, tracks[layer], start, start, duration);
            clip.opacity = std::clamp(1.0 - layer * 0.04, 0.55, 1.0);
            clip.fade_in = seg == 0 ? 0.0 : kOverlap;
            clip.fade_out = last ? 0.0 : kOverlap;
            if(layer == 0 && (seg == 0 || seg == kSegments/2 || seg == kSegments-1)){
                clip.video_effects.push_back(make_anime_restore_instance(p.next_id()));
            }
        }
    }
    req(p.clips.size() == 9u * static_cast<unsigned>(kSegments), "stress project has 9 layers and many clips");
    req(p.duration() >= 900.0, "stress project duration is 15 minutes");
    req(p.save(path_to_utf8(project_path)), "save stability project for inspection");

    // Render a compact two-event-per-track scenario all the way through. It
    // still exercises nine simultaneous layers, overlap fades and Anime Restore,
    // but stays practical even in an unoptimized Debug build.
    Project short_project = Project::make_default();
    short_project.settings.width = 160; short_project.settings.height = 90;
    short_project.settings.fps = 1.0; short_project.settings.preview_fps_limit = 1;
    short_project.settings.use_prepared_cache = false; short_project.auto_crossfade = true;
    const int short_mid = short_project.add_media(*src_media).id;
    const std::vector<int> short_tracks = ensure_video_tracks(short_project, 9);
    for(int layer = 0; layer < 9; ++layer){
        Track* track = short_project.track_by_id(short_tracks[layer]);
        req(track != nullptr, "valid short stress track");
        track->opacity = std::clamp(1.0 - layer * 0.06, 0.45, 1.0);
        Clip& first = short_project.add_clip(short_mid, short_tracks[layer], 0.0, 0.0, 12.0);
        first.fade_out = 2.0;
        first.opacity = std::clamp(1.0 - layer * 0.04, 0.55, 1.0);
        Clip& second = short_project.add_clip(short_mid, short_tracks[layer], 10.0, 10.0, 10.0);
        second.fade_in = 2.0;
        second.opacity = first.opacity;
        if(layer == 0) first.video_effects.push_back(make_anime_restore_instance(short_project.next_id()));
    }
    req(short_project.clips.size() == 18u, "short stability subset has 9 layers and overlap transitions");
    req(short_project.duration() >= 20.0, "short stability subset duration");
    std::atomic<double> short_progress = 0.0;
    err.clear();
    const bool short_rendered_ok = e.render_project(short_project, short_out_video, &short_progress, &err);
    if(!short_rendered_ok && !err.empty()) std::cerr << err << "\n";
    req(short_rendered_ok, "render short stability subset");
    req(fs::exists(short_out_video_path) && fs::file_size(short_out_video_path) > 1000, "short stability render exists");

    auto rendered = e.probe(short_out_video, &err);
    if(!rendered && !err.empty()) std::cerr << err;
    req(rendered.has_value(), "probe rendered short stability output");
    req(rendered->has_video && rendered->width == 160 && rendered->height == 90, "rendered short stability output dimensions");
    req(rendered->duration > 19.0, "rendered short stability output duration");

    for(const double t : {0.0, 10.0, 11.0, 19.0}){
        PreviewRequest reqp; reqp.path = short_out_video; reqp.time = t; reqp.width = 160; reqp.height = 90;
        reqp.token = static_cast<std::uint64_t>(2000 + t); reqp.continuous = false; reqp.allow_prepared_cache = false;
        e.request_preview(reqp); Frame f; std::uint64_t tok = 0; err.clear();
        for(int i=0;i<600;++i){ if(e.poll_preview(tok, f, err) && tok == reqp.token) break; std::this_thread::sleep_for(std::chrono::milliseconds(4)); }
        if(f.rgb.empty() && !err.empty()) std::cerr << err;
        req(tok == reqp.token && !f.rgb.empty(), "decode rendered short stability output frame");
    }

    // Launch the complete 15-minute scenario, observe real export progress and
    // then cancel it explicitly. This verifies long-timeline planning/process
    // stability and cancellation without requiring a full 15-minute CPU render.
    std::atomic<double> long_progress = 0.0;
    std::atomic<bool> cancel_long = false;
    std::string long_err;
    bool long_render_ok = false;
    std::thread long_render_thread([&]{ long_render_ok = e.render_project(p, out_video, &long_progress, &long_err, nullptr, &cancel_long); });
    bool long_started = false;
    for(int i=0;i<250;++i){
        if(long_progress.load() > 0.051){ long_started = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    cancel_long = true;
    long_render_thread.join();
    if(!long_err.empty()) std::cerr << long_err << "\n";
    req(long_started, "long stability render begins making progress");
    req(!long_render_ok && long_err == "Render cancelled", "long stability render cancels cleanly");

    std::cout << "KoraLo long-project stability test passed\n";
    std::cout << "Project:      " << path_to_utf8(project_path) << "\n";
    std::cout << "Source:       " << src_video << "\n";
    std::cout << "Short render: " << short_out_video << "\n";
    return 0;
}
