#include "lawrec_frame_queue.h"
#include <cassert>
#include <thread>
#include <cstdio>

static LawrecFramePtr frame(size_t size, uint64_t pts) {
    auto result = std::make_shared<LawrecEncodedFrame>();
    result->bytes.resize(size, 0x65); result->pts_us = pts;
    return result;
}
int main() {
    LawrecFrameQueue queue(2, 10);
    LawrecFramePtr out;
    assert(queue.pop(out, 0) == -ECANCELED);
    queue.reset();
    assert(queue.pop(out, 1) == 0);
    assert(queue.push(frame(4, 1)) == 0);
    assert(queue.push(frame(6, 2)) == 0);
    assert(queue.pop(out, 0) == 1 && out->pts_us == 1);
    assert(queue.pop(out, 0) == 1 && out->pts_us == 2);
    assert(queue.push(frame(11, 3)) == -ENOBUFS);
    assert(queue.pop(out, 0) == -ENOBUFS && !out);
    assert(queue.push(frame(1, 4)) == -ENOBUFS);
    queue.reset();
    assert(queue.push(frame(1, 1)) == 0);
    assert(queue.push(frame(1, 2)) == 0);
    assert(queue.push(frame(1, 3)) == -ENOBUFS);
    queue.reset();
    assert(queue.push(nullptr) == -EINVAL);
    queue.reset();
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); queue.close();
    });
    assert(queue.pop(out, 1000) == -ECANCELED); closer.join();
    LawrecFrameQueue threaded(64, 1024);
    threaded.reset();
    std::thread producer([&] { for (unsigned i = 0; i < 32; ++i) assert(threaded.push(frame(16, i)) == 0); });
    for (unsigned i = 0; i < 32; ++i) {
        assert(threaded.pop(out, 1000) == 1 && out->pts_us == i);
    }
    producer.join(); threaded.close();
    assert(threaded.pop(out, 0) == -ECANCELED);
    puts("frame queue: FIFO, bytes/frames bound, sticky failure, reset, close, concurrency passed");
}
