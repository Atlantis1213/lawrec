#pragma once
#include <cstdint>
#include "vo_sync.h"

namespace demo {
// This is the sole owner of VB, sensor capture and the preview video layer.
class Camera {
public:
    int start();
    int set_preview(bool enabled);
    int stop();
    void log_buffers() const;
    bool preview_enabled() const { return preview_; }
    uint32_t vb_budget_kib() const { return vb_kib_; }
private:
    int setup_buffers();
    int setup_display();
    int setup_capture();
    bool buffers_ = false, capture_ = false, streaming_ = false;
    bool bound_ = false, display_ = false, preview_ = false;
    uint32_t vb_kib_ = 0;
    VoSync sync_;
};
}
