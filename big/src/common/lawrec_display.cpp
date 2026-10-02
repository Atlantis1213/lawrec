#include "lawrec_display.h"
#include "mpi_vo_api.h"
#include <cerrno>
#include <cstdio>

int LawrecDisplayController::Recover()
{
    busy_.store(true);
    int disable = kd_mpi_vo_disable_video_layer(K_VO_LAYER0);
    int restore = kd_mpi_vo_enable_video_layer(K_VO_LAYER1);
    const int result = disable ? disable : restore;
    if (!result) busy_.store(false);
    printf("[lawrec-display] recover layer0_disable=%d layer1_restore=%d result=%d occupied=%d\n",
           disable, restore, result, (int)Busy());
    return result;
}

int LawrecDisplayController::Enable(uint32_t width, uint32_t height)
{
    if (width != 1280 || height != 720) return -EINVAL;
    if (Busy()) return -EBUSY;
    k_vo_video_layer_attr attr{};
    attr.img_size.width = width; attr.img_size.height = height;
    attr.pixel_format = PIXEL_FORMAT_YVU_PLANAR_420;
    attr.stride = (width / 8 - 1) | ((height - 1) << 16);
    attr.func = K_VO_SCALER_ENABLE;
    attr.scaler_attr.out_size.width = 480; attr.scaler_attr.out_size.height = 270;
    attr.scaler_attr.stride = (480 / 8 - 1) | ((270 - 1) << 16);
    attr.display_rect.y = 200;
    busy_.store(true);
    const char *stage = "layer0 attribute";
    int ret = kd_mpi_vo_set_video_layer_attr(K_VO_LAYER0, &attr);
    if (!ret) { stage = "layer1 disable"; ret = kd_mpi_vo_disable_video_layer(K_VO_LAYER1); }
    if (!ret) { stage = "layer0 enable"; ret = kd_mpi_vo_enable_video_layer(K_VO_LAYER0); }
    if (ret) {
        const int rollback = Recover();
        printf("[lawrec-display] playback enable stage=%s result=%d rollback=%d occupied=%d\n",
               stage, ret, rollback, (int)Busy());
    } else printf("[lawrec-display] playback layer enabled\n");
    return ret;
}
