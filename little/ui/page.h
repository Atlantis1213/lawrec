#pragma once
#include "lvgl.h"
#include "protocol.h"

namespace demo {
struct UiView {
    Status status;
    int transport_error = 0, operation_error = 0;
    uint32_t operation_command = 0;
    bool pending = false;
};
class Page {
public:
    using Action = void (*)(void *, uint32_t, uint32_t);
    void create(lv_obj_t *root, Action action, void *context);
    void update(const UiView &view);
    lv_obj_t *button(unsigned index) const { return index < 4 ? buttons_[index] : nullptr; }
private:
    static void clicked(lv_event_t *event);
    UiView view_;
    Action action_ = nullptr;
    void *context_ = nullptr;
    lv_obj_t *buttons_[4]{}, *states_[4]{};
    lv_obj_t *feedback_ = nullptr, *faces_ = nullptr, *video_ = nullptr;
    lv_obj_t *timing_ = nullptr, *system_ = nullptr, *viewport_ = nullptr;
};
}
