#pragma once

#include "../platform/process.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace koralo {

// Stage 1 preserves r42's single persistent preview decoder, but owns it behind
// a pool boundary so Stage 3 can add multiple A/B decoder slots without
// spreading process/session bookkeeping back through MediaEngine.
class DecoderPool {
public:
    bool compatible(const std::string& path, int width, int height, double step);
    bool start(const std::string& executable,
               const std::vector<std::string>& arguments,
               const std::string& path,
               int width,
               int height,
               double step,
               double next_time,
               bool hardware,
               std::string* error = nullptr);
    bool read_exact(void* data,
                    std::size_t bytes,
                    std::string* error,
                    const std::atomic<bool>* cancel,
                    int timeout_ms = 10000);
    void reset();

    double step() const { return step_; }
    double next_time() const { return next_time_; }
    void advance() { next_time_ += step_; }
    bool hardware() const { return hardware_; }
    std::uint64_t start_count() const { return start_count_.load(); }

private:
    ReadPipeProcess process_;
    std::string path_;
    int width_ = 0;
    int height_ = 0;
    double step_ = 0.0;
    double next_time_ = 0.0;
    bool hardware_ = false;
    std::atomic<std::uint64_t> start_count_{0};
};

} // namespace koralo
