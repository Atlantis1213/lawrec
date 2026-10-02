#ifndef _LIVEFRAMESOURCE_H
#define _LIVEFRAMESOURCE_H

#include "FramedSource.hh"
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cerrno>

enum class EncodeType {
   INVALID = 0,
   H264 = 1,
   H265 = 2,
   G711A = 100,
   BOTTOM
};

template <typename T>
std::shared_ptr<T> make_shared_array(size_t size) {
    return std::shared_ptr<T>(new T[size], std::default_delete<T[]>());
}

class LiveFrameSource : public FramedSource {
  public:
    static LiveFrameSource *createNew(UsageEnvironment &env, size_t queue_size);
    static constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024;
    static constexpr size_t kQueueBytes = 8 * 1024 * 1024;
    static constexpr size_t kMaxFramePackets = 132;
    int pushData(const uint8_t *data, size_t data_size, uint64_t timestamp);
    std::string getAuxLine() { std::lock_guard<std::mutex> guard(fParseMutex); return fAuxLine; }
    int deliveryError() const { return fError.load(); }
    unsigned long frameCount() const { return fAcceptedFrames.load(); }
    struct BufferStats {
        size_t raw_units = 0, raw_bytes = 0, ready_units = 0, retained_bytes = 0;
        uint64_t dropped_units = 0, nal_deliveries = 0;
        bool waiting_idr = false;
    };
    BufferStats bufferStats();
    void stopReader() {
        fNeedReadFrame.store(false);
        if (fThread.joinable()) fThread.join();
    }
    virtual EncodeType GetEncodeType() { return EncodeType::INVALID;}

  public:
    struct FramePacket {
      FramePacket() = default;
      FramePacket(std::shared_ptr<uint8_t> buffer, size_t offset, size_t size, struct timeval timestamp) :
        buffer_(buffer), offset_(offset), size_(size), timestamp_(timestamp) {}
      std::shared_ptr<uint8_t> buffer_{nullptr};
      size_t offset_{0};
      size_t size_{0};
      struct timeval timestamp_{};
    };

    struct RawData {
        std::shared_ptr<uint8_t> buffer_{nullptr};
        size_t size_{0};
        uint64_t timestamp_{0};
    };

  protected:
    LiveFrameSource(UsageEnvironment &env, size_t queue_size);
    virtual ~LiveFrameSource();

    virtual void doGetNextFrame();
    virtual void doStopGettingFrames();
    
    virtual unsigned maxFrameSize() const { 
      return kMaxFrameBytes;
    }
    static void deliverFrame0(void *clientData);
    void deliverFrame();

    int getFrame(); 
    void processFrame(std::shared_ptr<uint8_t> data, size_t size, const struct timeval &ref);
    virtual std::list<FramePacket> parseFrame(std::shared_ptr<uint8_t> data, size_t size, const struct timeval &ref);
    int fail(int error);
  
  protected:
    struct FrameBatch {
        std::list<FramePacket> packets;
        size_t retained_bytes;
    };
    // Keep an access unit and its backing buffers together, even while its
    // first NALs have been consumed. Count retained memory, not remaining NALs.
    std::list<FrameBatch> fFrameQueue;
    std::list<RawData> fRawDataQueue;
    size_t fFrameBytes = 0, fRawBytes = 0;
    uint64_t fCongestionDrops = 0;
    EventTriggerId fEventTriggerId = 0;
    size_t fQueueSize;

    std::thread fThread;
    std::mutex fMutex;
    std::mutex fMutexRaw;
    std::mutex fParseMutex;
    std::atomic<int> fError{0};
    std::atomic<unsigned long> fAcceptedFrames{0};
    std::atomic<uint64_t> fNalDeliveries{0};
    bool fWaitKey{false};
    std::atomic<bool> fNeedReadFrame{true};
    std::string fAuxLine;
};

#endif  // _LIVEFRAMESOURCE_H
