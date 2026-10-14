#pragma once
#include "frame_cache.hpp"
#include <string>
namespace koralo {
// Optional CUDA implementation of the same EASU/RCAS stages as the CPU FSR 1
// adaptation. False means no CUDA runtime/device or a recoverable CUDA error;
// the caller must then run the unchanged CPU algorithm.
bool fsr_cuda_available();
bool try_upscale_fsr1_cuda(const Frame& source, int width, int height,
                           float sharpness, Frame& output, std::string* reason=nullptr);
}
