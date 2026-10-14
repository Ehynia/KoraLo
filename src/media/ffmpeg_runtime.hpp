#pragma once

#include <string>

namespace koralo {

struct GpuBackendInfo {
    bool enabled = true;
    bool decode_available = false;
    bool encode_available = false;
    std::string decode_backend;
    std::string encoder;
    std::string label = "CPU";
};

// Resolves an FFmpeg-family executable using KoraLo overrides, the portable
// runtime directory, the application directory and finally PATH.
std::string locate_media_binary(const std::string& name);

// Probes usable FFmpeg hardware decode/encode support without changing any
// MediaEngine state. Expensive capability checks are intentionally isolated
// from preview/render scheduling.
GpuBackendInfo detect_ffmpeg_gpu_backend(const std::string& ffmpeg_path);

} // namespace koralo
