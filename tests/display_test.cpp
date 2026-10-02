#include "../big/src/common/lawrec_display.h"
#include "mpi_vo_api.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <vector>

static int attr_error, preview_disable_error, playback_enable_error, playback_disable_error, preview_restore_error;
static std::vector<int> calls;
extern "C" k_s32 kd_mpi_vo_set_video_layer_attr(k_vo_layer layer, k_vo_video_layer_attr *attr)
{
    assert(layer==K_VO_LAYER0 && attr->img_size.width==1280 && attr->img_size.height==720);
    assert(attr->scaler_attr.out_size.width==480 && attr->scaler_attr.out_size.height==270 && attr->display_rect.y==200);
    calls.push_back(1); return attr_error;
}
extern "C" k_s32 kd_mpi_vo_disable_video_layer(k_vo_layer layer)
{
    calls.push_back(layer==K_VO_LAYER1 ? 2 : 4);
    return layer==K_VO_LAYER1 ? preview_disable_error : playback_disable_error;
}
extern "C" k_s32 kd_mpi_vo_enable_video_layer(k_vo_layer layer)
{
    calls.push_back(layer==K_VO_LAYER0 ? 3 : 5);
    return layer==K_VO_LAYER0 ? playback_enable_error : preview_restore_error;
}
static void clear()
{
    calls.clear(); attr_error=preview_disable_error=playback_enable_error=playback_disable_error=preview_restore_error=0;
}
int main()
{
    LawrecDisplayController display;
    assert(display.Enable(480,800)==-EINVAL && calls.empty() && !display.Busy());
    assert(display.Enable(1280,720)==0 && display.Busy() && calls==std::vector<int>({1,2,3}));
    calls.clear(); assert(display.Enable(1280,720)==-EBUSY && calls.empty());
    playback_disable_error=-EIO;
    assert(display.Recover()==-EIO && display.Busy() && calls==std::vector<int>({4,5}));
    clear(); preview_restore_error=-ENOSPC;
    assert(display.Recover()==-ENOSPC && display.Busy());
    clear(); assert(display.Recover()==0 && !display.Busy());

    for (int step=1;step<=3;++step) {
        clear();
        if (step==1) attr_error=-EINVAL;
        if (step==2) preview_disable_error=-EINVAL;
        if (step==3) playback_enable_error=-EINVAL;
        assert(display.Enable(1280,720)==-EINVAL && !display.Busy());
        assert(calls.size()==(unsigned)(step+2) && calls[calls.size()-2]==4 && calls.back()==5);
        clear();
        playback_enable_error=-EIO; preview_restore_error=-ENOSPC;
        assert(display.Enable(1280,720)==-EIO && display.Busy());
        assert(display.Enable(1280,720)==-EBUSY);
        clear(); assert(display.Recover()==0 && !display.Busy());
    }
    clear(); playback_disable_error=-EIO; preview_restore_error=-ENOMEM;
    assert(display.Recover()==-EIO && display.Busy());
    puts("Display: original geometry, bounded switch rollback, first-error retention and recovery ownership passed (mock VO).");
}
