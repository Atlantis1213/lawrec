#pragma once
#include "protocol.h"
#include "counter_rate.h"
#include "source.h"
#include <string>

namespace demo {
struct ProcessSample { uint64_t ticks = 0, resident_pages = 0; };
bool parse_process_stat(const std::string &line, ProcessSample &sample);
class MediaMetrics {
public:
    void update(const SourceStats &source);
    void apply(Status &status) const;
private:
    CounterRate frames_, bytes_, cpu_;
    uint64_t last_us_ = 0;
    uint32_t fps_ = 0, kbps_ = 0;
    uint32_t cpu_percent_ = metric_unavailable, rss_ = metric_unavailable;
};
}
