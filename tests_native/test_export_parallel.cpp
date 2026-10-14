#include "media/export_parallelism.hpp"
#include "media/media_engine.hpp"
#include "platform/process.hpp"
#include "platform/app_paths.hpp"

#include <algorithm>
#include <atomic>
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

static void check(bool result, const char* why) {
    if (!result) throw std::runtime_error(why);
}
static void workers_override(const char* count) {
#ifdef _WIN32
    _putenv_s("KORALO_EXPORT_FRAME_WORKERS", count);
#else
    setenv("KORALO_EXPORT_FRAME_WORKERS", count, 1);
#endif
}
static Frame read_preview(MediaEngine& engine, const std::string& file,
                          double time, std::uint64_t token, int width, int height) {
    PreviewRequest request;
    request.path = file;
    request.time = time;
    request.width = width;
    request.height = height;
    request.token = token;
    request.allow_prepared_cache = false;
    engine.request_preview(request);
    std::string error;
    Frame frame;
    std::uint64_t received = 0;
    for (int i=0; i<400; ++i) {
        if (engine.poll_preview(received, frame, error) && received == token) {
            check(!frame.rgb.empty(), "decoded frame missing");
            return frame;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("parallel export preview comparison timed out: " + error);
}

int main() {
    // Property test: worker completions may be out of order; future consumers
    // still observe exact submission order, and concurrency never exceeds cap.
    {
        OrderedFrameWorkers pool(4);
        std::atomic<int> active{0}, maximum{0};
        std::vector<std::future<OrderedFrameWorkers::Pixels>> frames;
        for (int i=0;i<32;++i) {
            frames.push_back(pool.submit([&,i] {
                const int now = active.fetch_add(1) + 1;
                maximum.store(std::max(maximum.load(), now));
                std::this_thread::sleep_for(std::chrono::milliseconds(i%2 ? 9 : 13));
                active.fetch_sub(1);
                return OrderedFrameWorkers::Pixels{static_cast<std::uint8_t>(i)};
            }));
        }
        for (int i=0;i<32;++i) {
            const auto pixels = frames[static_cast<std::size_t>(i)].get();
            check(pixels.size()==1 && pixels[0]==i, "frame order corrupted");
        }
        check(maximum.load()>=2 && maximum.load()<=4, "frame pool did not use bounded parallelism");
        check(active.load()==0, "frame tasks still active");
    }
    {
        OrderedFrameWorkers serial(1);
        auto value = serial.submit([] {return OrderedFrameWorkers::Pixels{42};});
        check(value.get()[0]==42, "serial fallback failed");
        OrderedFrameWorkers pool(2);
        auto failure = pool.submit([]() -> OrderedFrameWorkers::Pixels { throw std::runtime_error("expected failure"); });
        bool caught=false;
        try { (void)failure.get(); } catch (const std::runtime_error&) { caught=true; }
        check(caught, "worker exception not propagated");
    }
    workers_override("1");
    check(export_frame_worker_count(160u*90u*4u)==1, "serial override ignored");
    workers_override("4");
    check(export_frame_worker_count(160u*90u*4u)==4, "parallel override ignored");
    check(export_frame_worker_count(3840u*2160u*4u)<=4, "4K memory cap ignored");

    // Set KORALO_EXPORT_BENCH_SIZE=medium for a 640x360 Anime V45 A/B run.
    const bool medium = std::getenv("KORALO_EXPORT_BENCH_SIZE") &&
                        std::string(std::getenv("KORALO_EXPORT_BENCH_SIZE")) == "medium";
    const int test_width = medium ? 640 : 160;
    const int test_height = medium ? 360 : 90;
    MediaEngine engine;
    if (!engine.ready()) { std::cout << "SKIP: ffmpeg unavailable\n"; return 77; }
    TemporaryFile reserved(temporary_directory(), ".parallel-test");
    check(reserved.valid(), "no temporary directory");
    const fs::path root = reserved.release();
    std::error_code ec;
    fs::remove(root, ec);
    fs::create_directories(root, ec);
    check(!ec, "cannot create temporary test directory");
    struct Cleanup { fs::path dir; ~Cleanup(){std::error_code ec;fs::remove_all(dir,ec);} } cleanup{root};
    const std::string source = path_to_utf8(root/"source.mp4");
    const std::string serial = path_to_utf8(root/"serial.mp4");
    const std::string parallel = path_to_utf8(root/"parallel.mp4");
    const auto generated=run_process(engine.ffmpeg_path(), {
        "-nostdin","-v","error","-y","-f","lavfi","-i",
        "testsrc2=size=" + std::to_string(test_width) + "x" + std::to_string(test_height) + ":rate=10","-t","1.8","-c:v","libx264",
        "-preset","ultrafast","-pix_fmt","yuv420p",source});
    check(generated.started && generated.exit_code==0, "could not make source video");
    std::string error;
    const auto media=engine.probe(source,&error);
    check(media.has_value(), "source probe failed");
    Project project=Project::make_default();
    project.settings.width=test_width;
    project.settings.height=test_height;
    project.settings.fps=10;
    const int id=project.add_media(*media).id;
    int track_id=0;
    for(const auto& t:project.tracks)
        if(t.kind==TrackKind::Video) {track_id=t.id;break;}
    check(track_id!=0, "no video track");
    Clip& clip=project.add_clip(id,track_id,0,0,1.8);
    clip.video_effects.push_back(make_effect_instance("koralo.anime_restore_v45", project.next_id()));
    RenderOptions options;
    options.width=test_width;
    options.height=test_height;
    options.fps=10;
    options.use_gpu=false;
    std::atomic<double> progress{0.0};
    workers_override("1");
    const auto start_serial=std::chrono::steady_clock::now();
    check(engine.render_project(project,serial,&progress,&error,&options),"serial export failed");
    const auto serial_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start_serial).count();
    workers_override("4");
    progress=0;
    error.clear();
    const auto start_parallel=std::chrono::steady_clock::now();
    check(engine.render_project(project,parallel,&progress,&error,&options),"parallel export failed");
    const auto parallel_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start_parallel).count();
    check(progress.load()>=.999, "parallel export did not finish");
    {
        std::ifstream audit(parallel+".render-control.jsonl");
        check(audit.good(),"export control audit missing");
        const std::string events((std::istreambuf_iterator<char>(audit)),{});
        check(events.find("\"event\":\"verify_ok\"")!=std::string::npos,
              "full video verification not logged");
        check(events.find("\"event\":\"render_success\"")!=std::string::npos,
              "successful publish not logged");
    }
    for (int i=0;i<3;++i) {
        const auto left=read_preview(engine,serial,0.2+i*0.5,200+i*2,test_width,test_height);
        const auto right=read_preview(engine,parallel,0.2+i*0.5,201+i*2,test_width,test_height);
        check(left.width==right.width && left.height==right.height, "preview dimensions differ");
        check(left.rgb==right.rgb, "serial/parallel exported pixels differ");
    }
    std::cout << "PASS: ordered frame workers, cancellation-safe errors, memory budget, exact pixel parity\n";
    std::cout << "serial_ms=" << serial_ms << " parallel_ms=" << parallel_ms << " size=" << test_width << "x" << test_height << '\n';
    return 0;
}
