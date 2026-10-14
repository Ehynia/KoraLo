#include "fsr_cuda.hpp"
namespace koralo {
bool fsr_cuda_available() { return false; }
bool try_upscale_fsr1_cuda(const Frame&,int,int,float,Frame&,std::string* reason) {
    if(reason) *reason="KoraLo was built without the CUDA Toolkit (CPU FSR remains available)";
    return false;
}
}
