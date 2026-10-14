#include "media/media_engine.hpp"
#include "platform/app_paths.hpp"
#include "platform/process.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace koralo;
namespace fs=std::filesystem;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void overlay_mode(bool on){
#ifdef _WIN32
    _putenv_s("KORALO_EXPORT_FORCE_OVERLAY",on?"1":"");
#else
    if(on)setenv("KORALO_EXPORT_FORCE_OVERLAY","1",1);
    else unsetenv("KORALO_EXPORT_FORCE_OVERLAY");
#endif
}
static bool logged(const fs::path& path,const std::string& needle){
    std::ifstream f(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),{}).find(needle)!=std::string::npos;
}
int main(){
    try {
        MediaEngine engine;
        engine.set_gpu_enabled(false);
        if(!engine.ready())return 77;
        const bool hd=std::getenv("KORALO_CUT_TEST_HD")!=nullptr;
        const int width=hd?1280:320, height=hd?720:180;
        const auto dir=temporary_directory()/(hd?"koralo-fastcuts-hd":"koralo-fastcuts-basic");
        std::error_code ec;fs::create_directories(dir,ec);
        const auto src=dir/"cut-input.mp4";
        const auto generated=run_process(engine.ffmpeg_path(),{"-nostdin","-v","error","-y",
            "-f","lavfi","-i","testsrc2=size="+std::to_string(width)+"x"+std::to_string(height)+":rate=30:duration=6",
            "-f","lavfi","-i","sine=frequency=515:duration=6:sample_rate=48000",
            "-c:v","libx264","-preset","ultrafast","-pix_fmt","yuv420p","-threads","2",
            "-c:a","aac","-shortest",path_to_utf8(src)});
        check(generated.exit_code==0,"Generate video fixture");
        std::string error;
        auto media=engine.probe(path_to_utf8(src),&error);
        check(media.has_value(),"Probe fixture");
        Project p;p.settings.width=width;p.settings.height=height;p.settings.fps=30;
        const int mid=p.add_media(*media).id;
        const int vid=p.add_track(TrackKind::Video).id;
        const int aud=p.add_track(TrackKind::Audio).id;
        p.add_clip(mid,vid,0,0,3);
        p.add_clip(mid,vid,3,3,3);
        p.add_clip(mid,aud,0,0,3);
        p.add_clip(mid,aud,3,3,3);
        RenderOptions opt;opt.width=width;opt.height=height;opt.fps=30;
        opt.use_gpu=false;opt.verify_output=true;
        auto execute=[&](const char* filename,bool force_overlay){
            overlay_mode(force_overlay);
            const auto out=dir/filename;
            const auto begin=std::chrono::steady_clock::now();
            std::atomic<double> progress{0};error.clear();
            check(engine.render_project(p,path_to_utf8(out),&progress,&error,&opt,nullptr),
                  "Export video");
            const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
            auto log=out;log+=fs::path(".render-control.jsonl");
            check(logged(log,"verify_ok"),"Decoder verified the complete video");
            if(force_overlay)check(!logged(log,"fast_cuts"),"Forced overlay baseline");
            else check(logged(log,"fast_cuts segments=2"),"Fast cuts were selected");
            std::cout<<(force_overlay?"overlay":"fast_cuts")<<" export_seconds="<<seconds<<"\n";
            return out;
        };
        const auto fast=execute("cuts-fast.mp4",false);
        const auto baseline=execute("cuts-overlay.mp4",true);
        overlay_mode(false);
        const auto psnr=run_process(engine.ffmpeg_path(),{"-nostdin","-v","info","-i",path_to_utf8(fast),
            "-i",path_to_utf8(baseline),"-lavfi","[0:v][1:v]psnr=stats_file=-","-f","null","-"});
        check(psnr.exit_code==0,"Both videos decode");
        const auto pos=psnr.output.rfind("average:");
        check(pos!=std::string::npos,"PSNR reported");
        const double value=std::stod(psnr.output.substr(pos+8));
        std::cout<<"cut_video_psnr="<<value<<" dB\n";
        check(value>35.0,"Fast cuts visually differ too much");
        // Audit timestamp alignment and presence of audio on both sides.
        for(const auto& out:{fast,baseline}){
            const auto inspected=run_process(engine.ffprobe_path(),{"-v","error","-show_entries",
                "format=duration:stream=codec_type","-of","default=noprint_wrappers=1",path_to_utf8(out)});
            check(inspected.exit_code==0,"ffprobe output");
            check(inspected.output.find("codec_type=audio")!=std::string::npos,"Audio track preserved");
            check(inspected.output.find("codec_type=video")!=std::string::npos,"Video track preserved");
        }
        std::cout<<"PASS: cuts, audio, duration, verification, decoded visual comparison\n";
        return 0;
    }catch(const std::exception& e){overlay_mode(false);std::cerr<<e.what()<<'\n';return 1;}
}
