#include "platform/process.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace koralo;
static void req(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
int main(){
 const std::string ffmpeg=find_executable_on_path("ffmpeg");
 if(ffmpeg.empty()){std::cout<<"SKIP ffmpeg missing\n";return 77;}
 const auto output=std::filesystem::temp_directory_path() / (
     "koralo-pipe-encoder-"+
     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
     ".mkv");
 struct Cleanup { std::filesystem::path file; ~Cleanup(){std::error_code ec;std::filesystem::remove(file,ec);} } cleanup{output};
 std::error_code ec;std::filesystem::remove(output,ec);
 {
   ReadPipeProcess encoder;std::string err;
   req(encoder.start(ffmpeg,{"-nostdin","-v","error","-y","-f","rawvideo","-pixel_format","rgba","-video_size","32x32","-framerate","10","-i","pipe:0","-an","-c:v","ffv1","-level","3","-pix_fmt","bgra",path_to_utf8(output)},&err,false,true),"start encoder");
   std::vector<unsigned char> pixels(32*32*4,255);
   for(int i=0;i<16;++i){
     for(std::size_t p=0;p<pixels.size();p+=4){pixels[p]=static_cast<unsigned char>(i*9);pixels[p+1]=40;pixels[p+2]=170;pixels[p+3]=255;}
     req(encoder.write_stdin_chunk({reinterpret_cast<const char*>(pixels.data()),pixels.size()},&err),"write raw input frame");
   }
   encoder.close_stdin();
   for(int i=0;i<500 && encoder.running();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
   req(!encoder.running() && encoder.exit_code()==0,"FFV1 encoder exits successfully");
 }
 req(std::filesystem::file_size(output)>1000,"encoded video is non-empty");
 auto ffprobe=find_executable_on_path("ffprobe");
 if(!ffprobe.empty()){
   const auto r=run_process(ffprobe,{"-v","error","-select_streams","v:0","-count_frames","-show_entries","stream=nb_read_frames","-of","default=noprint_wrappers=1:nokey=1",path_to_utf8(output)});
   req(r.exit_code==0 && r.output.find("16")!=std::string::npos,"preserved 16 frame count");
 }
 std::filesystem::remove(output,ec);
 // On broken child stdin KoraLo must never be killed by SIGPIPE.
 {
  ReadPipeProcess broken;std::string err;
  req(broken.start(ffmpeg,{"-v","quiet","-f","definitely_invalid","-i","pipe:0","-f","null","-"},&err,false,true),"start failure fixture");
  for(int i=0;i<200 && broken.running();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  req(!broken.running(),"invalid encoder exits early");
  std::array<char,4096> bytes{};
  const bool ok=broken.write_stdin_chunk({bytes.data(),bytes.size()},&err);
  req(!ok,"broken encoder stdin is reported, not fatal");
  broken.terminate();
 }
 std::cout<<"Continuous encoder stdin/EOF/failed-child regression passed\n";
}
