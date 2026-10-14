#pragma once
#include "frame_cache.hpp"
#include <atomic>
namespace koralo {
// CPU float32 adaptation of AMD FSR 1 EASU + RCAS. SDR RGB; alpha is bilinear.
// strength: 0 disables RCAS, 1 is maximum. Cancellation returns an empty frame.
// If enable_cuda, use NVIDIA CUDA kernels when built/available; otherwise CPU.
Frame upscale_fsr1(const Frame& input,int width,int height,float strength=.5f,
                   const std::atomic<bool>* cancel=nullptr, bool enable_cuda=false);
}
