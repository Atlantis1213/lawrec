#include "lawrec_preview.h"

#include <cstdio>
#include <cstring>

#include "k_vo_comm.h"
#include "mpi_vicap_api.h"
#include "mpi_sys_api.h"
#include "mpi_vb_api.h"
#include "mpi_vo_api.h"

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
#define LAWREC_PREVIEW_WIDTH 800
#define LAWREC_PREVIEW_HEIGHT 480
#else
#define LAWREC_PREVIEW_WIDTH 1920
#define LAWREC_PREVIEW_HEIGHT 1080
#endif

LawrecPreviewController::LawrecPreviewController()
    : backend_ready_(false),
      enabled_(false),
      bound_(false),
      blank_pool_id_(VB_INVALID_POOLID),
      blank_block_(VB_INVALID_HANDLE),
      blank_vaddr_(NULL)
{
    pthread_mutex_init(&lock_, NULL);
    memset(&blank_vf_, 0, sizeof(blank_vf_));
}

LawrecPreviewController::~LawrecPreviewController()
{
    pthread_mutex_destroy(&lock_);
}

void LawrecPreviewController::FillBindChannels(k_mpp_chn *vicap_mpp_chn,
                                               k_mpp_chn *vo_mpp_chn) const
{
    vicap_mpp_chn->mod_id = K_ID_VI;
    vicap_mpp_chn->dev_id = VICAP_DEV_ID_0;
    vicap_mpp_chn->chn_id = VICAP_CHN_ID_0;

    vo_mpp_chn->mod_id = K_ID_VO;
    vo_mpp_chn->dev_id = K_VO_DISPLAY_DEV_ID;
    vo_mpp_chn->chn_id = K_VO_DISPLAY_CHN_ID1;
}

int LawrecPreviewController::InsertBlankFrameLocked(const char *tag)
{
    int ret;

    if (blank_vaddr_ == NULL) {
        printf("[lawrec-preview] %s blank unavailable; no idle frame inserted\n", tag);
        return 0;
    }

    ret = kd_mpi_vo_chn_insert_frame(K_VO_LAYER1, &blank_vf_);
    if (ret != 0)
        printf("[lawrec] %s blank insert failed ret=%d\n", tag, ret);
    return ret;
}

int LawrecPreviewController::InitBlankFrame()
{
    k_u64 phys_addr;
    int y_size;
    int uv_size;

    if (blank_vaddr_ != NULL)
        return 0;

    memset(&blank_vf_, 0, sizeof(blank_vf_));
    blank_vf_.mod_id = K_ID_VO;
    blank_vf_.v_frame.width = LAWREC_PREVIEW_WIDTH;
    blank_vf_.v_frame.height = LAWREC_PREVIEW_HEIGHT;
    blank_vf_.v_frame.stride[0] = LAWREC_PREVIEW_WIDTH;
    blank_vf_.v_frame.pixel_format = PIXEL_FORMAT_YVU_PLANAR_420;

    y_size = LAWREC_PREVIEW_WIDTH * LAWREC_PREVIEW_HEIGHT;
    uv_size = y_size / 2;
    /* A persistent idle frame must not compete with VICAP/VENC common pools. */
    k_vb_pool_config pool = {};
    pool.blk_cnt = 1;
    pool.blk_size = (y_size + uv_size + 4095) & ~4095;
    pool.mode = VB_REMAP_MODE_NOCACHE;
    blank_pool_id_ = kd_mpi_vb_create_pool(&pool);
    if (blank_pool_id_ == VB_INVALID_POOLID) {
        printf("[lawrec-preview] blank pool create failed bytes=%llu\n",
               (unsigned long long)pool.blk_size);
        return -1;
    }
    blank_block_ = kd_mpi_vb_get_block(blank_pool_id_, y_size + uv_size, NULL);
    if (blank_block_ == VB_INVALID_HANDLE) {
        printf("[lawrec-preview] blank get block failed pool=%u bytes=%d\n",
               blank_pool_id_, y_size + uv_size);
        kd_mpi_vb_destory_pool(blank_pool_id_);
        blank_pool_id_ = VB_INVALID_POOLID;
        return -1;
    }

    phys_addr = kd_mpi_vb_handle_to_phyaddr(blank_block_);
    if (phys_addr == 0) {
        printf("[lawrec] preview blank phys addr failed\n");
        kd_mpi_vb_release_block(blank_block_);
        blank_block_ = VB_INVALID_HANDLE;
        kd_mpi_vb_destory_pool(blank_pool_id_);
        blank_pool_id_ = VB_INVALID_POOLID;
        return -1;
    }

    blank_vaddr_ = kd_mpi_sys_mmap(phys_addr, y_size + uv_size);
    if (blank_vaddr_ == NULL) {
        printf("[lawrec] preview blank mmap failed\n");
        kd_mpi_vb_release_block(blank_block_);
        blank_block_ = VB_INVALID_HANDLE;
        kd_mpi_vb_destory_pool(blank_pool_id_);
        blank_pool_id_ = VB_INVALID_POOLID;
        return -1;
    }

    memset(blank_vaddr_, 0x00, y_size);
    memset((k_u8 *)blank_vaddr_ + y_size, 0x80, uv_size);

    blank_vf_.pool_id = kd_mpi_vb_handle_to_pool_id(blank_block_);
    blank_vf_.v_frame.phys_addr[0] = phys_addr;
    blank_vf_.v_frame.phys_addr[1] = phys_addr + y_size;
    printf("[lawrec] preview blank ready pool=%u phys=0x%lx\n",
           blank_vf_.pool_id, phys_addr);
    return 0;
}

void LawrecPreviewController::DeinitBlankFrame()
{
    int size;

    if (blank_vaddr_ == NULL || blank_block_ == VB_INVALID_HANDLE)
        return;

    size = LAWREC_PREVIEW_WIDTH * LAWREC_PREVIEW_HEIGHT * 3 / 2;
    kd_mpi_sys_munmap(blank_vaddr_, size);
    kd_mpi_vb_release_block(blank_block_);
    blank_vaddr_ = NULL;
    blank_block_ = VB_INVALID_HANDLE;
    int ret = kd_mpi_vb_destory_pool(blank_pool_id_);
    if (ret != 0)
        printf("[lawrec-preview] blank pool destroy failed pool=%u ret=%d\n", blank_pool_id_, ret);
    blank_pool_id_ = VB_INVALID_POOLID;
}

int LawrecPreviewController::Enter(lawrec_preview_bind_fn_t bind_fn)
{
    int ret = 0;

    if (!backend_ready_.load()) {
        printf("[lawrec] preview enter rejected: backend not ready\n");
        return -1;
    }

    pthread_mutex_lock(&lock_);
    if (!bound_.load()) {
        ret = bind_fn != NULL ? bind_fn() : -1;
        if (ret == 0)
            bound_.store(true);
    }
    if (ret == 0)
        enabled_.store(true);
    pthread_mutex_unlock(&lock_);

    printf("[lawrec] preview enter ret=%d enabled=%d bound=%d\n",
           ret, (int)enabled_.load(), (int)bound_.load());
    return ret;
}

int LawrecPreviewController::Exit(lawrec_preview_unbind_fn_t unbind_fn)
{
    k_mpp_chn vicap_mpp_chn;
    k_mpp_chn vo_mpp_chn;

    pthread_mutex_lock(&lock_);
    enabled_.store(false);
    if (!bound_.load()) {
        pthread_mutex_unlock(&lock_);
        printf("[lawrec] preview exit ignored: already unbound\n");
        return 0;
    }

    FillBindChannels(&vicap_mpp_chn, &vo_mpp_chn);
    int ret = unbind_fn != NULL ? unbind_fn(vicap_mpp_chn, vo_mpp_chn) : -1;
    if (ret != 0) {
        /* Keep ownership truthful so the next stop can retry the failed unbind. */
        pthread_mutex_unlock(&lock_);
        printf("[lawrec] preview exit unbind failed ret=%d bound=1\n", ret);
        return ret;
    }
    bound_.store(false);
    ret = InsertBlankFrameLocked("preview exit");
    pthread_mutex_unlock(&lock_);

    printf("[lawrec] preview exit done enabled=%d bound=%d\n",
           (int)enabled_.load(), (int)bound_.load());
    return ret;
}

void LawrecPreviewController::ProbeFrame()
{
    /* One bounded probe per observed entry, on the main loop, never in IPC.
     * Binding succeeds even when no camera frames are being produced. */
    k_video_frame_info frame;
    memset(&frame, 0, sizeof(frame));
    k_vicap_chn channel = VICAP_CHN_ID_0;
    int ret = kd_mpi_vicap_dump_frame(VICAP_DEV_ID_0, channel,
                                    VICAP_DUMP_YUV, &frame, 500);
    if (ret != 0) {
        printf("[lawrec-preview] frame probe chn=0 ret=%d hex=0x%08x; checking encode feed\n",
               ret, (unsigned)ret);
        // A bound display may consume CHN0. Check CHN1 separately so a display
        // dump timeout is not mistaken for proof that the entire sensor is idle.
        channel = VICAP_CHN_ID_1;
        ret = kd_mpi_vicap_dump_frame(VICAP_DEV_ID_0, channel, VICAP_DUMP_YUV, &frame, 500);
        if (ret != 0) {
            printf("[lawrec-preview] frame probe failed chn=1 ret=%d hex=0x%08x; inspect capture startup/sensor\n",
                   ret, (unsigned)ret);
            return;
        }
    }
    printf("[lawrec-preview] frame chn=%d size=%ux%u format=%d stride=%u pts=%llu phys=0x%llx\n",
           channel, frame.v_frame.width, frame.v_frame.height, (int)frame.v_frame.pixel_format,
           frame.v_frame.stride[0], (unsigned long long)frame.v_frame.pts,
           (unsigned long long)frame.v_frame.phys_addr[0]);
    ret = kd_mpi_vicap_dump_release(VICAP_DEV_ID_0, channel, &frame);
    if (ret != 0)
        printf("[lawrec-preview] frame release failed chn=%d ret=%d\n", channel, ret);
}

void LawrecPreviewController::ForceUnbindAtStartup(lawrec_preview_unbind_fn_t unbind_fn)
{
    k_mpp_chn vicap_mpp_chn;
    k_mpp_chn vo_mpp_chn;

    pthread_mutex_lock(&lock_);
    FillBindChannels(&vicap_mpp_chn, &vo_mpp_chn);
    if (unbind_fn != NULL)
        unbind_fn(vicap_mpp_chn, vo_mpp_chn);
    bound_.store(false);
    enabled_.store(false);
    InsertBlankFrameLocked("startup");
    pthread_mutex_unlock(&lock_);

    printf("[lawrec] preview forced to idle at startup\n");
}

void LawrecPreviewController::SetBackendReady(bool ready)
{
    backend_ready_.store(ready);
}

bool LawrecPreviewController::BackendReady() const
{
    return backend_ready_.load();
}

bool LawrecPreviewController::Enabled() const
{
    return enabled_.load();
}

bool LawrecPreviewController::Bound() const
{
    return bound_.load();
}

void LawrecPreviewController::Snapshot(bool *ready, bool *enabled, bool *bound)
{
    pthread_mutex_lock(&lock_);
    *ready = backend_ready_.load();
    *enabled = enabled_.load();
    *bound = bound_.load();
    pthread_mutex_unlock(&lock_);
}
