#include "decoder_pool.hpp"

#include <cmath>

namespace koralo {

bool DecoderPool::compatible(const std::string& path, int width, int height, double step) {
    return !path_.empty() && path_ == path && width_ == width && height_ == height &&
           std::abs(step_ - step) < 1e-7 && process_.running();
}

bool DecoderPool::start(const std::string& executable,
                        const std::vector<std::string>& arguments,
                        const std::string& path,
                        int width,
                        int height,
                        double step,
                        double next_time,
                        bool hardware,
                        std::string* error) {
    reset();
    if (!process_.start(executable, arguments, error)) return false;
    path_ = path;
    width_ = width;
    height_ = height;
    step_ = step;
    next_time_ = next_time;
    hardware_ = hardware;
    start_count_.fetch_add(1);
    return true;
}

bool DecoderPool::read_exact(void* data,
                             std::size_t bytes,
                             std::string* error,
                             const std::atomic<bool>* cancel,
                             int timeout_ms) {
    return process_.read_exact(data, bytes, error, cancel, timeout_ms);
}

void DecoderPool::reset() {
    process_.terminate();
    path_.clear();
    width_ = 0;
    height_ = 0;
    step_ = 0.0;
    next_time_ = 0.0;
    hardware_ = false;
}

} // namespace koralo
