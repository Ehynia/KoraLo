#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace koralo {

// The outer frame workers and inner spatial-filter workers share one CPU budget.
// A value of zero preserves the existing preview/default parallel-row behavior.
inline thread_local unsigned export_inner_thread_budget = 0;

class ExportInnerThreadBudget {
public:
    explicit ExportInnerThreadBudget(unsigned budget) : previous_(export_inner_thread_budget) {
        export_inner_thread_budget = budget;
    }
    ~ExportInnerThreadBudget() { export_inner_thread_budget = previous_; }
    ExportInnerThreadBudget(const ExportInnerThreadBudget&) = delete;
    ExportInnerThreadBudget& operator=(const ExportInnerThreadBudget&) = delete;
private:
    unsigned previous_;
};

// Keep both the number of tasks and their scratch-memory footprint bounded.
// KORALO_EXPORT_FRAME_WORKERS=1 is a deterministic serial baseline for A/B tests.
inline bool export_fixed_worker_override() {
    return std::getenv("KORALO_EXPORT_FRAME_WORKERS") != nullptr;
}

inline unsigned export_frame_worker_count(std::size_t rgba_bytes) {
    const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    unsigned desired = std::clamp(hc / 3u, 1u, 8u);
    if (const char* value = std::getenv("KORALO_EXPORT_FRAME_WORKERS")) {
        try {
            const std::string requested(value);
            std::size_t consumed = 0;
            const unsigned long count = std::stoul(requested, &consumed, 10);
            if (consumed == requested.size() && count >= 1 && count <= 8)
                desired = static_cast<unsigned>(count);
        } catch (...) { /* Ignore malformed override. */ }
    }
    constexpr std::size_t kScratchBudget = 512ull * 1024 * 1024;
    // RGBA input/output + RGB/alpha + effect scratch planes; conservative.
    const std::size_t estimated_per_worker = std::max<std::size_t>(1, rgba_bytes) * 6;
    const unsigned memory_workers = static_cast<unsigned>(
        std::max<std::size_t>(1, kScratchBudget / estimated_per_worker));
    return std::min(desired, memory_workers);
}

inline unsigned export_inner_worker_count(unsigned frame_workers) {
    if (frame_workers <= 1) return 0; // Preserve previous intra-frame behavior.
    const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hc / frame_workers, 1u, 4u);
}

// Fixed worker threads instead of launching one std::async/thread per frame.
// Callers keep <= N pending futures and consume them in submission order.
class OrderedFrameWorkers {
public:
    using Pixels = std::vector<std::uint8_t>;
    using Task = std::packaged_task<Pixels()>;

    explicit OrderedFrameWorkers(unsigned count) {
        if (count <= 1) return;
        try {
            for (unsigned i = 0; i < count; ++i) {
                threads_.emplace_back([this] {
                    for (;;) {
                        Task task;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            cv_.wait(lock, [this] { return (stopping_ && queue_.empty()) || (!queue_.empty() && active_tasks_<active_limit_); });
                            if (queue_.empty() && stopping_) return;
                            if (queue_.empty() || active_tasks_>=active_limit_) continue;
                            task = std::move(queue_.front());
                            queue_.pop_front();
                            ++active_tasks_;
                        }
                        task(); // packaged_task stores exceptions in the future
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            --active_tasks_;
                        }
                        cv_.notify_all();
                    }
                });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~OrderedFrameWorkers() { shutdown(); }
    OrderedFrameWorkers(const OrderedFrameWorkers&) = delete;
    OrderedFrameWorkers& operator=(const OrderedFrameWorkers&) = delete;

    // Changes the number of workers allowed to run concurrently, without
    // recreating threads or stopping already-running frame jobs.
    void set_active_limit(unsigned wanted) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_limit_=std::clamp(wanted,1u,std::max(1u,static_cast<unsigned>(threads_.size())));
        }
        cv_.notify_all();
    }

    std::future<Pixels> submit(std::function<Pixels()> work) {
        Task task(std::move(work));
        auto future = task.get_future();
        if (threads_.empty()) {
            task();
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push_back(std::move(task));
            }
            cv_.notify_one();
        }
        return future;
    }

private:
    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& thread : threads_)
            if (thread.joinable()) thread.join();
        threads_.clear();
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
    unsigned active_tasks_ = 0;
    unsigned active_limit_ = 8;
};

} // namespace koralo
