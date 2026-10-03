#include "camera.h"
#include "config.h"
#include <cerrno>
#include <cstdio>
#include "mpi_connector_api.h"
#include "mpi_sys_api.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"
#include "mpi_vo_api.h"

namespace demo {
namespace {
int checked(const char *operation, int ret) {
    std::printf("[vision-camera] %s result=%d hex=0x%08x\n", operation, ret, static_cast<unsigned>(ret));
    return ret > 0 ? -EIO : ret;
}
constexpr unsigned align(unsigned bytes) { return (bytes + 4095) & ~4095U; }
k_mpp_chn source() { return {K_ID_VI, VICAP_DEV_ID_0, VICAP_CHN_ID_0}; }
k_mpp_chn destination() { return {K_ID_VO, K_VO_DISPLAY_DEV_ID, K_VO_DISPLAY_CHN_ID1}; }
}

int Camera::setup_buffers() {
    k_vb_config config{};
    config.max_pool_cnt = 64;
    // Preview, RGB AI and YUV encode have independent capture allocations.
    const unsigned sizes[] = {align(preview_width * preview_height * 3 / 2),
        align(video_width * video_height * 3), align(video_width * video_height * 3 / 2),
        stream_block_bytes, 2560, 5120};
    const unsigned counts[] = {capture_buffers, capture_buffers, 8, stream_buffers, 50, 25};
    uint64_t bytes = 0;
    for (unsigned i = 0; i < 6; ++i) {
        config.comm_pool[i].blk_size = sizes[i];
        config.comm_pool[i].blk_cnt = counts[i];
        config.comm_pool[i].mode = VB_REMAP_MODE_NOCACHE;
        bytes += static_cast<uint64_t>(sizes[i]) * counts[i];
        std::printf("[vision-vb] pool=%u block=%u count=%u\n", i, sizes[i], counts[i]);
    }
    vb_kib_ = static_cast<uint32_t>((bytes + 1023) / 1024);
    std::printf("[vision-vb] configured common pools=%u KiB; excludes AI tensors/OSD/SDK metadata\n", vb_kib_);
    int ret = checked("VB set config", kd_mpi_vb_set_config(&config));
    if (!ret) ret = checked("VB init", kd_mpi_vb_init());
    buffers_ = !ret;
    return ret;
}

int Camera::setup_display() {
    k_connector_info info{};
    int ret = checked("connector info", kd_mpi_get_connector_info(ST7701_V1_MIPI_2LAN_480X800_30FPS, &info));
    if (ret) return ret;
    // Exact passing application timing; kernel panel sequence/PHY stays frozen.
    info.pixclk_div = 21;
    info.phy_attr.n = 3; info.phy_attr.m = 52;
    info.phy_attr.voc = 0x1f; info.phy_attr.hs_freq = 0xb5;
    info.resolution.pclk = 27000; info.resolution.phyclk = 324000;
    info.resolution.htotal = 528; info.resolution.hdisplay = 480;
    info.resolution.hsync_len = 8; info.resolution.hback_porch = 10; info.resolution.hfront_porch = 30;
    info.resolution.vtotal = 870; info.resolution.vdisplay = 800;
    info.resolution.vsync_len = 10; info.resolution.vback_porch = 20; info.resolution.vfront_porch = 40;
    info.dsi_test_mode = 0; info.screen_test_mode = 0;
    int fd = kd_mpi_connector_open(info.connector_name);
    if (fd < 0) return checked("connector open", fd);
    ret = checked("connector power", kd_mpi_connector_power_set(fd, K_TRUE));
    if (!ret) ret = checked("connector init", kd_mpi_connector_init(fd, info));
    int close_ret = checked("connector close", kd_mpi_connector_close(fd));
    if (!ret) ret = close_ret;
    if (ret) return ret;
    display_ = true;
    k_vo_video_layer_attr layer{};
    layer.img_size.width = preview_height;
    layer.img_size.height = preview_width;
    layer.pixel_format = PIXEL_FORMAT_YVU_PLANAR_420;
    layer.stride = (preview_height / 8 - 1) | ((preview_width - 1) << 16);
    layer.func = K_ROTATION_90;
    ret = checked("preview layer attr", kd_mpi_vo_set_video_layer_attr(K_VO_LAYER1, &layer));
    if (!ret) ret = checked("preview initial off", kd_mpi_vo_disable_video_layer(K_VO_LAYER1));
    return ret;
}

int Camera::setup_capture() {
    k_vicap_sensor_info sensor{};
    int ret = checked("GC2093 info", kd_mpi_vicap_get_sensor_info(
        GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR, &sensor));
    if (ret) return ret;
    k_vicap_dev_attr device{};
    device.acq_win.width = 1920; device.acq_win.height = 1080;
    device.mode = VICAP_WORK_ONLINE_MODE;
    device.pipe_ctrl.data = 0xffffffff;
    device.pipe_ctrl.bits.af_enable = 0;
    device.pipe_ctrl.bits.ahdr_enable = 0;
    device.pipe_ctrl.bits.dnr3_enable = 0;
    device.sensor_info = sensor;
    ret = checked("VICAP device attr", kd_mpi_vicap_set_dev_attr(VICAP_DEV_ID_0, device));
    if (ret) return ret;
    for (unsigned i = 0; i < 3; ++i) {
        auto channel = static_cast<k_vicap_chn>(i);
        k_vicap_chn_attr attr{};
        attr.out_win.width = i == preview_channel ? preview_width : video_width;
        attr.out_win.height = i == preview_channel ? preview_height : video_height;
        attr.crop_win = device.acq_win; attr.scale_win = attr.out_win;
        attr.crop_enable = K_FALSE; attr.scale_enable = K_FALSE;
        attr.chn_enable = K_TRUE; attr.buffer_num = capture_buffers;
        // VICAP output and VO layer use different SDK format conventions.
        // Match sample_vicap: capture YUV semiplanar, VO layer YVU planar.
        attr.pix_format = i == rgb_channel ? PIXEL_FORMAT_RGB_888_PLANAR : PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        attr.buffer_size = align(attr.out_win.width * attr.out_win.height * 3 /
            (i == rgb_channel ? 1 : 2));
        // SDK sample reserves dump output explicitly. Only AI needs user dumps.
        kd_mpi_vicap_set_dump_reserved(VICAP_DEV_ID_0, channel, i == rgb_channel ? K_TRUE : K_FALSE);
        std::printf("[vision-capture] channel=%u %ux%u format=%d count=%u block=%u dump=%u\n",
            i, attr.out_win.width, attr.out_win.height, attr.pix_format,
            attr.buffer_num, attr.buffer_size, i == rgb_channel);
        ret = checked("VICAP channel attr", kd_mpi_vicap_set_chn_attr(VICAP_DEV_ID_0, channel, attr));
        if (ret) return ret;
    }
    // Bind before init/start, as in the SDK sample; keep this bind for capture's
    // lifetime. Preview OFF only gates its layer, never stops AI/encoder frames.
    auto src = source(), dst = destination();
    ret = checked("VI0 -> VO1 bind", kd_mpi_sys_bind(&src, &dst));
    if (ret) return ret;
    bound_ = true;
    ret = checked("VICAP database", kd_mpi_vicap_set_database_parse_mode(VICAP_DEV_ID_0, VICAP_DATABASE_PARSE_XML_JSON));
    if (!ret) ret = checked("VICAP init", kd_mpi_vicap_init(VICAP_DEV_ID_0));
    if (ret) return ret;
    capture_ = true;
    ret = checked("VICAP start", kd_mpi_vicap_start_stream(VICAP_DEV_ID_0));
    streaming_ = !ret;
    return ret;
}

int Camera::start() {
    if (buffers_ || capture_ || bound_) return -EALREADY;
    int ret = setup_buffers();
    if (!ret) ret = setup_display();
    if (!ret) ret = setup_capture();
    if (ret) stop();
    return ret;
}
int Camera::set_preview(bool enabled) {
    if (!streaming_) return -EAGAIN;
    if (enabled == preview_) return 0;
    int ret = checked(enabled ? "preview ON" : "preview OFF", enabled ?
        kd_mpi_vo_enable_video_layer(K_VO_LAYER1) : kd_mpi_vo_disable_video_layer(K_VO_LAYER1));
    if (!ret) preview_ = enabled;
    return ret;
}
int Camera::stop() {
    int error = 0;
    auto check = [&](const char *operation, int result) {
        int ret = checked(operation, result);
        if (ret && !error) error = ret;
        return ret;
    };
    if (streaming_ && !check("VICAP stop", kd_mpi_vicap_stop_stream(VICAP_DEV_ID_0))) streaming_ = false;
    if (streaming_) return error; // Do not release buffers still in use.
    if (bound_) {
        auto src = source(), dst = destination();
        if (!check("VI0 -> VO1 unbind", kd_mpi_sys_unbind(&src, &dst))) bound_ = false;
    }
    if (capture_ && !check("VICAP deinit", kd_mpi_vicap_deinit(VICAP_DEV_ID_0))) capture_ = false;
    if (display_ && !check("preview layer off", kd_mpi_vo_disable_video_layer(K_VO_LAYER1))) {
        display_ = false; preview_ = false;
    }
    if (!bound_ && !capture_ && !display_ && buffers_ && !check("VB exit", kd_mpi_vb_exit())) buffers_ = false;
    return error;
}
}
