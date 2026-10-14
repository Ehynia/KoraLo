// CUDA implementations of separable spatial primitives used by Anime V45.
// This is not a full GPU pipeline: other native stages still run on the CPU.
// Failures are recoverable, including insufficient VRAM and missing driver.
#include "native_cuda.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace koralo { namespace {
__device__ __forceinline__ int reflected(int x,int n){
    if(n<2)return 0;
    // Spatial radii <= 32, so one reflection normally suffices; handle tiny images.
    while(x<0||x>=n)x=x<0?-x:2*n-2-x;
    return x;
}
__global__ void gaussian_x(const float* src,float* dst,int w,int h,const float* kernel,int r){
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=w||y>=h)return;
    float sum=0.f;
    for(int i=-r;i<=r;++i)sum+=src[y*w+reflected(x+i,w)]*kernel[i+r];
    dst[y*w+x]=sum;
}
__global__ void gaussian_y(const float* src,float* dst,int w,int h,const float* kernel,int r){
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=w||y>=h)return;
    float sum=0.f;
    for(int i=-r;i<=r;++i)sum+=src[reflected(y+i,h)*w+x]*kernel[i+r];
    dst[y*w+x]=sum;
}
__global__ void morph_x(const float* src,float* dst,int w,int h,int r,bool maximum){
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=w||y>=h)return;
    float v=src[y*w+x];
    for(int i=-r;i<=r;++i){float a=src[y*w+max(0,min(w-1,x+i))];v=maximum?fmaxf(v,a):fminf(v,a);}
    dst[y*w+x]=v;
}
__global__ void morph_y(const float* src,float* dst,int w,int h,int r,bool maximum){
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=w||y>=h)return;
    float v=src[y*w+x];
    for(int i=-r;i<=r;++i){float a=src[max(0,min(h-1,y+i))*w+x];v=maximum?fmaxf(v,a):fminf(v,a);}
    dst[y*w+x]=v;
}
struct DeviceBuffer {
    float* data=nullptr;
    ~DeviceBuffer(){if(data)cudaFree(data);}
    bool allocate(size_t count){return cudaMalloc(reinterpret_cast<void**>(&data),count*sizeof(float))==cudaSuccess;}
    DeviceBuffer()=default;
    DeviceBuffer(const DeviceBuffer&)=delete;
    DeviceBuffer& operator=(const DeviceBuffer&)=delete;
};
bool valid(const std::vector<float>& in,int w,int h){
    return w>0&&h>0&&static_cast<std::uint64_t>(w)*h<=16u*1024u*1024u&&
           in.size()==static_cast<std::size_t>(w)*h;
}
bool available(){int count=0;return cudaGetDeviceCount(&count)==cudaSuccess&&count>0;}
}
bool try_native_gaussian_cuda(const std::vector<float>& input,int w,int h,float sigma,
                              std::vector<float>& output){
    if(!valid(input,w,h)||!std::isfinite(sigma)||sigma<=0||!available())return false;
    const int r=std::clamp(static_cast<int>(std::nearbyint(2.5f*sigma)),1,32);
    float kernel[65]{};float total=0.f;
    for(int i=-r;i<=r;++i){kernel[i+r]=std::exp(-float(i*i)/(2.f*sigma*sigma));total+=kernel[i+r];}
    for(int i=0;i<=2*r;++i)kernel[i]/=total;
    DeviceBuffer src,tmp,dst,weights;
    const size_t count=input.size(),bytes=count*sizeof(float);
    if(!src.allocate(count)||!tmp.allocate(count)||!dst.allocate(count)||!weights.allocate(2*r+1))return false;
    if(cudaMemcpy(src.data,input.data(),bytes,cudaMemcpyHostToDevice)!=cudaSuccess||
       cudaMemcpy(weights.data,kernel,size_t(2*r+1)*sizeof(float),cudaMemcpyHostToDevice)!=cudaSuccess)return false;
    dim3 block(16,16),grid((w+15)/16,(h+15)/16);
    gaussian_x<<<grid,block>>>(src.data,tmp.data,w,h,weights.data,r);
    if(cudaGetLastError()!=cudaSuccess)return false;
    gaussian_y<<<grid,block>>>(tmp.data,dst.data,w,h,weights.data,r);
    if(cudaGetLastError()!=cudaSuccess)return false;
    std::vector<float> result(count);
    if(cudaMemcpy(result.data(),dst.data,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess)return false;
    output=std::move(result);return true;
}
bool try_native_morph_cuda(const std::vector<float>& input,int w,int h,int radius,
                           bool maximum,std::vector<float>& output){
    if(!valid(input,w,h)||radius<0||radius>32||!available())return false;
    DeviceBuffer src,tmp,dst;
    const size_t count=input.size(),bytes=count*sizeof(float);
    if(!src.allocate(count)||!tmp.allocate(count)||!dst.allocate(count))return false;
    if(cudaMemcpy(src.data,input.data(),bytes,cudaMemcpyHostToDevice)!=cudaSuccess)return false;
    dim3 block(16,16),grid((w+15)/16,(h+15)/16);
    morph_x<<<grid,block>>>(src.data,tmp.data,w,h,radius,maximum);
    if(cudaGetLastError()!=cudaSuccess)return false;
    morph_y<<<grid,block>>>(tmp.data,dst.data,w,h,radius,maximum);
    if(cudaGetLastError()!=cudaSuccess)return false;
    std::vector<float> result(count);
    if(cudaMemcpy(result.data(),dst.data,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess)return false;
    output=std::move(result);return true;
}
} // namespace koralo
