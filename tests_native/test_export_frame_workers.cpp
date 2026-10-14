#include "media/export_parallelism.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using koralo::OrderedFrameWorkers;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    constexpr int count = 80;
    constexpr int pixels = 9000;
    const auto frame = [](int i) {
        OrderedFrameWorkers::Pixels result(pixels);
        for (int j = 0; j < pixels; ++j)
            result[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>((j*37+i*17) % 256);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        return result;
    };
    std::vector<OrderedFrameWorkers::Pixels> expected;
    expected.reserve(count);
    const auto start_serial = std::chrono::steady_clock::now();
    {
        OrderedFrameWorkers serial(1);
        for (int i=0;i<count;++i)
            expected.push_back(serial.submit([&,i]{return frame(i);}).get());
    }
    const auto serial_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start_serial).count();
    std::atomic<int> active{0}, maximum{0};
    const auto start_parallel = std::chrono::steady_clock::now();
    {
        OrderedFrameWorkers parallel(4);
        std::vector<std::future<OrderedFrameWorkers::Pixels>> pending;
        pending.reserve(count);
        for(int i=0;i<count;++i){
            pending.push_back(parallel.submit([&,i] {
                const int current = active.fetch_add(1)+1;
                int prior=maximum.load();
                while(current>prior && !maximum.compare_exchange_weak(prior,current)){}
                auto result=frame(i);
                active.fetch_sub(1);
                return result;
            }));
        }
        for(int i=0;i<count;++i)
            require(pending[static_cast<std::size_t>(i)].get()==expected[static_cast<std::size_t>(i)],
                    "parallel pixels differ or output frame order changed");
    }
    const auto parallel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start_parallel).count();
    require(maximum.load()>=2 && maximum.load()<=4, "worker concurrency exceeded or never increased");
    require(active.load()==0, "workers still active after join");
    {
        OrderedFrameWorkers pool(2);
        auto fail=pool.submit([]() -> OrderedFrameWorkers::Pixels {
            throw std::runtime_error("synthetic failure");
        });
        bool caught=false;
        try { (void)fail.get(); } catch(const std::runtime_error&) { caught=true; }
        require(caught, "worker error not propagated to calling thread");
    }
    {
        koralo::ExportInnerThreadBudget scope(3);
        require(koralo::export_inner_thread_budget==3, "inner worker budget not installed");
    }
    require(koralo::export_inner_thread_budget==0, "inner worker budget not restored");
    require(koralo::export_frame_worker_count(3840u*2160u*4u)<=4, "4K scratch memory cap exceeded");
    std::cout << "PASS: stable pixel order, bounded concurrency, exceptions, worker budget, RAM limit\n";
    std::cout << "serial_ms=" << serial_ms << " parallel_ms=" << parallel_ms <<
        " observed_concurrency=" << maximum.load() << '\n';
    return 0;
}
