// Optional CUDA implementation of KoraLo's FSR 1-style EASU + RCAS adaptation.
// Follows the scalar algorithm in fsr_upscaler.cpp. Requires NVIDIA GPU and
// CUDA Toolkit at build time; portable CPU implementation remains the fallback.
#include "fsr_cuda.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
namespace koralo { namespace {
struct Pixel {float v[3];};
__device__ __forceinline__ float limit(float x,float a,float b){return fminf(fmaxf(x,a),b);}
__device__ __forceinline__ int limit_i(int x,int a,int b){return max(a,min(x,b));}
__device__ __forceinline__ unsigned char to_byte(float v){return static_cast<unsigned char>(floorf(limit(v,0.f,1.f)*255.f+.5f));}
__device__ __forceinline__ Pixel load_rgb(const unsigned char* src,int w,int h,int x,int y){
  const int i=(limit_i(y,0,h-1)*w+limit_i(x,0,w-1))*3;
  return {{src[i]/255.f,src[i+1]/255.f,src[i+2]/255.f}};
}
__device__ __forceinline__ float lum(const Pixel& p){return p.v[1]+.5f*(p.v[0]+p.v[2]);}
__device__ __forceinline__ float load_alpha(const unsigned char* a,int w,int h,int x,int y){
  return a[limit_i(y,0,h-1)*w+limit_i(x,0,w-1)]/255.f;
}
__device__ void edge(float w,float a,float b,float c,float d,float e,float& dx,float& dy,float& len){
  const float gx=d-b,gy=e-a;dx+=gx*w;dy+=gy*w;
  const float lx=limit(fabsf(gx)/fmaxf(fmaxf(fabsf(d-c),fabsf(c-b)),1e-8f),0.f,1.f);
  const float ly=limit(fabsf(gy)/fmaxf(fmaxf(fabsf(e-c),fabsf(c-a)),1e-8f),0.f,1.f);
  len+=(lx*lx+ly*ly)*w;
}
__global__ void easu_kernel(const unsigned char* src,const unsigned char* alpha,
 unsigned char* dst,unsigned char* dst_alpha,int in_w,int in_h,int out_w,int out_h){
  const int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
  if(x>=out_w||y>=out_h)return;
  const float sx=(x+.5f)*in_w/out_w-.5f,sy=(y+.5f)*in_h/out_h-.5f;
  const int ix=static_cast<int>(floorf(sx)),iy=static_cast<int>(floorf(sy));
  const float px=sx-ix,py=sy-iy;
  constexpr int taps[12][2]={{0,-1},{1,-1},{-1,0},{0,0},{1,0},{2,0},{-1,1},{0,1},{1,1},{2,1},{0,2},{1,2}};
  Pixel colors[12];float l[12];
  for(int k=0;k<12;++k){colors[k]=load_rgb(src,in_w,in_h,ix+taps[k][0],iy+taps[k][1]);l[k]=lum(colors[k]);}
  float dx=0.f,dy=0.f,len=0.f;
  edge((1-px)*(1-py),l[0],l[2],l[3],l[4],l[7],dx,dy,len);
  edge(px*(1-py),l[1],l[3],l[4],l[5],l[8],dx,dy,len);
  edge((1-px)*py,l[3],l[6],l[7],l[8],l[10],dx,dy,len);
  edge(px*py,l[4],l[7],l[8],l[9],l[11],dx,dy,len);
  const float d2=dx*dx+dy*dy;
  if(d2<1.f/32768){dx=1.f;dy=0.f;}else{const float inv=rsqrtf(d2);dx*=inv;dy*=inv;}
  len*=.5f;len*=len;
  const float stretch=1.f/fmaxf(fabsf(dx),fabsf(dy));
  const float ax=1+(stretch-1)*len,ay=1-.5f*len,lobe=.5f+(.21f-.5f)*len,clip=1.f/lobe;
  Pixel sum{{0,0,0}};float weights=0;
  for(int k=0;k<12;++k){
    const float ox=taps[k][0]-px,oy=taps[k][1]-py;
    const float vx=(ox*dx+oy*dy)*ax,vy=(-ox*dy+oy*dx)*ay;
    const float dist=fminf(vx*vx+vy*vy,clip);
    const float wb=.4f*dist-1,wa=lobe*dist-1;
    const float weight=(1.5625f*wb*wb-.5625f)*wa*wa;
    weights+=weight;for(int c=0;c<3;++c)sum.v[c]+=colors[k].v[c]*weight;
  }
  const int index=y*out_w+x;
  for(int c=0;c<3;++c){
    const float lo=fminf(fminf(colors[3].v[c],colors[4].v[c]),fminf(colors[7].v[c],colors[8].v[c]));
    const float hi=fmaxf(fmaxf(colors[3].v[c],colors[4].v[c]),fmaxf(colors[7].v[c],colors[8].v[c]));
    dst[index*3+c]=to_byte(limit(fabsf(weights)>1e-8f?sum.v[c]/weights:colors[3].v[c],lo,hi));
  }
  if(dst_alpha){
    const float a00=load_alpha(alpha,in_w,in_h,ix,iy),a10=load_alpha(alpha,in_w,in_h,ix+1,iy);
    const float a01=load_alpha(alpha,in_w,in_h,ix,iy+1),a11=load_alpha(alpha,in_w,in_h,ix+1,iy+1);
    dst_alpha[index]=to_byte((1-py)*((1-px)*a00+px*a10)+py*((1-px)*a01+px*a11));
  }
}
__global__ void rcas_kernel(const unsigned char* src,unsigned char* dst,int w,int h,float strength){
  const int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
  if(x>=w||y>=h)return;
  const Pixel b=load_rgb(src,w,h,x,y-1),d=load_rgb(src,w,h,x-1,y);
  const Pixel e=load_rgb(src,w,h,x,y),f=load_rgb(src,w,h,x+1,y),h1=load_rgb(src,w,h,x,y+1);
  float lobe=-.1875f;
  for(int c=0;c<3;++c){
    const float lo=fminf(fminf(b.v[c],d.v[c]),fminf(f.v[c],h1.v[c]));
    const float hi=fmaxf(fmaxf(b.v[c],d.v[c]),fmaxf(f.v[c],h1.v[c]));
    const float hit_min=fminf(lo,e.v[c])/fmaxf(4.f*hi,1e-8f);
    const float hit_max=(1.f-fmaxf(hi,e.v[c]))/fminf(4.f*lo-4.f,-1e-8f);
    lobe=fmaxf(lobe,fmaxf(-hit_min,hit_max));
  }
  lobe=fminf(lobe,0.f)*strength;
  const int index=(y*w+x)*3;
  for(int c=0;c<3;++c)dst[index+c]=to_byte((e.v[c]+lobe*(b.v[c]+d.v[c]+f.v[c]+h1.v[c]))/(1+4*lobe));
}
std::string cuda_error(cudaError_t result){return cudaGetErrorString(result);}
} // namespace
bool fsr_cuda_available(){int devices=0;return cudaGetDeviceCount(&devices)==cudaSuccess&&devices>0;}
bool try_upscale_fsr1_cuda(const Frame& source,int width,int height,float sharpness,
                          Frame& output,std::string* reason){
  if(!fsr_cuda_available()){if(reason)*reason="CUDA device or NVIDIA driver unavailable";return false;}
  if(source.width<1||source.height<1||width<source.width||height<source.height||
     uint64_t(width)*height>16u*1024u*1024u||source.rgb.size()!=size_t(source.width)*source.height*3||
     (!source.alpha.empty()&&source.alpha.size()!=size_t(source.width)*source.height)){
    if(reason)*reason="Invalid CUDA FSR frame dimensions";return false;
  }
  unsigned char *a=nullptr,*b=nullptr,*aa=nullptr,*ab=nullptr;
  auto fail=[&](cudaError_t err){if(reason)*reason=cuda_error(err);return false;};
  // At most four device buffers; upper bound derives from the 16-Mpixel FSR cap.
  const size_t rgb_capacity=size_t(width)*height*3,alpha_capacity=size_t(width)*height;
  cudaError_t err=cudaMalloc(reinterpret_cast<void**>(&a),rgb_capacity);
  if(err==cudaSuccess)err=cudaMalloc(reinterpret_cast<void**>(&b),rgb_capacity);
  if(err==cudaSuccess&&!source.alpha.empty())err=cudaMalloc(reinterpret_cast<void**>(&aa),alpha_capacity);
  if(err==cudaSuccess&&!source.alpha.empty())err=cudaMalloc(reinterpret_cast<void**>(&ab),alpha_capacity);
  if(err==cudaSuccess)err=cudaMemcpy(a,source.rgb.data(),source.rgb.size(),cudaMemcpyHostToDevice);
  if(err==cudaSuccess&&!source.alpha.empty())err=cudaMemcpy(aa,source.alpha.data(),source.alpha.size(),cudaMemcpyHostToDevice);
  int w=source.width,h=source.height;
  const dim3 block(16,16);
  while(err==cudaSuccess&&(w<width||h<height)){
    const int nw=std::min(width,w*2),nh=std::min(height,h*2);
    const dim3 grid((nw+15)/16,(nh+15)/16);
    easu_kernel<<<grid,block>>>(a,aa,b,ab,w,h,nw,nh);
    err=cudaGetLastError();
    if(err==cudaSuccess){std::swap(a,b);std::swap(aa,ab);w=nw;h=nh;}
  }
  if(err==cudaSuccess&&sharpness>0.f){
    const dim3 grid((width+15)/16,(height+15)/16);
    rcas_kernel<<<grid,block>>>(a,b,width,height,std::clamp(sharpness,0.f,1.f));
    err=cudaGetLastError();
    if(err==cudaSuccess)std::swap(a,b);
  }
  Frame result;result.width=width;result.height=height;
  if(err==cudaSuccess){result.rgb.resize(rgb_capacity);err=cudaMemcpy(result.rgb.data(),a,rgb_capacity,cudaMemcpyDeviceToHost);}
  if(err==cudaSuccess&&aa){result.alpha.resize(alpha_capacity);err=cudaMemcpy(result.alpha.data(),aa,alpha_capacity,cudaMemcpyDeviceToHost);}
  cudaFree(a);cudaFree(b);cudaFree(aa);cudaFree(ab);
  if(err!=cudaSuccess)return fail(err);
  output=std::move(result);return true;
}
} // namespace koralo
