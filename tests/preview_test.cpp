/* Exercise real controller ownership/error paths without VO or camera hardware. */
#include <cassert>
#include <cstdio>
#include "lawrec_preview.h"
#include "mpi_vb_api.h"
#include "mpi_sys_api.h"
#include "mpi_vo_api.h"
#include "mpi_vicap_api.h"

static unsigned char blank[800 * 480 * 3 / 2];
static int pools, blocks, inserts, releases, binds;
static int unbind_result, insert_result, dump_result;
static bool block_failure;

extern "C" {
k_s32 kd_mpi_vb_create_pool(k_vb_pool_config *config)
{
    assert(config->blk_cnt == 1 && config->blk_size >= sizeof(blank));
    ++pools;
    return 7;
}
k_s32 kd_mpi_vb_destory_pool(k_u32 pool) { assert(pool == 7 && blocks == 0); --pools; return 0; }
k_vb_blk_handle kd_mpi_vb_get_block(k_u32 pool, k_u64, const k_char *)
{
    assert(pool == 7);
    if (block_failure) return VB_INVALID_HANDLE;
    ++blocks;
    return 1;
}
k_s32 kd_mpi_vb_release_block(k_vb_blk_handle) { --blocks; return 0; }
k_u64 kd_mpi_vb_handle_to_phyaddr(k_vb_blk_handle) { return 0x2000; }
k_s32 kd_mpi_vb_handle_to_pool_id(k_vb_blk_handle) { return 7; }
void *kd_mpi_sys_mmap(k_u64, k_u32 size) { assert(size == sizeof(blank)); return blank; }
k_s32 kd_mpi_sys_munmap(void *, k_u32) { return 0; }
k_s32 kd_mpi_vo_chn_insert_frame(k_u32, k_video_frame_info *) { ++inserts; return insert_result; }
k_s32 kd_mpi_vicap_dump_frame(k_vicap_dev, k_vicap_chn, k_vicap_dump_format,
                             k_video_frame_info *, k_u32 timeout)
{
    assert(timeout == 500);
    return dump_result;
}
k_s32 kd_mpi_vicap_dump_release(k_vicap_dev, k_vicap_chn, const k_video_frame_info *)
{ ++releases; return 0; }
}
static int bind() { ++binds; return 0; }
static int unbind(k_mpp_chn, k_mpp_chn) { return unbind_result; }

int main()
{
    LawrecPreviewController preview;
    bool ready, enabled, bound;
    preview.Snapshot(&ready, &enabled, &bound);
    assert(!ready && !enabled && !bound);
    assert(preview.Enter(bind) != 0 && binds == 0);
    preview.SetBackendReady(true);
    assert(preview.Enter(NULL) != 0 && !preview.Enabled() && !preview.Bound());
    block_failure = true;
    assert(preview.InitBlankFrame() != 0 && pools == 0 && blocks == 0);
    block_failure = false;
    assert(preview.InitBlankFrame() == 0 && pools == 1 && blocks == 1);
    assert(preview.InitBlankFrame() == 0 && pools == 1 && blocks == 1);
    assert(blank[0] == 0 && blank[800 * 480] == 0x80);
    assert(preview.Enter(bind) == 0 && preview.Enabled() && preview.Bound());
    preview.Snapshot(&ready, &enabled, &bound);
    assert(ready && enabled && bound);
    assert(preview.Enter(bind) == 0 && binds == 1);
    unbind_result = -5;
    assert(preview.Exit(unbind) == -5 && preview.Bound() && !preview.Enabled());
    preview.Snapshot(&ready, &enabled, &bound);
    assert(ready && !enabled && bound);
    assert(inserts == 0);
    unbind_result = 0;
    insert_result = -6;
    assert(preview.Exit(unbind) == -6 && !preview.Bound() && inserts == 1);
    dump_result = -7;
    preview.ProbeFrame();
    assert(releases == 0);
    dump_result = 0;
    preview.ProbeFrame();
    assert(releases == 1);
    preview.DeinitBlankFrame();
    preview.DeinitBlankFrame();
    assert(pools == 0 && blocks == 0);
    puts("Preview: bind/unbind errors, private blank pool and probe release passed");
}
