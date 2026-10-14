from pathlib import Path
r=Path(__file__).parent
p=r/'CMakeLists.txt'; s=p.read_text(); q='option(KORALO_BUILD_CLI "Build offline CLI" ON)'; assert q in s
s=s.replace(q,q+'''
# CUDA is optional: the editor must still build and render via CPU without
# nvcc, an NVIDIA device, or the CUDA Toolkit on the target machine.
option(KORALO_ENABLE_CUDA "Build CUDA FSR kernels when NVIDIA CUDA Toolkit is installed" ON)
set(KORALO_CUDA_AVAILABLE OFF)
if(KORALO_ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        find_package(CUDAToolkit QUIET)
        if(CUDAToolkit_FOUND)
            set(KORALO_CUDA_AVAILABLE ON)
            if(NOT CMAKE_CUDA_ARCHITECTURES)
                set(CMAKE_CUDA_ARCHITECTURES 75 86)
            endif()
            message(STATUS "KoraLo CUDA FSR enabled (${CMAKE_CUDA_COMPILER})")
        endif()
    endif()
endif()
if(NOT KORALO_CUDA_AVAILABLE)
    message(STATUS "KoraLo CUDA FSR unavailable - retaining portable CPU FSR and FFmpeg NVENC")
endif()
''')
anchor='target_include_directories(koralo_core PUBLIC src)'; assert anchor in s
s=s.replace(anchor,'''if(KORALO_CUDA_AVAILABLE)
    target_sources(koralo_core PRIVATE src/media/fsr_cuda.cu)
    target_link_libraries(koralo_core PRIVATE CUDA::cudart_static)
    set_target_properties(koralo_core PROPERTIES CUDA_RUNTIME_LIBRARY Static)
else()
    target_sources(koralo_core PRIVATE src/media/fsr_cuda_stub.cpp)
endif()
'''+anchor)
p.write_text(s)
p=r/'src/media/fsr_upscaler.hpp'; s=p.read_text();s=s.replace('const std::atomic<bool>* cancel=nullptr);','const std::atomic<bool>* cancel=nullptr, bool enable_cuda=false);');s=s.replace('// strength: 0 disables RCAS, 1 is maximum. Cancellation returns an empty frame.','// strength: 0 disables RCAS, 1 is maximum. Cancellation returns an empty frame.\n// If enable_cuda, use NVIDIA CUDA kernels when built/available; otherwise CPU.');p.write_text(s)
p=r/'src/media/fsr_upscaler.cpp';s=p.read_text();s=s.replace('#include "fsr_upscaler.hpp"','#include "fsr_upscaler.hpp"\n#include "fsr_cuda.hpp"');s=s.replace('float strength,const std::atomic<bool>* cancel){\n    if(input.width<1','float strength,const std::atomic<bool>* cancel,bool enable_cuda){\n    if(input.width<1');needle='''    Frame result=input;
    while(result.width<width||result.height<height){''';assert needle in s
s=s.replace(needle,'''    if(cancel && cancel->load()) return {};
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
    while(result.width<width||result.height<height){''');p.write_text(s)
p=r/'src/media/render_planner.cpp';s=p.read_text();s=s.replace('''           (plan.width==project.settings.width&&plan.height==project.settings.height)||
           std::uint64_t(plan.width)*plan.height>16*1024*1024)''','''           std::uint64_t(plan.width)*plan.height>16*1024*1024)''');s=s.replace('''return fail("FSR requires output larger than the project, without reducing either dimension (up to 16 megapixels)");''','''return fail("FSR: output must be at least the project size, both dimensions even, and <= 16 megapixels. Check resolution in Project Settings and Export.");''');p.write_text(s)
p=r/'src/media/native_export.inc';s=p.read_text();needle='''int fsr_source_width=0, int fsr_source_height=0, float fsr_sharpness=.5f, RenderControlSession* control=nullptr)''';assert needle in s;s=s.replace(needle,'''int fsr_source_width=0, int fsr_source_height=0, float fsr_sharpness=.5f, RenderControlSession* control=nullptr, bool enable_cuda_fsr=false)''');s=s.replace('''           fsr_sharpness, &motion, &effects, cancel, flatten]() mutable {''','''           fsr_sharpness, enable_cuda_fsr, &motion, &effects, cancel, flatten]() mutable {''');s=s.replace('''frame = upscale_fsr1(frame, width, height, fsr_sharpness, cancel);''','''frame = upscale_fsr1(frame, width, height, fsr_sharpness, cancel, enable_cuda_fsr);''');s=s.replace('''options.fsr_sharpness,options.control))return false;''','''options.fsr_sharpness,options.control,options.use_gpu&&gpu_enabled_flag_.load()))return false;''');
# Report failing frame index and FFmpeg exit conditions through existing error path.
s=s.replace('''"Native source decode failed at frame " + std::to_string(j));''','''"FSR/native source decode failed at frame " + std::to_string(j) +
                      " (" + std::to_string(decode_width) + "x" + std::to_string(decode_height) +
                      ", FFmpeg raw RGBA). Check input decoding and FFmpeg stderr.");''')
p.write_text(s)
p=r/'BUILD_REVISION.txt';p.write_text('2026-10-10-r45-cuda-fsr-fallback-fixes\n')
