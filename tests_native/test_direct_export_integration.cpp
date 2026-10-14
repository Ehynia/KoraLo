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
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void graph_override(bool value){
#ifdef _WIN32
  _putenv_s("KORALO_EXPORT_FORCE_GENERIC",value?"1":"");
#else
  if(value)setenv("KORALO_EXPORT_FORCE_GENERIC","1",1);
  else unsetenv("KORALO_EXPORT_FORCE_GENERIC");
#endif
}
static bool log_contains(const fs::path& path,const std::string& pattern){
  std::ifstream input(path,std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),{}).find(pattern)!=std::string::npos;
}
int main(){
 try{
  MediaEngine engine;engine.set_gpu_enabled(false);
  if(!engine.ready())return 77;
  const bool hd=std::getenv("KORALO_DIRECT_TEST_HD")!=nullptr;
  const int width=hd?1280:320, height=hd?720:180;
  const fs::path base=temporary_directory()/(hd?"koralo-direct-benchmark-hd":"koralo-direct-benchmark");
  std::error_code ec;fs::create_directories(base,ec);
  const fs::path fixture=base/"single-source.mp4";
  const auto fixture_result=run_process(engine.ffmpeg_path(),{"-nostdin","-hide_banner","-v","error","-y",
      "-f","lavfi","-i","testsrc2=s="+std::to_string(width)+"x"+std::to_string(height)+":r=30:d=4",
      "-f","lavfi","-i","sine=frequency=470:duration=4",
      "-c:v","libx264","-preset","ultrafast","-pix_fmt","yuv420p","-threads","2",
      "-c:a","aac","-shortest",path_to_utf8(fixture)});
  check(fixture_result.exit_code==0,"fixture generation");
  std::string error;const auto media=engine.probe(path_to_utf8(fixture),&error);
  check(media.has_value(),"fixture probe");
  Project project;project.settings.width=width;project.settings.height=height;
  project.settings.fps=30;project.settings.sample_rate=48000;
  const int id=project.add_media(*media).id;
  check(project.place_media(id,0)>0,"place source video and audio");
  check(project.clips.size()==2,"paired A/V media");
  RenderOptions options;options.width=width;options.height=height;options.fps=30;options.use_gpu=false;
  options.verify_output=true;
  const auto run=[&](const char* filename,bool generic){
    const auto out=base/filename;
    graph_override(generic);
    std::atomic<double> progress{0};error.clear();
    const auto start=std::chrono::steady_clock::now();
    const bool success=engine.render_project(project,path_to_utf8(out),&progress,&error,&options,nullptr);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    check(success,"export failed");check(progress.load()>=.99,"export progress");
    check(fs::exists(out),"export exists");
    auto logfile=out;logfile+=fs::path(".render-control.jsonl");
    check(log_contains(logfile,"verify_ok"),"output must pass verification");
    if(!generic)check(log_contains(logfile,"direct_single_source"),"direct path selected");
    else check(!log_contains(logfile,"direct_single_source"),"forced general path selected");
    std::cout<<(generic?"generic":"direct")<<" export seconds="<<seconds<<"\n";
    return out;
  };
  const auto direct=run("direct.mp4",false);
  const auto generic=run("generic.mp4",true);
  graph_override(false);
  // Compare both decoded outputs in a deterministic filtergraph. Both files
  // are lossy H.264 encodes, so byte-for-byte equality is not expected.
  const auto compare=run_process(engine.ffmpeg_path(),{"-nostdin","-v","info","-i",path_to_utf8(direct),
    "-i",path_to_utf8(generic),"-lavfi","[0:v][1:v]psnr=stats_file=-","-f","null","-"});
  check(compare.exit_code==0,"decode and compare output videos");
  const auto avg=compare.output.rfind("average:");
  check(avg!=std::string::npos,"PSNR results should be reported");
  const double psnr=std::stod(compare.output.substr(avg+8));
  std::cout<<"Video output PSNR="<<psnr<<" dB\n";
  check(psnr>35.0,"direct and composed video too different");
  std::cout<<"Direct and general exports decoded and verified successfully\n";
  return 0;
 }catch(const std::exception& ex){graph_override(false);std::cerr<<ex.what()<<'\n';return 1;}
}
