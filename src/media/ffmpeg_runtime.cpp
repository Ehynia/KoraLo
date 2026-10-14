#include "ffmpeg_runtime.hpp"

#include "../platform/app_paths.hpp"
#include "../platform/process.hpp"

#include <cctype>
#include <filesystem>

namespace koralo {
namespace fs = std::filesystem;

std::string locate_media_binary(const std::string& name) {
    std::string exe = name;
#ifdef _WIN32
    exe += ".exe";
#endif
    std::string key = "KORALO_";
    for (unsigned char c : name) key += static_cast<char>(std::toupper(c));

    auto explicit_path = path_from_utf8(environment_utf8(key.c_str()));
    if (!explicit_path.empty()) {
        return is_executable_file(explicit_path) ? path_to_utf8(explicit_path) : std::string{};
    }

    auto runtime = path_from_utf8(environment_utf8("KORALO_FFMPEG_DIR"));
    if (!runtime.empty()) {
        const auto candidate = runtime / exe;
        return is_executable_file(candidate) ? path_to_utf8(candidate) : std::string{};
    }

    const auto root = app_install_directory();
    for (const auto& candidate : {root / "bin" / exe, root / exe}) {
        if (!root.empty() && is_executable_file(candidate)) return path_to_utf8(candidate);
    }
    return find_executable_on_path(exe);
}

GpuBackendInfo detect_ffmpeg_gpu_backend(const std::string& ffmpeg_path) {
    GpuBackendInfo out;
    if (ffmpeg_path.empty()) {
        out.enabled = false;
        out.label = "CPU (FFmpeg missing)";
        return out;
    }

    const auto hw = run_process(ffmpeg_path, {"-hide_banner", "-hwaccels"});
    const std::string h = hw.output;
#ifdef _WIN32
    if (h.find("d3d11va") != std::string::npos) {
        out.decode_available = true;
        out.decode_backend = "d3d11va";
        out.label = "FFmpeg hwaccel d3d11va (CPU fallback)";
    } else if (h.find("dxva2") != std::string::npos) {
        out.decode_available = true;
        out.decode_backend = "dxva2";
        out.label = "FFmpeg hwaccel dxva2 (CPU fallback)";
    }
#else
    if (h.find("cuda") != std::string::npos && fs::exists("/dev/nvidia0")) {
        out.decode_available = true;
        out.decode_backend = "cuda";
        out.label = "FFmpeg hwaccel cuda (CPU fallback)";
    } else if (h.find("vaapi") != std::string::npos && fs::exists("/dev/dri/renderD128")) {
        out.decode_available = true;
        out.decode_backend = "vaapi";
        out.label = "FFmpeg hwaccel vaapi (CPU fallback)";
    }
#endif

    const auto enc = run_process(ffmpeg_path, {"-hide_banner", "-encoders"});
    const std::string e = enc.output;
    for (const char* candidate : {"h264_nvenc", "h264_qsv", "h264_amf"}) {
        if (e.find(candidate) == std::string::npos) continue;
        ProcessOptions options;
        options.timeout_ms = 2000;
        auto check = run_process(ffmpeg_path,
            {"-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=s=64x64:d=0.04",
             "-frames:v", "1", "-c:v", candidate, "-f", "null", "-"}, options);
        if (check.started && check.exit_code == 0) {
            out.encode_available = true;
            out.encoder = candidate;
            break;
        }
    }
    if (!out.decode_available && !out.encode_available) {
        out.label = "CPU (no usable FFmpeg GPU backend detected)";
    }
    return out;
}

} // namespace koralo
