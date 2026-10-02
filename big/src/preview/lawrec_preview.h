#pragma once

#include <atomic>
#include <pthread.h>

#include "k_sys_comm.h"
#include "k_vb_comm.h"
#include "k_video_comm.h"

typedef int (*lawrec_preview_bind_fn_t)(void);
typedef int (*lawrec_preview_unbind_fn_t)(k_mpp_chn, k_mpp_chn);

class LawrecPreviewController {
  public:
    LawrecPreviewController();
    ~LawrecPreviewController();

    int InitBlankFrame();
    void DeinitBlankFrame();

    int Enter(lawrec_preview_bind_fn_t bind_fn);
    int Exit(lawrec_preview_unbind_fn_t unbind_fn);
    void ForceUnbindAtStartup(lawrec_preview_unbind_fn_t unbind_fn);

    void SetBackendReady(bool ready);
    bool BackendReady() const;
    bool Enabled() const;
    bool Bound() const;
    void Snapshot(bool *ready, bool *enabled, bool *bound);
    void ProbeFrame();

  private:
    void FillBindChannels(k_mpp_chn *vicap_mpp_chn, k_mpp_chn *vo_mpp_chn) const;
    int InsertBlankFrameLocked(const char *tag);

  private:
    std::atomic<bool> backend_ready_;
    std::atomic<bool> enabled_;
    std::atomic<bool> bound_;
    pthread_mutex_t lock_;
    k_u32 blank_pool_id_;
    k_vb_blk_handle blank_block_;
    k_video_frame_info blank_vf_;
    void *blank_vaddr_;
};
