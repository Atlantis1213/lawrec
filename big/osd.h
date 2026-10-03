#pragma once
#include "faces.h"
#include <cstdint>
#include "k_vb_comm.h"
#include "k_video_comm.h"

namespace demo {
class Osd {
public:
    int start();
    int show(const std::vector<Face> &faces);
    int clear();
    int stop();
    unsigned budget_kib() const { return pool_ == VB_INVALID_POOLID ? 0 : 480 * 800 * 4 * 2 / 1024; }
private:
    k_u32 pool_ = VB_INVALID_POOLID;
    k_vb_blk_handle blocks_[2] = {VB_INVALID_HANDLE, VB_INVALID_HANDLE};
    k_video_frame_info frames_[2]{};
    uint32_t *pixels_[2]{};
    unsigned next_ = 0;
    bool visible_ = false;
    bool submitted_ = false;
};
}
