#include "media/fsr_upscaler.hpp"
#include "media/media_engine.hpp"
#include "media/native_effects.hpp"
#include "platform/app_paths.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace koralo;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    Frame flat;flat.width=16;flat.height=12;flat.rgb.assign(16*12*3,128);flat.alpha.assign(16*12,128);
    for(float sharp:{0.f,.5f,1.f}){
        auto up=upscale_fsr1(flat,64,48,sharp);check(up.width==64&&up.height==48,"FSR output size");
        check(std::all_of(up.rgb.begin(),up.rgb.end(),[](auto c){return std::abs(int(c)-128)<=1;}),"FSR preserves flat tones");
        check(std::all_of(up.alpha.begin(),up.alpha.end(),[](auto c){return c==128;}),"FSR alpha preserved");
    }
    Frame edge=flat;for(int y=0;y<edge.height;++y)for(int x=0;x<edge.width;++x)for(int c=0;c<3;++c)edge.rgb[(y*edge.width+x)*3+c]=x>=y?220:25;
    auto up=upscale_fsr1(edge,32,24,0);check(*std::min_element(up.rgb.begin(),up.rgb.end())>=25&&*std::max_element(up.rgb.begin(),up.rgb.end())<=220,"EASU anti-ringing bounds");
    check(upscale_fsr1(edge,32,24,1).rgb!=up.rgb,"RCAS control changes detail");
    std::atomic<bool> cancelled{true};check(upscale_fsr1(edge,32,24,.5f,&cancelled).rgb.empty(),"FSR responds to cancellation");
    bool rejected=false;try{upscale_fsr1(flat,8,6);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"FSR rejects downscaling");
    auto paper=make_effect_instance("koralo.anime_paper_texture",1);
    set_effect_numeric(paper,"texture",0);set_effect_numeric(paper,"warmth",.3);
    Frame warm=flat;check(apply_native_effect(warm,paper,0)&&warm.rgb!=flat.rgb,"paper warmth independent of grain strength");
    set_effect_numeric(paper,"texture",.15);set_effect_numeric(paper,"paper_fibers",.5);set_effect_numeric(paper,"paper_stains",.5);
    Frame first=flat,second=flat;apply_native_effect(first,paper,0);apply_native_effect(second,paper,2);check(first.rgb==second.rgb,"paper is stable over time by default");
    set_effect_numeric(paper,"paper_seed",42);second=flat;apply_native_effect(second,paper,0);check(first.rgb!=second.rgb,"paper seed changes pattern");
    set_effect_numeric(paper,"paper_motion",12);first=flat;second=flat;apply_native_effect(first,paper,0);apply_native_effect(second,paper,1);check(first.rgb!=second.rgb,"opt-in animated paper");
    MediaEngine engine;engine.set_gpu_enabled(false);
    if(!engine.ready()){std::cout<<"FSR and paper tests passed; SKIP export (no FFmpeg)\n";return 77;}
    TemporaryFile input(temporary_directory(),".mkv"),output(temporary_directory(),".mp4");std::string error;
    auto made=run_process(engine.ffmpeg_path(),{"-v","error","-y","-f","lavfi","-i","testsrc2=s=64x36:r=12:d=1","-f","lavfi","-i","sine=frequency=440:duration=1","-c:v","ffv1","-c:a","pcm_s16le","-threads","1",path_to_utf8(input.path())});
    check(made.exit_code==0,"create AV export fixture");
    auto item=engine.probe(path_to_utf8(input.path()),&error);check(item.has_value(),"probe fixture");
    Project project;project.settings.width=64;project.settings.height=36;project.settings.fps=12;
    const int mid=project.add_media(*item).id;project.place_media(mid,0);
    project.video_output_effects.push_back(paper);
    RenderOptions options;options.width=128;options.height=72;options.fps=12;options.use_gpu=false;options.fsr_upscale=true;
    std::atomic<double> progress{0};cancelled=false;
    auto task=std::async(std::launch::async,[&]{return engine.render_project(project,path_to_utf8(output.path()),&progress,&error,&options,&cancelled);});
    double last=0;bool intermediate=false;
    while(task.wait_for(std::chrono::milliseconds(3))!=std::future_status::ready){const double value=progress.load();check(value+1e-8>=last&&value<=1,"monotonic export progress");intermediate|=value>0&&value<1;last=value;}
    if(!task.get())throw std::runtime_error(error);
    check(intermediate&&progress==1,"progress reports intermediate work and completion");
    const auto result=engine.probe(path_to_utf8(output.path()),&error);
    check(result&&result->width==128&&result->height==72&&result->has_audio&&result->duration>.9,"FSR export retains dimensions audio and duration");
    // Regression: an input with only ONE 12-fps frame and a 3-second timeline
    // must not abort with "FSR/native source decode failed at frame 13".
    // FFmpeg must clone the final frame until the requested frame count.
    {
        TemporaryFile one(temporary_directory(),".mkv"),extended(temporary_directory(),".mp4");
        auto fixture=run_process(engine.ffmpeg_path(),{"-nostdin","-v","error","-y","-f","lavfi",
            "-i","color=c=blue:s=64x36:r=12","-frames:v","1","-c:v","ffv1",path_to_utf8(one.path())});
        check(fixture.exit_code==0,"one-frame FSR test fixture");
        auto short_media=engine.probe(path_to_utf8(one.path()),&error);
        check(short_media&&short_media->has_video,"one-frame source is readable");
        Project longer;longer.settings.width=64;longer.settings.height=36;longer.settings.fps=12;
        const int sid=longer.add_media(*short_media).id;
        const int tid=longer.add_track(TrackKind::Video).id;
        longer.add_clip(sid,tid,0,0,3.0);
        std::atomic<double> p2{0};error.clear();
        check(engine.render_project(longer,path_to_utf8(extended.path()),&p2,&error,&options,nullptr),
              "FSR extended short source should succeed");
        auto verified=engine.probe(path_to_utf8(extended.path()),&error);
        check(verified&&verified->width==128&&verified->height==72&&verified->duration>=2.9,
              "FSR preserves the extended clip duration");
    }

    const auto before=std::filesystem::file_size(output.path());cancelled=true;
    check(!engine.render_project(project,path_to_utf8(output.path()),&progress,&error,&options,&cancelled)&&error=="Render cancelled","cancelled export reports cancellation");
    check(std::filesystem::file_size(output.path())==before,"cancelled export preserves existing output");
#ifndef _WIN32
    // Simulate an encoder returning success while leaving its reserved output
    // empty. The continuous encoder must reject it before publishing the output.
    TemporaryFile wrapper(temporary_directory(),".sh");
    {std::ofstream script(wrapper.path());script<<"#!/bin/sh\nfor arg do\n  if [ \"$arg\" = '-pixel_format' ]; then exit 0; fi\ndone\nexec \"$KORALO_TEST_REAL_FFMPEG\" \"$@\"\n";}
    std::filesystem::permissions(wrapper.path(),std::filesystem::perms::owner_all);
    const auto prior_ffmpeg=environment_utf8("KORALO_FFMPEG"),prior_real=environment_utf8("KORALO_TEST_REAL_FFMPEG");
    setenv("KORALO_TEST_REAL_FFMPEG",engine.ffmpeg_path().c_str(),1);
    setenv("KORALO_FFMPEG",path_to_utf8(wrapper.path()).c_str(),1);
    MediaEngine broken;broken.set_gpu_enabled(false);cancelled=false;options.fsr_upscale=false;
    const bool broken_result=broken.render_project(project,path_to_utf8(output.path()),&progress,&error,&options,&cancelled);
    if(prior_ffmpeg.empty())unsetenv("KORALO_FFMPEG");else setenv("KORALO_FFMPEG",prior_ffmpeg.c_str(),1);
    if(prior_real.empty())unsetenv("KORALO_TEST_REAL_FFMPEG");else setenv("KORALO_TEST_REAL_FFMPEG",prior_real.c_str(),1);
    const bool rejected_broken_pipe = error.find("Native encoder stdin write failed")!=std::string::npos;
    const bool rejected_empty_output = error.find("Continuous native FFV1 output is invalid")!=std::string::npos;
    check(!broken_result&&(rejected_broken_pipe||rejected_empty_output),
          "empty successful encoder output is rejected before publishing");
    check(std::filesystem::file_size(output.path())==before,"invalid intermediate preserves existing output");
#endif
    std::cout<<"PASS: FSR EASU/RCAS, paper controls, AV export, progress, cancellation, invalid intermediate rejection\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
