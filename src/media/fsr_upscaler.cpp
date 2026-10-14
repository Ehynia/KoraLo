// AMD FSR 1 EASU/RCAS scalar CPU adaptation. See licenses/AMD-FSR1-MIT.txt.
// Reference: third_party/fsr1/ffx_fsr1.h (GPUOpen-Effects/FidelityFX-FSR).
#include "fsr_upscaler.hpp"
#include "fsr_cuda.hpp"
#include "export_parallelism.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <vector>
namespace koralo {
namespace {
template<class F> void parallel_rows(int height, F&& fn){
    const unsigned hc=export_inner_thread_budget ? export_inner_thread_budget : std::thread::hardware_concurrency();
    const int workers=std::min(height, int(std::clamp(hc?hc:4u,export_inner_thread_budget?1u:2u,16u)));
    if(height<64||workers<2){fn(0,height);return;}
    std::vector<std::thread> threads;threads.reserve(workers);
    for(int i=0;i<workers;++i){const int y0=height*i/workers,y1=height*(i+1)/workers;threads.emplace_back([&,y0,y1]{fn(y0,y1);});}
    for(auto& t:threads)t.join();
}
using Pixel=std::array<float,3>;
std::uint8_t byte(float v){return static_cast<std::uint8_t>(std::lround(std::clamp(v,0.f,1.f)*255));}
Pixel load(const Frame& f,int x,int y){
    const auto i=(std::size_t(std::clamp(y,0,f.height-1))*f.width+std::clamp(x,0,f.width-1))*3;
    return {f.rgb[i]/255.f,f.rgb[i+1]/255.f,f.rgb[i+2]/255.f};
}
float lum(Pixel c){return c[1]+.5f*(c[0]+c[2]);}
Frame easu(const Frame& in,int width,int height,const std::atomic<bool>* cancel){
    Frame out;out.width=width;out.height=height;out.rgb.resize(std::size_t(width)*height*3);
    if(!in.alpha.empty())out.alpha.resize(std::size_t(width)*height);
    constexpr int taps[12][2]={{0,-1},{1,-1},{-1,0},{0,0},{1,0},{2,0},{-1,1},{0,1},{1,1},{2,1},{0,2},{1,2}};
    parallel_rows(height,[&](int y0,int y1){
      for(int y=y0;y<y1;++y){
        if(cancel&&cancel->load())return;
        for(int x=0;x<width;++x){
            const float sx=(x+.5f)*in.width/width-.5f,sy=(y+.5f)*in.height/height-.5f;
            const int ix=int(std::floor(sx)),iy=int(std::floor(sy));const float px=sx-ix,py=sy-iy;
            Pixel colors[12];float l[12];for(int k=0;k<12;++k){colors[k]=load(in,ix+taps[k][0],iy+taps[k][1]);l[k]=lum(colors[k]);}
            float dx=0,dy=0,len=0;
            auto edge=[&](float w,float a,float b,float c,float d,float e){
                float gx=d-b,gy=e-a;
                dx+=gx*w;dy+=gy*w;
                const float lx=std::clamp(std::abs(gx)/std::max({std::abs(d-c),std::abs(c-b),1e-8f}),0.f,1.f);
                const float ly=std::clamp(std::abs(gy)/std::max({std::abs(e-c),std::abs(c-a),1e-8f}),0.f,1.f);
                len+=(lx*lx+ly*ly)*w;
            };
            edge((1-px)*(1-py),l[0],l[2],l[3],l[4],l[7]);
            edge(px*(1-py),l[1],l[3],l[4],l[5],l[8]);
            edge((1-px)*py,l[3],l[6],l[7],l[8],l[10]);
            edge(px*py,l[4],l[7],l[8],l[9],l[11]);
            const float d2=dx*dx+dy*dy;
            if(d2<1.f/32768){dx=1;dy=0;}else{const float inv=1/std::sqrt(d2);dx*=inv;dy*=inv;}
            len*=.5f;len*=len;
            const float stretch=1/std::max(std::abs(dx),std::abs(dy));
            const float ax=1+(stretch-1)*len,ay=1-.5f*len,lobe=.5f+(.21f-.5f)*len,clip=1/lobe;
            Pixel sum{};float weights=0;
            for(int k=0;k<12;++k){
                const float ox=taps[k][0]-px,oy=taps[k][1]-py;
                const float vx=(ox*dx+oy*dy)*ax,vy=(-ox*dy+oy*dx)*ay;
                const float distance=std::min(vx*vx+vy*vy,clip);
                float wb=.4f*distance-1,wa=lobe*distance-1;
                const float weight=(1.5625f*wb*wb-.5625f)*wa*wa;
                weights+=weight;for(int c=0;c<3;++c)sum[c]+=colors[k][c]*weight;
            }
            const auto index=std::size_t(y)*width+x;
            for(int c=0;c<3;++c){
                const float lo=std::min({colors[3][c],colors[4][c],colors[7][c],colors[8][c]});
                const float hi=std::max({colors[3][c],colors[4][c],colors[7][c],colors[8][c]});
                out.rgb[index*3+c]=byte(std::clamp(std::abs(weights)>1e-8f?sum[c]/weights:colors[3][c],lo,hi));
            }
            if(!out.alpha.empty()){
                auto alpha=[&](int xx,int yy){return in.alpha[std::size_t(std::clamp(yy,0,in.height-1))*in.width+std::clamp(xx,0,in.width-1)]/255.f;};
                out.alpha[index]=byte((1-py)*((1-px)*alpha(ix,iy)+px*alpha(ix+1,iy))+py*((1-px)*alpha(ix,iy+1)+px*alpha(ix+1,iy+1)));
            }
        }
      }
    });
    if(cancel&&cancel->load())return {};
    return out;
}
void rcas(Frame& frame,float strength,const std::atomic<bool>* cancel){
    if(strength<=0)return;
    const Frame in=frame;
    parallel_rows(frame.height,[&](int y0,int y1){
      for(int y=y0;y<y1;++y){
        if(cancel&&cancel->load())return;
        for(int x=0;x<frame.width;++x){
            const Pixel b=load(in,x,y-1),d=load(in,x-1,y),e=load(in,x,y),f=load(in,x+1,y),h=load(in,x,y+1);
            float lobe=-.1875f;
            for(int c=0;c<3;++c){
                const float lo=std::min({b[c],d[c],f[c],h[c]}),hi=std::max({b[c],d[c],f[c],h[c]});
                const float hit_min=std::min(lo,e[c])/std::max(4*hi,1e-8f);
                const float hit_max=(1-std::max(hi,e[c]))/std::min(4*lo-4,-1e-8f);
                lobe=std::max(lobe,std::max(-hit_min,hit_max));
            }
            lobe=std::min(lobe,0.f)*strength;
            const auto index=(std::size_t(y)*frame.width+x)*3;
            for(int c=0;c<3;++c)frame.rgb[index+c]=byte((e[c]+lobe*(b[c]+d[c]+f[c]+h[c]))/(1+4*lobe));
        }
      }
    });
    if(cancel&&cancel->load())frame={};
}
}
Frame upscale_fsr1(const Frame& input,int width,int height,float strength,const std::atomic<bool>* cancel,bool enable_cuda){
    if(input.width<1||input.height<1||width<input.width||height<input.height||
       std::size_t(width)*height>16*1024*1024||input.rgb.size()!=std::size_t(input.width)*input.height*3||
       (!input.alpha.empty()&&input.alpha.size()!=std::size_t(input.width)*input.height)||!std::isfinite(strength))
        throw std::invalid_argument("Invalid FSR frame or output size (up to 16 megapixels)");
    if(cancel && cancel->load()) return {};
    // GPU errors are non-fatal: the CPU path must always remain usable.
    // Keep CUDA optional per export job so disabling GPU in the UI works.
    if(enable_cuda && std::size_t(width)*height >= 256*256) {
        Frame gpu_output;
        if(try_upscale_fsr1_cuda(input,width,height,strength,gpu_output)) {
            if(cancel && cancel->load()) return {};
            return gpu_output;
        }
    }
    Frame result=input;
    while(result.width<width||result.height<height){
        result=easu(result,std::min(width,result.width*2),std::min(height,result.height*2),cancel);
        if(result.rgb.empty())return {};
    }
    rcas(result,std::clamp(strength,0.f,1.f),cancel);return result;
}
}
