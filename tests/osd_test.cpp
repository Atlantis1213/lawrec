// Tests resource ordering and rendered pixels, not scanout or alpha blending.
#include "osd.h"
#include "mpi_vb_api.h"
#include "mpi_sys_api.h"
#include "mpi_vo_api.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <vector>

namespace {
std::vector<uint32_t> pixels[2];
unsigned next_block, releases, unmaps, destroys, inserts;
bool submitted;
int enable_error, disable_error;
}
extern "C" {
k_s32 kd_mpi_vb_create_pool(k_vb_pool_config *config) {
    assert(config->blk_cnt == 2 && config->blk_size == 480 * 800 * 4);
    next_block = 0; return 3;
}
k_vb_blk_handle kd_mpi_vb_get_block(k_u32 pool, k_u64 size, const k_char *) {
    assert(pool == 3 && size == 480 * 800 * 4 && next_block < 2);
    return ++next_block;
}
k_u64 kd_mpi_vb_handle_to_phyaddr(k_vb_blk_handle block) { return block * 4096; }
void *kd_mpi_sys_mmap(k_u64 address, k_u32 size) {
    auto &buffer = pixels[address / 4096 - 1];
    buffer.resize(size / 4); return buffer.data();
}
k_s32 kd_mpi_sys_munmap(void *, k_u32) { assert(!submitted); ++unmaps; return 0; }
k_s32 kd_mpi_vb_release_block(k_vb_blk_handle) { assert(!submitted); ++releases; return 0; }
k_s32 kd_mpi_vb_destory_pool(k_u32 pool) { assert(pool == 3 && !submitted); ++destroys; return 0; }
k_s32 kd_mpi_vo_set_video_osd_attr(k_vo_osd layer, k_vo_video_osd_attr *attr) {
    assert(layer == K_VO_OSD2 && attr->pixel_format == PIXEL_FORMAT_ARGB_8888);
    assert(attr->img_size.width == 480 && attr->img_size.height == 800 && attr->stride == 240);
    return 0;
}
k_s32 kd_mpi_vo_chn_insert_frame(k_u32 channel, k_video_frame_info *frame) {
    assert(channel == 5 && frame->v_frame.width == 480 && frame->v_frame.height == 800);
    submitted = true; ++inserts; return 0;
}
k_s32 kd_mpi_vo_osd_enable(k_vo_osd layer) { assert(layer == K_VO_OSD2); return enable_error; }
k_s32 kd_mpi_vo_osd_disable(k_vo_osd layer) {
    assert(layer == K_VO_OSD2);
    if (!disable_error) submitted = false;
    return disable_error;
}
}
int main() {
    demo::Osd osd;
    assert(osd.start() == 0 && osd.budget_kib() == 3000);
    assert(osd.show({}) == 0 && submitted);
    assert(pixels[0][0] == 0 && pixels[0][160 * 480 + 27] == 0xff32df8b);
    assert(osd.show({}) == 0 && inserts == 2);
    disable_error = -EIO;
    assert(osd.stop() == -EIO && submitted && !unmaps && !releases && !destroys);
    disable_error = 0;
    assert(osd.stop() == 0 && !submitted && unmaps == 2 && releases == 2 && destroys == 1);
    assert(osd.budget_kib() == 0);
    assert(osd.start() == 0);
    enable_error = -EIO;
    assert(osd.show({}) == -EIO && submitted);
    disable_error = -EIO;
    assert(osd.stop() == -EIO && unmaps == 2 && releases == 2);
    disable_error = 0;
    assert(osd.stop() == 0 && !submitted && releases == 4);
    std::puts("OSD MOCK ONLY: separate layer, transparent pixels and failed-enable buffer retention passed");
}
