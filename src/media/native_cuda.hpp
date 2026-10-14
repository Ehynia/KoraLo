#pragma once
#include <vector>
namespace koralo {
// GPU processing is an explicit, scoped export policy. Preview and CPU jobs
// never acquire a CUDA device without opting in.
extern thread_local bool native_cuda_opt_in;
struct NativeCudaScope {
    bool previous;
    explicit NativeCudaScope(bool enabled) noexcept : previous(native_cuda_opt_in) { native_cuda_opt_in=enabled; }
    ~NativeCudaScope() { native_cuda_opt_in=previous; }
    NativeCudaScope(const NativeCudaScope&) = delete;
    NativeCudaScope& operator=(const NativeCudaScope&) = delete;
};
inline bool native_cuda_enabled() noexcept { return native_cuda_opt_in; }
bool try_native_gaussian_cuda(const std::vector<float>& input,int width,int height,
                              float sigma,std::vector<float>& output);
bool try_native_morph_cuda(const std::vector<float>& input,int width,int height,
                           int radius,bool maximum,std::vector<float>& output);
}
