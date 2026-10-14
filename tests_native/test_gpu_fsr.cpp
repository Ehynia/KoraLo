#include "media/fsr_upscaler.hpp"
#include "media/fsr_cuda.hpp"
#include "media/native_cuda.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace koralo;
void check(bool condition,const char* what){if(!condition)throw std::runtime_error(what);}
int main(){try{
    Frame src;src.width=320;src.height=180;
    src.rgb.resize(320*180*3);src.alpha.resize(320*180);
    for(int y=0;y<180;++y)for(int x=0;x<320;++x){
        auto i=static_cast<size_t>(y)*320+x;
        src.rgb[i*3]=static_cast<unsigned char>((x+y)%256);
        src.rgb[i*3+1]=static_cast<unsigned char>((x*3+y)%256);
        src.rgb[i*3+2]=static_cast<unsigned char>((x+y*5)%256);
        src.alpha[i]=static_cast<unsigned char>((x+y)%256);
    }
    const Frame cpu=upscale_fsr1(src,640,360,.7f,nullptr,false);
    const Frame optional_gpu=upscale_fsr1(src,640,360,.7f,nullptr,true);
    check(cpu.width==optional_gpu.width&&cpu.height==optional_gpu.height,"GPU/CPU output dimensions");
    check(cpu.rgb.size()==optional_gpu.rgb.size()&&cpu.alpha.size()==optional_gpu.alpha.size(),"GPU/CPU output buffers");
    double mean=0;
    for(size_t i=0;i<cpu.rgb.size();++i)
        mean+=std::abs(int(cpu.rgb[i])-int(optional_gpu.rgb[i]));
    mean/=cpu.rgb.size();
    check(mean<3.0,"CUDA FSR and CPU FSR visual parity (MAE < 3)");
    check(cpu.alpha==optional_gpu.alpha,"CUDA FSR alpha parity");
    check(!native_cuda_enabled(),"GPU is opt-in by default");
    {NativeCudaScope outer(true);check(native_cuda_enabled(),"GPU scope enabled");
     {NativeCudaScope inner(false);check(!native_cuda_enabled(),"nested GPU scope off");}
     check(native_cuda_enabled(),"GPU scope restored");}
    check(!native_cuda_enabled(),"GPU scope reset after export");
    if(fsr_cuda_available()){
        Frame actual;
        check(try_upscale_fsr1_cuda(src,640,360,.7f,actual),"CUDA device must execute FSR kernels");
        check(actual.rgb.size()==640u*360u*3u,"CUDA output is complete");
    }
    std::cout<<"PASS: CUDA optional FSR; backend="<<(fsr_cuda_available()?"NVIDIA CUDA":"CPU fallback")
             <<"; GPU/CPU MAE="<<mean<<"\n";
    return 0;
 }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}}
