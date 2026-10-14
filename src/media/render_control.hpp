#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace koralo {

// Only atomics cross the render/UI boundary; no FFmpeg or GUI calls here.
struct RenderControlLive {
    std::atomic<unsigned> workers{0};
    std::atomic<unsigned> worker_cap{0};
    std::atomic<int> process_cpu_percent{0};
    std::atomic<std::uint64_t> available_memory_mb{0};
    std::atomic<int> state{0}; // 0 idle, 1 render, 2 verifying, 3 success, 4 failed
};

struct RenderControlSnapshot {
    unsigned workers=0, worker_cap=0;
    int process_cpu_percent=0;
    std::uint64_t available_memory_mb=0;
    int state=0;
};

RenderControlSnapshot render_control_snapshot(const RenderControlLive&);

class RenderControlSession {
public:
    RenderControlSession(const std::filesystem::path& output, RenderControlLive* live);
    ~RenderControlSession();
    RenderControlSession(const RenderControlSession&)=delete;
    RenderControlSession& operator=(const RenderControlSession&)=delete;
    void record(const std::string& event, const std::string& message);
    void update_workers(unsigned active, unsigned cap, int cpu_percent, std::uint64_t available_mb);
    void stage(int value, const std::string& message);
    void finish(bool success, const std::string& detail);
    const std::filesystem::path& log_path() const { return log_path_; }
private:
    std::filesystem::path log_path_;
    std::ofstream log_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point start_;
    RenderControlLive* live_;
};

// A bounded feedback controller. It keeps memory safe first, then uses measured
// process CPU activity to grow the active part of the existing fixed worker pool.
// Explicit KORALO_EXPORT_FRAME_WORKERS remains deterministic for A/B tests.
class AdaptiveRenderWorkers {
public:
    AdaptiveRenderWorkers(unsigned maximum, std::size_t rgba_frame_bytes,
                          bool fixed_override);
    unsigned active() const { return active_; }
    unsigned maximum() const { return maximum_; }
    int cpu_percent() const { return cpu_percent_; }
    std::uint64_t available_memory_mb() const { return available_mb_; }
    // Call from the ordered writer (one thread), never from frame workers.
    bool sample(); // true when statistics refreshed
private:
    unsigned maximum_, active_;
    std::uint64_t reserved_bytes_;
    bool fixed_;
    int cpu_percent_=0;
    std::uint64_t available_mb_=0;
    std::chrono::steady_clock::time_point last_;
    std::uint64_t last_process_cpu_ns_=0;
    unsigned low_cpu_samples_=0;
};

// Checks actual codec streams and complete decodability before the temporary
// export is atomically published. Existing destination stays intact on failure.
bool verify_rendered_video(const std::string& ffprobe, const std::string& ffmpeg,
                           const std::filesystem::path& candidate,
                           int expected_width, int expected_height,
                           double expected_duration,
                           const std::atomic<bool>* cancel,
                           std::string* error, bool expect_audio=false,
                           RenderControlSession* session=nullptr);

} // namespace koralo
