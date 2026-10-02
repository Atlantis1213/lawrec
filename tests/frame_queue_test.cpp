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
    auto s = queue.stats();
    assert(!s.closed && !s.depth && !s.accepted && !s.rejected);
    assert(queue.pop(out, 1) == 0);
    assert(queue.push(frame(4, 1)) == 0);
    assert(queue.push(frame(6, 2)) == 0);
    s = queue.stats();
    assert(s.depth == 2 && s.bytes == 10 && s.accepted == 2);
    assert(s.peak_depth == 2 && s.peak_bytes == 10);
    assert(queue.pop(out, 0) == 1 && out->pts_us == 1);
    assert(queue.pop(out, 0) == 1 && out->pts_us == 2);
    assert(queue.push(frame(11, 3)) == -ENOBUFS);
    assert(queue.pop(out, 0) == -ENOBUFS && !out);
    assert(queue.push(frame(1, 4)) == -ENOBUFS);
    s = queue.stats();
    assert(s.closed && s.error == -ENOBUFS && s.popped == 2 && s.rejected == 2);
    assert(s.accepted - s.popped - s.discarded == s.depth && !s.bytes);
    queue.reset();
    assert(queue.push(frame(1, 1)) == 0);
    assert(queue.push(frame(1, 2)) == 0);
    assert(queue.push(frame(1, 3)) == -ENOBUFS);
    s = queue.stats();
    assert(s.discarded == 2 && s.accepted == 2 && !s.depth && s.rejected == 1);
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
    s = threaded.stats();
    assert(s.accepted == 32 && s.popped == 32 && !s.discarded && !s.rejected);
    assert(threaded.pop(out, 0) == -ECANCELED);
    queue.reset();
    assert(queue.push(frame(4, 10)) == 0 && queue.push(frame(6, 11)) == 0);
    queue.finish();
    s = queue.stats();
    assert(s.depth == 2 && s.closed && s.accepted == 2 && !s.discarded);
    assert(queue.push(frame(1, 12)) == -ECANCELED);
    assert(queue.pop(out, 0) == 1 && out->pts_us == 10);
    assert(queue.pop(out, 0) == 1 && out->pts_us == 11);
    assert(queue.pop(out, 0) == -ECANCELED);
    s = queue.stats();
    assert(s.popped == 2 && s.rejected == 1 && !s.depth);
    queue.reset();
    assert(!queue.stats().accepted && !queue.stats().peak_bytes);
    // Age is local queue residence, never the capture PTS (even UINT64_MAX).
    assert(queue.push(frame(2, UINT64_MAX)) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    s = queue.stats();
    assert(s.oldest_age_us >= 1000 && !s.max_residence_us);
    assert(queue.pop(out, 0) == 1 && queue.stats().max_residence_us >= s.oldest_age_us);
    assert(queue.push(frame(2, 0)) == 0);
    queue.close(); queue.close();
    s = queue.stats();
    assert(s.popped == 1 && s.discarded == 1 && s.accepted == 2 && !s.depth);
    queue.reset();
    assert(queue.push(frame(1, 1)) == 0);
    assert(queue.fail(-EIO) == -EIO);
    queue.close();
    s = queue.stats();
    assert(s.discarded == 1 && s.error == -EIO && !s.depth);
    queue.reset();
    assert(queue.push(frame(11, 3)) == -ENOBUFS);
    queue.finish();
    assert(queue.error() == -ENOBUFS && queue.pop(out, 0) == -ENOBUFS);
    puts("frame queue: FIFO, bounds, sticky failure, close/finish, accounting, residence and concurrency passed");
}
