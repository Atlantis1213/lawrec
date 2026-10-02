#include "BasicUsageEnvironment.hh"
#include "h264LiveFrameSource.h"
#include "g711LiveFrameSource.h"
#include <cassert>
#include <cstdio>
#include <vector>

class VideoSource : public H264LiveFrameSource {
public:
    VideoSource(UsageEnvironment &env, size_t count) : H264LiveFrameSource(env, count) { stopReader(); }
    int ingest(const std::vector<uint8_t> &data, uint64_t pts = 1000000) {
        pushData(data.data(), data.size(), pts);
        return getFrame();
    }
    size_t units() { std::lock_guard<std::mutex> lock(fMutex); return fFrameQueue.size(); }
    size_t retained() { std::lock_guard<std::mutex> lock(fMutex); return fFrameBytes; }
};

class AudioSource : public G711LiveFrameSource {
public:
    AudioSource(UsageEnvironment &env) : G711LiveFrameSource(env, 2) { stopReader(); }
    void ingest() {
        const uint8_t packet[320]{};
        pushData(packet, sizeof(packet), 1000000);
        getFrame();
    }
};

struct ReadResult {
    unsigned bytes = 0, truncated = 0;
    bool closed = false;
};
static void received(void *opaque, unsigned bytes, unsigned truncated, timeval, unsigned) {
    auto &result = *static_cast<ReadResult *>(opaque);
    result.bytes = bytes; result.truncated = truncated;
}
static void closed(void *opaque) { static_cast<ReadResult *>(opaque)->closed = true; }
static ReadResult read(LiveFrameSource *source, std::vector<uint8_t> &buffer) {
    ReadResult result;
    source->getNextFrame(buffer.data(), buffer.size(), received, &result, closed, &result);
    if (!result.bytes && !result.closed) source->stopGettingFrames();
    return result;
}

struct AsyncRead {
    LiveFrameSource *source;
    std::vector<uint8_t> buffer{std::vector<uint8_t>(4 * 1024 * 1024)};
    unsigned packets = 0;
    volatile char done = 0;
    bool timeout = false, closed = false;
};
static void async_closed(void *opaque) {
    auto &r = *static_cast<AsyncRead *>(opaque);
    r.closed = true; r.done = 1;
}
static void async_received(void *opaque, unsigned bytes, unsigned truncated, timeval, unsigned) {
    auto &r = *static_cast<AsyncRead *>(opaque);
    const unsigned expected[] = {7, 8, 7, 8, 5};
    assert(bytes && !truncated && r.packets < 5 && (r.buffer[0] & 31) == expected[r.packets]);
    if (++r.packets == 5) r.done = 1;
    else r.source->getNextFrame(r.buffer.data(), r.buffer.size(), async_received, &r, async_closed, &r);
}
static void async_timeout(void *opaque) {
    auto &r = *static_cast<AsyncRead *>(opaque);
    r.timeout = true; r.done = 1;
}
static std::vector<uint8_t> nal(unsigned type, size_t size = 8) {
    assert(size >= 5);
    std::vector<uint8_t> bytes(size, 0x33);
    bytes[0] = bytes[1] = bytes[2] = 0; bytes[3] = 1; bytes[4] = type;
    return bytes;
}
static std::vector<uint8_t> key(unsigned slices = 1, size_t slice_size = 8) {
    auto bytes = nal(0x67);
    auto append = [&](std::vector<uint8_t> more) { bytes.insert(bytes.end(), more.begin(), more.end()); };
    append(nal(0x68));
    for (unsigned i = 0; i < slices; ++i) append(nal(0x65, slice_size));
    return bytes;
}
int main() {
    auto *scheduler = BasicTaskScheduler::createNew();
    assert(scheduler);
    auto *env = BasicUsageEnvironment::createNew(*scheduler);
    assert(env);
    auto *source = new VideoSource(*env, 8);
    // The encoder accepts up to 4 MiB. Large IDRs must not silently disappear.
    auto bytes = key(1, 600 * 1024);
    assert(source->ingest(bytes) == (int)bytes.size());
    std::vector<uint8_t> output(4 * 1024 * 1024);
    bool idr = false;
    for (unsigned i = 0; i < 5; ++i) {
        ReadResult result = read(source, output);
        assert(result.bytes && !result.truncated && !result.closed);
        idr = idr || (output[0] & 31) == 5;
    }
    assert(idr);
    assert(source->bufferStats().nal_deliveries == 5);
    Medium::close(source);

    source = new VideoSource(*env, 2);
    bytes = key(12);
    assert(source->ingest(bytes) == (int)bytes.size() && source->units() == 1);
    // The frame has 16 NAL packets but consumes one access-unit queue slot.
    unsigned idrs = 0;
    for (unsigned i = 0; i < 16; ++i) {
        auto result = read(source, output);
        assert(result.bytes && !result.truncated);
        if ((output[0] & 31) == 5) ++idrs;
    }
    assert(idrs == 12 && source->units() == 0 && !source->retained());
    source->ingest(key()); source->ingest(nal(0x41));
    auto count = source->frameCount();
    source->ingest(nal(0x41));
    assert(!source->units() && !source->deliveryError() && source->frameCount() == count);
    assert(source->bufferStats().waiting_idr && source->bufferStats().dropped_units >= 3);
    source->ingest(nal(0x41)); assert(!source->units());
    source->ingest(key()); assert(source->units() == 1);
    auto config = read(source, output);
    assert(config.bytes && (output[0] & 31) == 7); // Recovery starts with SPS.
    Medium::close(source);

    source = new VideoSource(*env, 8);
    bytes = key(1, 3 * 1024 * 1024);
    source->ingest(bytes); source->ingest(bytes);
    auto retained = source->retained();
    assert(retained > 6 * 1024 * 1024 && source->units() == 2);
    read(source, output);
    assert(source->retained() == retained); // Partial NAL consumption still retains the full buffer.
    source->ingest(bytes);
    assert(source->retained() <= LiveFrameSource::kQueueBytes && source->units() == 1);
    Medium::close(source);

    source = new VideoSource(*env, 8);
    bytes.resize(3 * 1024 * 1024);
    assert(!source->pushData(bytes.data(), bytes.size(), 1));
    assert(!source->pushData(bytes.data(), bytes.size(), 2));
    assert(source->pushData(bytes.data(), bytes.size(), 3) == -ENOBUFS);
    assert(!source->frameCount());
    assert(source->bufferStats().raw_units == 2 && source->bufferStats().raw_bytes == 6 * 1024 * 1024);
    Medium::close(source);

    source = new VideoSource(*env, 8);
    source->ingest(key(129));
    assert(source->deliveryError() == -E2BIG && !source->units() && !source->frameCount());
    Medium::close(source);
    source = new VideoSource(*env, 8);
    source->ingest(std::vector<uint8_t>(32, 0x55));
    assert(source->deliveryError() == -EBADMSG && !source->frameCount());
    Medium::close(source);
    source = new VideoSource(*env, 8);
    source->ingest(key());
    std::vector<uint8_t> tiny(1);
    auto rejected = read(source, tiny);
    assert(rejected.closed && !rejected.bytes && source->deliveryError() == -EMSGSIZE);
    Medium::close(source);
    source = new VideoSource(*env, 8);
    bytes.resize(LiveFrameSource::kMaxFrameBytes + 1);
    assert(source->pushData(bytes.data(), bytes.size(), 1) == -EMSGSIZE);
    assert(source->pushData(nullptr, 0, 1) == -EMSGSIZE); // First error is sticky.
    Medium::close(source);

    auto *audio = new AudioSource(*env);
    audio->ingest(); audio->ingest(); audio->ingest();
    assert(audio->deliveryError() == -ENOBUFS && audio->frameCount() == 2);
    Medium::close(audio);
    assert(!H264LiveFrameSource::createNew(*env, 0));

    auto *threaded = H264LiveFrameSource::createNew(*env, 8);
    assert(threaded);
    AsyncRead asynchronous{threaded};
    threaded->getNextFrame(asynchronous.buffer.data(), asynchronous.buffer.size(),
                           async_received, &asynchronous, async_closed, &asynchronous);
    bytes = key(1, 600 * 1024);
    assert(!threaded->pushData(bytes.data(), bytes.size(), 1234000));
    std::fill(bytes.begin(), bytes.end(), 0); // SDK/producer memory is gone before asynchronous delivery.
    TaskToken deadline = scheduler->scheduleDelayedTask(1000000, async_timeout, &asynchronous);
    scheduler->doEventLoop(&asynchronous.done);
    scheduler->unscheduleDelayedTask(deadline);
    threaded->stopGettingFrames(); threaded->stopReader();
    assert(!asynchronous.timeout && !asynchronous.closed && asynchronous.packets == 5 &&
           !threaded->deliveryError() && threaded->frameCount() == 1 && !threaded->getAuxLine().empty());
    Medium::close(threaded);
    assert(env->reclaim());
    delete scheduler;
    puts("rtsp source: real live555 event loop/reader, owned large/multi-slice IDR, complete-unit recovery, byte bounds and explicit errors passed");
}
