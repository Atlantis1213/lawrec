// Narrow ownership/order fixture, not a camera/VO simulator.
#include "camera.h"
#include "config.h"
#include "mpi_connector_api.h"
#include "mpi_sys_api.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"
#include "mpi_vo_api.h"
#include <cassert>
#include <cstdio>
#include <cerrno>
#include <unistd.h>

namespace {
bool vb_live, bound, initialized, streaming;
int starts, stops, enables, disables;
int start_error, stop_error, attr_error;
int connector_error, close_error, closes;
k_vb_config pools;
bool dump_reserved[3];
}
extern "C" {
k_s32 kd_mpi_vb_set_config(const k_vb_config *config) {
    pools = *config;
    assert(pools.comm_pool[1].blk_size >= demo::video_width * demo::video_height * 3);
    assert(pools.comm_pool[2].blk_size >= demo::video_width * demo::video_height * 3 / 2);
    assert(pools.comm_pool[3].blk_size == demo::stream_block_bytes);
    assert(pools.comm_pool[3].blk_cnt == demo::stream_buffers);
    return 0;
}
k_s32 kd_mpi_vb_init() { assert(!vb_live); vb_live = true; return 0; }
k_s32 kd_mpi_vb_exit() { assert(!streaming && !initialized && !bound); vb_live = false; return 0; }
k_s32 kd_mpi_get_connector_info(k_connector_type, k_connector_info *) { return 0; }
k_s32 kd_mpi_connector_open(const char *) { return 3; }
k_s32 kd_mpi_connector_power_set(k_s32, k_bool) { return 0; }
k_s32 kd_mpi_connector_close(k_s32) {
    assert(false && "SDK connector close has an undefined return value; use close");
    return -4096;
}
int close(int fd) noexcept {
    assert(fd == 3);
    ++closes;
    errno = close_error;
    return close_error ? -1 : 0;
}
k_s32 kd_mpi_connector_init(k_s32, k_connector_info info) {
    assert(info.pixclk_div == 21 && info.phy_attr.m == 52 && info.phy_attr.n == 3);
    assert(info.phy_attr.voc == 0x1f && info.phy_attr.hs_freq == 0xb5);
    assert(info.resolution.pclk == 27000 && info.resolution.phyclk == 324000);
    assert(!info.dsi_test_mode && !info.screen_test_mode);
    return connector_error;
}
k_s32 kd_mpi_vo_set_video_layer_attr(k_vo_layer layer, k_vo_video_layer_attr *attr) {
    assert(layer == K_VO_LAYER1 && attr->img_size.width == 480 && attr->img_size.height == 800);
    assert(attr->func == K_ROTATION_90 && attr->pixel_format == PIXEL_FORMAT_YVU_PLANAR_420);
    return 0;
}
k_s32 kd_mpi_vo_enable_video_layer(k_vo_layer) { ++enables; return 0; }
k_u8 kd_mpi_vo_enable() { return 0; }
k_s32 kd_mpi_vo_disable_video_layer(k_vo_layer) { ++disables; return 0; }
k_s32 kd_mpi_vicap_get_sensor_info(k_vicap_sensor_type type, k_vicap_sensor_info *) {
    assert(type == GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR); return 0;
}
k_s32 kd_mpi_vicap_set_dev_attr(k_vicap_dev, k_vicap_dev_attr attr) {
    assert(attr.acq_win.width == 1920 && attr.acq_win.height == 1080); return 0;
}
void kd_mpi_vicap_set_dump_reserved(k_vicap_dev, k_vicap_chn channel, k_bool reserved) {
    dump_reserved[channel] = reserved;
}
k_s32 kd_mpi_vicap_set_chn_attr(k_vicap_dev, k_vicap_chn channel, k_vicap_chn_attr attr) {
    assert(attr.chn_enable && attr.buffer_num == demo::capture_buffers);
    assert(attr.buffer_size <= pools.comm_pool[channel].blk_size);
    if (channel == 0) assert(attr.pix_format == PIXEL_FORMAT_YUV_SEMIPLANAR_420 && !dump_reserved[0]);
    if (channel == 1) assert(attr.pix_format == PIXEL_FORMAT_RGB_888_PLANAR && dump_reserved[1]);
    if (channel == 2) assert(attr.pix_format == PIXEL_FORMAT_YUV_SEMIPLANAR_420 && !dump_reserved[2]);
    return channel == 2 ? attr_error : 0;
}
k_s32 kd_mpi_sys_bind(k_mpp_chn *src, k_mpp_chn *dst) {
    assert(streaming && src->mod_id == K_ID_VI && src->chn_id == 0 && dst->chn_id == 1);
    bound = true; return 0;
}
k_s32 kd_mpi_sys_unbind(k_mpp_chn *, k_mpp_chn *) { bound = false; return 0; }
k_s32 kd_mpi_vicap_set_database_parse_mode(k_vicap_dev, k_vicap_database_parse_mode) { return 0; }
k_s32 kd_mpi_vicap_init(k_vicap_dev) { assert(!bound); initialized = true; return 0; }
k_s32 kd_mpi_vicap_start_stream(k_vicap_dev) { ++starts; streaming = !start_error; return start_error; }
k_s32 kd_mpi_vicap_stop_stream(k_vicap_dev) { ++stops; if (!stop_error) streaming = false; return stop_error; }
k_s32 kd_mpi_vicap_deinit(k_vicap_dev) { assert(!streaming); initialized = false; return 0; }
}
int main() {
    demo::Camera camera;
    assert(camera.start() == 0 && streaming && vb_live && !bound);
    assert(closes == 1);
    assert(camera.vb_budget_kib() > 0);
    assert(camera.set_preview(true) == 0 && enables == 1);
    assert(camera.set_preview(true) == 0 && enables == 1);
    assert(camera.set_preview(false) == 0 && streaming && !bound && stops == 0);
    stop_error = -9;
    assert(camera.stop() == -9 && streaming && vb_live && !bound);
    stop_error = 0;
    assert(camera.stop() == 0 && !streaming && !vb_live && !bound);
    int stop_count = stops;
    start_error = -8;
    assert(camera.start() == -8 && !vb_live && !initialized && !bound);
    assert(stops == stop_count);
    start_error = 0; attr_error = -7;
    assert(camera.start() == -7 && !vb_live && !bound);
    assert(camera.stop() == 0);
    attr_error = 0; close_error = EIO;
    assert(camera.start() == -EIO && !vb_live && !bound);
    connector_error = -6;
    assert(camera.start() == -6 && !vb_live && !bound);
    connector_error = close_error = 0;
    assert(camera.start() == 0 && streaming && vb_live && !bound);
    assert(camera.stop() == 0 && !vb_live && !bound);
    std::puts("camera MOCK ONLY: independent RGB/YUV, preview bind gating, cleanup passed");
}
