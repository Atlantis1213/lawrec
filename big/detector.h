#pragma once
#include "faces.h"
#include <cstdint>
#include <memory>

namespace demo {
struct AiTiming { uint32_t ai2d_us = 0, kpu_us = 0, post_us = 0, total_us = 0; };
class Detector {
public:
    Detector();
    ~Detector();
    int load(const char *model);
    int process(std::vector<Face> &faces, AiTiming &timing);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
