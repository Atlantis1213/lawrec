#include "frame.h"
#include "frame_queue.h"
#include "media_time.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <limits>

namespace {
demo::FramePtr frame(bool key, size_t size = 5) {
    auto value = std::make_shared<demo::Frame>();
    value->bytes.resize(size, 0x65); value->key = key; return value;
}
}
int main() {
    demo::H264Headers headers;
    demo::Frame video;
    video.bytes = {0,0,0,1,0x67,0x42,0,0x1e,0xaa,0,0,1,0x68,0xce};
    assert(headers.prepare(video) == 0 && !video.key);
    video.bytes = {0,0,1,0x65,0x88,0,0,0,1,0x65,0x99};
    assert(headers.prepare(video) == 1 && video.key);
    std::vector<demo::Nal> nals;
    assert(demo::split_h264(video.bytes, nals) == 0 && nals.size() == 4);
    assert(nals[0].type == 7 && nals[1].type == 8 && nals[2].type == 5 && nals[3].type == 5);
    video.bytes = {0,0,1,0x41,0xaa}; assert(headers.prepare(video) == 1 && !video.key);
    video.bytes = {0,0,1}; assert(headers.prepare(video) < 0);
    video.bytes = {0,0,1,0xe5,0x88}; assert(headers.prepare(video) < 0);
    video.bytes = {0,0,1,0x67,0xaa}; assert(headers.prepare(video) < 0);
    headers.reset(); video.bytes = {0,0,1,0x65,0x88};
    assert(headers.prepare(video) == 1 && !video.key);
    demo::FrameQueue live(demo::QueuePolicy::LiveVideo, true, 2, 10);
    demo::FrameQueue record(demo::QueuePolicy::Record, true, 2, 10);
    live.reset(); record.reset();
    auto first = frame(true); demo::FramePtr out;
    assert(live.push(frame(false)) == 0 && !live.stats().depth);
    assert(live.push(first) == 0 && record.push(first) == 0);
    assert(live.pop(out, 0) == 1 && out == first && record.stats().depth == 1);
    assert(record.pop(out, 0) == 1 && out == first); // Same immutable object, independent reads.
    assert(live.push(first) == 0 && live.push(frame(false)) == 0);
    assert(live.push(frame(false)) == 1 && live.stats().depth == 0);
    assert(live.push(frame(false)) == 0 && live.stats().depth == 0);
    assert(live.push(first) == 0 && live.stats().depth == 1);
    assert(record.push(first) == 0 && record.push(frame(false)) == 0);
    assert(record.push(frame(false)) == -ENOBUFS && record.stats().error == -ENOBUFS);
    assert(live.stats().error == 0 && live.pop(out, 0) == 1);
    assert(record.pop(out, 0) == -ENOBUFS);
    live.reset(); live.push(first); live.finish();
    assert(live.pop(out, 0) == 1 && live.pop(out, 0) == -ECANCELED);
    live.reset(); live.push(first); live.close(); assert(live.pop(out, 0) == -ECANCELED);
    live.reset(); assert(live.push(frame(true, 11)) == -EMSGSIZE);
    demo::FrameQueue audio(demo::QueuePolicy::LiveAudio, false, 2, 10);
    audio.reset(); assert(audio.push(first) == 0 && audio.push(first) == 0 && audio.push(first) == 0);
    assert(audio.stats().depth == 2 && audio.stats().bytes == 10 && audio.stats().dropped == 1);
    demo::MediaClock clock; uint64_t mapped;
    assert(clock.map(1000000, 9000000, mapped) && mapped == 9000000);
    assert(clock.map(1040000, 99000000, mapped) && mapped == 9040000);
    assert(clock.map(980000, 99000000, mapped) && mapped == 8980000);
    clock.reset(); assert(clock.map(10, 20, mapped));
    assert(!clock.map(std::numeric_limits<uint64_t>::max(), 20, mapped));
    demo::Frame pcm; pcm.pts_us = 1000000; pcm.bytes.resize(320);
    auto slice = demo::clip_audio(pcm, 1000100, 1039999);
    assert(slice.offset == 1 && slice.samples == 319 && slice.pts_us == 1000125);
    slice = demo::clip_audio(pcm, 990000, 1000100); assert(slice.offset == 0 && slice.samples == 1);
    assert(demo::clip_audio(pcm, 1040000, 1050000).samples == 0);
    std::puts("frames: H264 access units/headers, independent bounded queues, GOP recovery and AV PTS clipping passed");
}
