#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace demo {
struct Frame {
    std::vector<uint8_t> bytes;
    uint64_t pts_us = 0;
    bool key = false; // Complete IDR access unit, including SPS and PPS.
};
using FramePtr = std::shared_ptr<const Frame>;
struct Nal { size_t offset, size; unsigned type; };
int split_h264(const std::vector<uint8_t> &bytes, std::vector<Nal> &nals);
class H264Headers {
public:
    // 1=VCL frame, 0=parameter-only, negative=invalid stream.
    int prepare(Frame &frame);
    void reset() { sps_.clear(); pps_.clear(); }
private:
    std::vector<uint8_t> sps_, pps_;
};
}
