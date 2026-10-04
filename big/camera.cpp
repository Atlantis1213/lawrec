#include "camera.h"
#include "config.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
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
void read_registers(const char *name, uint64_t base, const unsigned *offsets, unsigned count) {
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { std::printf("[vision-vo] %s mem open failed errno=%d\n", name, errno); return; }
    void *mapping = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(base));
    close(fd);
    if (mapping == MAP_FAILED || !mapping) {
        std::printf("[vision-vo] %s map failed errno=%d\n", name, errno);
        return;
    }
    auto *regs = static_cast<volatile uint32_t *>(mapping);
    for (unsigned i = 0; i < count; ++i)
        std::printf("[vision-vo] %s +0x%x = 0x%08x\n", name, offsets[i], regs[offsets[i] / 4]);
    munmap(mapping, 4096);
}
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
    display_ = !ret;
    // SDK kd_mpi_connector_close() calls close() but omits its return value.
    int close_ret = ::close(fd);
    close_ret = checked("connector close", close_ret ? -errno : 0);
    if (!ret) ret = close_ret;
    if (ret) return ret;
    k_vo_video_layer_attr layer{};
    layer.img_size.width = preview_height;
    layer.img_size.height = preview_width;
    layer.pixel_format = PIXEL_FORMAT_YVU_PLANAR_420;
    layer.stride = (preview_height / 8 - 1) | ((preview_width - 1) << 16);
    layer.func = K_ROTATION_90;
    ret = checked("preview layer attr", kd_mpi_vo_set_video_layer_attr(K_VO_LAYER1, &layer));
    if (!ret) ret = checked("preview initial off", kd_mpi_vo_disable_video_layer(K_VO_LAYER1));
    // Connector enabled VO before layer setup; commit the final layer config.
    if (!ret) ret = checked("VO layer config commit", kd_mpi_vo_enable());
    if (!ret) ret = checked("VO frame-end sync", sync_.enable());
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
    // An inactive VO consumer must not retain capture frames in its queue.
    // Bind only while preview is requested; RGB/encode capture stays live.
    ret = checked("VICAP database", kd_mpi_vicap_set_database_parse_mode(VICAP_DEV_ID_0, VICAP_DATABASE_PARSE_XML_JSON));
    if (!ret) ret = checked("VICAP init", kd_mpi_vicap_init(VICAP_DEV_ID_0));
    if (ret) return ret;
    capture_ = true;
    ret = checked("VICAP start", kd_mpi_vicap_start_stream(VICAP_DEV_ID_0));
    streaming_ = !ret;
    return ret;
}

int Camera::start() {
    if (buffers_ || capture_ || bound_ || sync_.active()) return -EALREADY;
    int ret = setup_buffers();
    if (!ret) ret = setup_display();
    if (!ret) ret = setup_capture();
    if (ret) stop();
    return ret;
}
void Camera::log_buffers() const {
    const unsigned vo[] = {0x3e0, 0x3e4, 0x3ec};
    read_registers("VO irq", 0x90840000ULL, vo, 3);
    const char *paths[] = {"/proc/umap/vo", "/proc/umap/vb"};
    for (const char *path : paths) {
        FILE *report = std::fopen(path, "r");
        if (!report) {
            std::printf("[vision-vb] %s unavailable errno=%d\n", path, errno);
            continue;
        }
        // RT-Smart proc callbacks may print directly instead of filling read().
        std::printf("[vision-vb] live report %s (serial v refreshes)\n", path);
        char line[512];
        while (std::fgets(line, sizeof(line), report)) std::fputs(line, stdout);
        std::fclose(report);
    }
}
int Camera::set_preview(bool enabled) {
    if (!streaming_) return -EAGAIN;
    if (enabled == preview_ && bound_ == enabled) return 0;
    auto src = source(), dst = destination();
    int ret = 0;
    if (enabled) {
        ret = checked("preview layer ON", kd_mpi_vo_enable_video_layer(K_VO_LAYER1));
        if (!ret) ret = checked("VI0 -> VO1 bind", kd_mpi_sys_bind(&src, &dst));
        if (!ret) bound_ = true;
        else kd_mpi_vo_disable_video_layer(K_VO_LAYER1);
    } else {
        if (bound_) {
            ret = checked("VI0 -> VO1 unbind", kd_mpi_sys_unbind(&src, &dst));
            if (!ret) bound_ = false;
        }
        if (!ret) ret = checked("preview layer OFF", kd_mpi_vo_disable_video_layer(K_VO_LAYER1));
    }
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
    if (!bound_ && !capture_ && !display_) check("VO frame-end restore", sync_.stop());
    if (!bound_ && !capture_ && !display_ && !sync_.active() && buffers_ &&
        !check("VB exit", kd_mpi_vb_exit())) buffers_ = false;
    return error;
}
}
