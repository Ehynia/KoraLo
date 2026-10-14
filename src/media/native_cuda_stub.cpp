#include "native_cuda.hpp"
namespace koralo {
bool try_native_gaussian_cuda(const std::vector<float>&,int,int,float,std::vector<float>&){return false;}
bool try_native_morph_cuda(const std::vector<float>&,int,int,int,bool,std::vector<float>&){return false;}
}
