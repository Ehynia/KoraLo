#include "media/render_control.hpp"
#include "media/export_parallelism.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace koralo;
namespace fs = std::filesystem;
static void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}

int main() {
    const auto temp=fs::temp_directory_path() /
        ("koralo-render-control-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);
    struct Clean {fs::path dir;~Clean(){std::error_code ec;fs::remove_all(dir,ec);}} clean{temp};
    RenderControlLive status;
    const auto file=temp/"ready.mp4";
    {
        RenderControlSession session(file,&status);
        session.update_workers(3,8,140,1024);
        auto snap=render_control_snapshot(status);
        check(snap.state==1 && snap.workers==3 && snap.worker_cap==8 && snap.process_cpu_percent==140,"live render status");
        session.finish(true,"test");
    }
    check(status.state==3 && status.workers==0,"finished status");
    std::ifstream log(file.string()+".render-control.jsonl");
    std::string text((std::istreambuf_iterator<char>(log)),{});
    check(text.find("render_start")!=std::string::npos && text.find("resource_sample")!=std::string::npos &&
          text.find("render_success")!=std::string::npos,"structured diagnostic logs");
    {
        OrderedFrameWorkers workers(4);
        workers.set_active_limit(1);
        std::atomic<int> active{0},max_active{0};
        auto work=[&]{
            const int running=active.fetch_add(1)+1;
            int old=max_active.load();
            while(old<running && !max_active.compare_exchange_weak(old,running)){}
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            active.fetch_sub(1);
            return OrderedFrameWorkers::Pixels{42};
        };
        std::vector<std::future<OrderedFrameWorkers::Pixels>> jobs;
        for(int i=0;i<8;++i)jobs.push_back(workers.submit(work));
        for(auto& j:jobs)check(j.get()[0]==42,"ordered result");
        check(max_active.load()==1,"single active worker limit");
        max_active=0;
        workers.set_active_limit(4);
        jobs.clear();
        for(int i=0;i<16;++i)jobs.push_back(workers.submit(work));
        for(auto& j:jobs)check(j.get()[0]==42,"parallel result");
        check(max_active.load()>=2 && max_active.load()<=4,"active limit changed dynamically");
    }
    AdaptiveRenderWorkers adaptive(4,320*240*4,false);
    check(adaptive.active()>=1 && adaptive.active()<=4,"adaptive bounds");
    AdaptiveRenderWorkers fixed(4,320*240*4,true);
    check(fixed.active()==4,"manual override remains fixed");

    const auto ffmpeg=find_executable_on_path("ffmpeg");
    const auto ffprobe=find_executable_on_path("ffprobe");
    if(ffmpeg.empty()||ffprobe.empty()){
        std::cout<<"Worker and log checks pass; SKIP FFmpeg verification\n";
        return 0;
    }
    const auto generated=run_process(ffmpeg,{"-nostdin","-v","error","-y","-f","lavfi",
        "-i","testsrc2=size=96x64:rate=10","-f","lavfi","-i","sine=frequency=440:sample_rate=44100",
        "-t","1","-c:v","mpeg4","-q:v","4","-c:a","aac",path_to_utf8(file)});
    check(generated.started&&generated.exit_code==0,"generate verification fixture");
    std::string err;
    check(verify_rendered_video(ffprobe,ffmpeg,file,96,64,1.0,nullptr,&err),"valid video verifies");
    check(verify_rendered_video(ffprobe,ffmpeg,file,96,64,1.0,nullptr,&err,true),"audio stream verified");
    check(!verify_rendered_video(ffprobe,ffmpeg,file,192,64,1.0,nullptr,&err),"bad resolution rejected");
    check(err.find("resolution mismatch")!=std::string::npos,"resolution diagnostic");
    check(!verify_rendered_video(ffprobe,ffmpeg,file,96,64,9.0,nullptr,&err),"bad duration rejected");
    const auto invalid=temp/"broken.mp4";
    {
        std::ofstream damaged(invalid,std::ios::binary);
        damaged<<"not a valid exported video";
    }
    check(!verify_rendered_video(ffprobe,ffmpeg,invalid,96,64,1.0,nullptr,&err),"corrupt file rejected");
    std::atomic<bool> cancel{true};
    check(!verify_rendered_video(ffprobe,ffmpeg,file,96,64,1.0,&cancel,&err),"cancel stops verify");
    check(fs::file_size(file)>1000,"original remains intact after failures");
    std::cout<<"Render control: live stats/logs, adaptive pool, full decode and corrupt video rejection PASS\n";
}
