#include "osd.h"
#include "config.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "mpi_vb_api.h"
#include "mpi_sys_api.h"
#include "mpi_vo_api.h"

namespace demo {
namespace {
constexpr unsigned width = 480, height = 800, bytes = width * height * 4;
constexpr k_vo_osd layer = K_VO_OSD2;
int check(const char *operation, int ret) {
    if (ret) std::printf("[vision-osd] %s error=%d\n", operation, ret);
    return ret > 0 ? -EIO : ret;
}
void pixel(uint32_t *buffer, int x, int y, uint32_t color) {
    if (x >= 0 && x < int(width) && y >= 0 && y < int(height)) buffer[y * width + x] = color;
}
void box(uint32_t *buffer, const Face &f) {
    int x1 = std::clamp(int(f.x1), 0, int(width - 1)), x2 = std::clamp(int(f.x2), 0, int(width - 1));
    int y1 = std::clamp(int(f.y1), 0, int(height - 1)), y2 = std::clamp(int(f.y2), 0, int(height - 1));
    for (int thickness = 0; thickness < 2; ++thickness) {
        for (int x = x1; x <= x2; ++x) { pixel(buffer, x, y1 + thickness, 0xff32df8b); pixel(buffer, x, y2 - thickness, 0xff32df8b); }
        for (int y = y1; y <= y2; ++y) { pixel(buffer, x1 + thickness, y, 0xff32df8b); pixel(buffer, x2 - thickness, y, 0xff32df8b); }
    }
    for (auto p : f.landmarks) for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x)
        pixel(buffer, int(p.x) + x, int(p.y) + y, 0xffffbc47);
}
void digit(uint32_t *buffer, unsigned number, int start_x) {
    static constexpr uint8_t glyph[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}};
    for (unsigned y = 0; y < 7; ++y) for (unsigned x = 0; x < 5; ++x)
        if (glyph[number % 10][y] & (1U << (4 - x)))
            for (unsigned dy = 0; dy < 3; ++dy) for (unsigned dx = 0; dx < 3; ++dx)
                pixel(buffer, start_x + x * 3 + dx, 160 + y * 3 + dy, 0xff32df8b);
}
}
int Osd::start() {
    if (pool_ != VB_INVALID_POOLID) return -EALREADY;
    k_vb_pool_config config{};
    config.blk_size = bytes; config.blk_cnt = 2; config.mode = VB_REMAP_MODE_NOCACHE;
    pool_ = kd_mpi_vb_create_pool(&config);
    if (pool_ == VB_INVALID_POOLID) return -ENOMEM;
    int ret = 0;
    for (unsigned i = 0; i < 2 && !ret; ++i) {
        blocks_[i] = kd_mpi_vb_get_block(pool_, bytes, nullptr);
        if (blocks_[i] == VB_INVALID_HANDLE) { ret = -ENOMEM; break; }
        auto &frame = frames_[i];
        frame.pool_id = pool_; frame.mod_id = K_ID_VO;
        frame.v_frame.width = width; frame.v_frame.height = height; frame.v_frame.stride[0] = width;
        frame.v_frame.pixel_format = PIXEL_FORMAT_ARGB_8888;
        frame.v_frame.phys_addr[0] = kd_mpi_vb_handle_to_phyaddr(blocks_[i]);
        if (!frame.v_frame.phys_addr[0]) { ret = -EIO; break; }
        pixels_[i] = static_cast<uint32_t *>(kd_mpi_sys_mmap(frame.v_frame.phys_addr[0], bytes));
        if (!pixels_[i]) { ret = -ENOMEM; break; }
        std::memset(pixels_[i], 0, bytes);
    }
    if (!ret) {
        k_vo_video_osd_attr attr{};
        attr.img_size.width = width; attr.img_size.height = height;
        attr.pixel_format = PIXEL_FORMAT_ARGB_8888; attr.stride = width * 4 / 8;
        attr.global_alptha = 0xff;
        ret = check("attribute", kd_mpi_vo_set_video_osd_attr(layer, &attr));
    }
    if (ret) stop();
    else std::printf("[vision-osd] SDK OSD2 / insert CHN5, 2x480x800 ARGB buffers=%u KiB; no Linux OSD4..7 use\n", bytes * 2 / 1024);
    return ret;
}
int Osd::show(const std::vector<Face> &faces) {
    if (!pixels_[next_]) return -EAGAIN;
    if (faces.size() > max_faces) return -EOVERFLOW;
    std::memset(pixels_[next_], 0, bytes);
    for (auto face : faces) box(pixels_[next_], portrait_face(face, video_width, video_height));
    digit(pixels_[next_], unsigned(faces.size()) / 10, 24);
    digit(pixels_[next_], unsigned(faces.size()) % 10, 44);
    // Even a failed insertion/enable may have handed this buffer to hardware.
    submitted_ = true;
    int ret = check("insert", kd_mpi_vo_chn_insert_frame(unsigned(layer) + 3, &frames_[next_]));
    if (!ret && !visible_) {
        ret = check("enable", kd_mpi_vo_osd_enable(layer));
        if (!ret) visible_ = true;
    }
    if (!ret) next_ = 1 - next_;
    return ret;
}
int Osd::clear() {
    if (!visible_ && !submitted_) return 0;
    int ret = check("disable", kd_mpi_vo_osd_disable(layer));
    if (!ret) { visible_ = false; submitted_ = false; }
    return ret;
}
int Osd::stop() {
    int error = clear();
    if (error) return error; // Do not unmap/free memory still scanned by VO.
    for (unsigned i = 0; i < 2; ++i) {
        if (pixels_[i]) {
            int ret = check("unmap", kd_mpi_sys_munmap(pixels_[i], bytes));
            if (ret && !error) error = ret;
            if (!ret) pixels_[i] = nullptr;
        }
        if (!pixels_[i] && blocks_[i] != VB_INVALID_HANDLE) {
            int ret = check("release", kd_mpi_vb_release_block(blocks_[i]));
            if (ret && !error) error = ret;
            if (!ret) blocks_[i] = VB_INVALID_HANDLE;
        }
    }
    if (pool_ != VB_INVALID_POOLID && blocks_[0] == VB_INVALID_HANDLE && blocks_[1] == VB_INVALID_HANDLE) {
        int ret = check("destroy pool", kd_mpi_vb_destory_pool(pool_));
        if (ret && !error) error = ret;
        if (!ret) pool_ = VB_INVALID_POOLID;
    }
    return error;
}
}
