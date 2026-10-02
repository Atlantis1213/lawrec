#include <iostream>
#include <mutex>
#include <system_error>
#include "LiveFrameSource.h"

LiveFrameSource* LiveFrameSource::createNew(UsageEnvironment &env, size_t queue_size) {
    return new LiveFrameSource(env, queue_size);
}

LiveFrameSource::LiveFrameSource(UsageEnvironment &env, size_t queue_size) : FramedSource(env), fQueueSize(queue_size) {
    if (!queue_size) { fail(-EINVAL); return; }
    fEventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0);
    if (!fEventTriggerId) { fail(-ENOMEM); return; }
    try {
    fThread = std::thread([this](){
        try {
        while(this->fNeedReadFrame && !fError.load()) {
            this->getFrame();
            usleep(1000 * 10);
        }
        } catch (...) {
            fail(-ENOMEM);
            fNeedReadFrame.store(false);
        }
    });
    } catch (const std::system_error &error) {
        fail(error.code().value() > 0 ? -error.code().value() : -EAGAIN);
    }
}

int LiveFrameSource::fail(int error) {
    int expected = 0;
    if (fError.compare_exchange_strong(expected, error))
        std::cerr << "[rtsp-source] delivery failed=" << error << std::endl;
    return fError.load();
}

LiveFrameSource::BufferStats LiveFrameSource::bufferStats() {
    BufferStats stats;
    {
        std::lock_guard<std::mutex> lock(fMutexRaw);
        stats.raw_units = fRawDataQueue.size(); stats.raw_bytes = fRawBytes;
    }
    {
        std::lock_guard<std::mutex> lock(fMutex);
        stats.ready_units = fFrameQueue.size(); stats.retained_bytes = fFrameBytes;
        stats.dropped_units = fCongestionDrops; stats.waiting_idr = fWaitKey;
    }
    stats.nal_deliveries = fNalDeliveries.load();
    return stats;
}

int LiveFrameSource::pushData(const uint8_t *data, size_t data_size, uint64_t timestamp) {
    if (fError.load()) return fError.load();
    if (!data || !data_size) return fail(-EINVAL);
    if (data_size > kMaxFrameBytes) return fail(-EMSGSIZE);
    std::unique_lock<std::mutex> lck(fMutexRaw);
    if (fRawDataQueue.size() >= 32 || data_size > kQueueBytes - fRawBytes)
        return fail(-ENOBUFS);
    std::shared_ptr<uint8_t> buf = make_shared_array<uint8_t>(data_size);
    memcpy(buf.get(), data, data_size); 
    RawData raw_data;
    raw_data.buffer_ = buf;
    raw_data.size_ = data_size;
    raw_data.timestamp_ = timestamp;
    
    fRawDataQueue.push_back(raw_data);
    fRawBytes += data_size;
    return 0;
}

int LiveFrameSource::getFrame() {
    RawData raw_data;
    std::unique_lock<std::mutex> lck(fMutexRaw);
    if (!fRawDataQueue.empty()) {
        raw_data = fRawDataQueue.front();
        fRawDataQueue.pop_front();
        fRawBytes -= raw_data.size_;
    }
    lck.unlock();

    int frameSize = 0;
    if (raw_data.buffer_ && raw_data.size_) {
        // Producer already mapped SDK PTS onto the shared RTSP wall clock.
        struct timeval ref{};
        ref.tv_sec = raw_data.timestamp_ / 1000000;
        ref.tv_usec = raw_data.timestamp_ % 1000000;
        frameSize = raw_data.size_;
        processFrame(raw_data.buffer_, frameSize, ref);
    }
    return frameSize;
}

void LiveFrameSource::processFrame(std::shared_ptr<uint8_t> data, size_t size, const struct timeval &ref) {
    std::lock_guard<std::mutex> guard(fParseMutex);
    if (fError.load()) return;
    std::list<FramePacket> packetList = this->parseFrame(data, size, ref);
    if (packetList.empty() || fError.load()) return;
    if (packetList.size() > kMaxFramePackets) { fail(-E2BIG); return; }
    bool h264 = GetEncodeType() == EncodeType::H264, key = false;
    size_t retained = size;
    for (const auto &p : packetList) {
        if (!p.buffer_ || !p.size_ || p.size_ > kMaxFrameBytes) { fail(-EBADMSG); return; }
        if (h264 && (p.buffer_.get()[p.offset_] & 31) == 5) key = true;
        // SPS/PPS copies may be repeated; overcounting is safe and bounded.
        if (p.buffer_.get() != data.get()) {
            if (p.size_ > kQueueBytes - retained) { fail(-EMSGSIZE); return; }
            retained += p.size_;
        }
    }
    if (retained > kQueueBytes) { fail(-EMSGSIZE); return; }
    std::unique_lock<std::mutex> lck(fMutex);
    if (fFrameQueue.size() >= fQueueSize || retained > kQueueBytes - fFrameBytes) {
        if (GetEncodeType() == EncodeType::G711A) { fail(-ENOBUFS); return; }
        fCongestionDrops += fFrameQueue.size();
        fFrameQueue.clear(); fFrameBytes = 0;
        fWaitKey = h264;
        if (fCongestionDrops <= fQueueSize || fCongestionDrops % 64 < fQueueSize)
            std::cerr << "[rtsp-source] congestion dropped_units=" << fCongestionDrops
                      << " wait_idr=" << fWaitKey << std::endl;
    }
    if (fWaitKey && !key) { ++fCongestionDrops; return; }
    if (key) fWaitKey = false;
    // Publish the entire parsed access unit, never only the first few NALs.
    fFrameQueue.push_back(FrameBatch{std::move(packetList), retained});
    fFrameBytes += retained;
    ++fAcceptedFrames;
    lck.unlock();

    // post an event to ask to deliver the frame
    envir().taskScheduler().triggerEvent(fEventTriggerId, this);
}

 void LiveFrameSource::doGetNextFrame() {
    deliverFrame();
 }

void LiveFrameSource::doStopGettingFrames() {
    FramedSource::doStopGettingFrames();
}

LiveFrameSource::~LiveFrameSource() {
    stopReader();
    fFrameQueue.clear();
    fRawDataQueue.clear();
    if(fEventTriggerId) {
        envir().taskScheduler().deleteEventTrigger(fEventTriggerId);
        fEventTriggerId = 0;
    }
}

std::list<LiveFrameSource::FramePacket>
LiveFrameSource::parseFrame(std::shared_ptr<uint8_t> frame_data, size_t size, const struct timeval &ref) {
    std::list<FramePacket> frameList;
    if (frame_data != NULL) {
        FramePacket packet(frame_data, 0, size, ref);
        frameList.push_back(packet);
    } else {
        std::cout << "LiveFrameSource::parseFrame  frame empty" << std::endl;
    }
    return frameList;
}


void LiveFrameSource::deliverFrame0(void *clientData) {
    ((LiveFrameSource*)clientData)->deliverFrame();
}

void LiveFrameSource::deliverFrame() {
    if (isCurrentlyAwaitingData()) {
        fDurationInMicroseconds = 0;
        fNumTruncatedBytes = 0;
        fFrameSize = 0;

        FramePacket packet;
        std::unique_lock<std::mutex> lck(fMutex);
        if (!fFrameQueue.empty()) {
            auto &batch = fFrameQueue.front();
            packet = batch.packets.front();
            batch.packets.pop_front();
            if (batch.packets.empty()) {
                fFrameBytes -= batch.retained_bytes;
                fFrameQueue.pop_front();
            }
        }
        lck.unlock();

        if(packet.size_) {
            if (packet.size_ > fMaxSize) {
                fail(-EMSGSIZE);
                handleClosure();
                return;
            } else {
                fFrameSize = packet.size_;
            }
            fPresentationTime = packet.timestamp_;
            memcpy(fTo, packet.buffer_.get() + packet.offset_, fFrameSize);
        }

        if (fFrameSize > 0) {
            ++fNalDeliveries;
            FramedSource::afterGetting(this);
        }
    }
}
