#include "media/playback_controller.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace koralo;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static Frame frame(std::uint8_t value) {
    Frame f;
    f.width = 1;
    f.height = 1;
    f.rgb = {value, value, value};
    return f;
}

int main() {
    try {
        PlaybackController controller;
        PreviewRequest first;
        first.token = 1;
        controller.note_request(first);
        controller.queue_request(first, "same");

        PreviewRequest newer = first;
        newer.token = 2;
        newer.playback_epoch=42;newer.playback=true;newer.continuous=true;
        controller.note_request(newer);
        controller.queue_request(newer, "same");

        std::atomic<bool> stop{false};
        PreviewRequest dequeued;
        require(controller.wait_next(stop, dequeued), "queued request available");
        require(dequeued.token == 2, "identical pending request coalesces to newest token");

        require(dequeued.playback_epoch==42&&dequeued.playback&&dequeued.continuous,"coalescing retains newest transport metadata");

        controller.publish_cached(2, frame(20));
        controller.publish_cached(3, frame(30));
        std::uint64_t token = 0;
        Frame out;
        std::string error;
        require(controller.poll(token, out, error) && token == 3 && out.rgb[0] == 30,
                "poll chooses newest published token and clears queue");
        require(!controller.poll(token, out, error), "poll consumes published results");

        PreviewRequest old_playback;
        old_playback.token = 4;
        old_playback.playback = true;
        old_playback.playback_epoch = 10;
        controller.note_request(old_playback);
        require(controller.should_publish(old_playback), "current playback epoch publishes");

        PreviewRequest seek;
        seek.token = 5;
        seek.playback = true;
        seek.playback_epoch = 11;
        controller.note_request(seek);
        require(!controller.should_publish(old_playback), "seek invalidates completed frame from old epoch");
        require(controller.should_publish(seek), "new playback epoch publishes");

        PreviewRequest advancing = seek;
        advancing.token = 4;
        require(controller.should_publish(advancing),
                "late playback frame within active epoch remains publishable");

        controller.publish_worker_result(old_playback, frame(40), {});
        require(!controller.poll(token, out, error), "old epoch cannot refill result queue");
        controller.publish_worker_result(seek, frame(50), {});
        require(controller.poll(token, out, error) && token == 5 && out.rgb[0] == 50,
                "current epoch worker result is observable");

        std::atomic<bool> stopping{true};
        PreviewRequest ignored;
        require(!controller.wait_next(stopping, ignored), "stop state exits wait path");

        // Concurrency regression: producer, worker and foreground poller touch
        // the controller at the same time. Exact frame count is intentionally
        // not asserted because coalescing is part of the scheduling contract.
        PlaybackController concurrent;
        std::atomic<bool> done{false};
        std::atomic<int> published{0};
        std::thread worker([&]{
            PreviewRequest request;
            while(concurrent.wait_next(done,request)){
                concurrent.publish_worker_result(request,frame(static_cast<std::uint8_t>(request.token&0xffu)),{});
                published.fetch_add(1);
            }
        });
        std::thread producer([&]{
            for(std::uint64_t i=1;i<=200;++i){
                PreviewRequest request;request.token=i;request.playback=true;request.playback_epoch=77;
                concurrent.note_request(request);
                concurrent.queue_request(std::move(request),"concurrent");
                if((i%8)==0)std::this_thread::yield();
            }
        });
        std::thread poller([&]{
            std::uint64_t seen=0;Frame result;std::string poll_error;
            while(!done.load()||seen<concurrent.latest_token()){
                std::uint64_t current=0;
                if(concurrent.poll(current,result,poll_error))seen=std::max(seen,current);
                else std::this_thread::yield();
                if(seen>=200)break;
            }
        });
        producer.join();
        for(int i=0;i<200&&published.load()==0;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        done=true;concurrent.wake();worker.join();poller.join();
        require(concurrent.latest_token()==200,"concurrent requests preserve latest token");
        require(published.load()>0,"concurrent worker publishes at least one coalesced request");

        std::cout << "KoraLo playback controller tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
