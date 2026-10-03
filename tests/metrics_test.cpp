#include "metrics.h"
#include <cassert>
#include <cstdio>
#include <sstream>

int main() {
    demo::CounterRate frames, bytes, cpu;
    assert(frames.sample(1000000, 10, 1, 1000000000) == 0);
    assert(frames.sample(2000000, 40, 1, 1000000000) == 30000);
    assert(frames.sample(3000000, 40, 1, 1000000000) == 0);
    assert(frames.sample(4000000, 100, 2, 1000000000) == 0);
    assert(frames.sample(5000000, 130, 2, 1000000000) == 30000);
    bytes.sample(0, 0, 1, 8000);
    assert(bytes.sample(1000000, 500000, 1, 8000) == 4000);
    cpu.sample(1000000, 5, 0, 1000000000);
    assert(cpu.sample(2000000, 30, 0, 1000000000) == 25000);
    assert(demo::bounded_rate(UINT64_MAX, UINT64_MAX, 1) == UINT32_MAX - 1);
    std::ostringstream line;
    line << "123 (worker ) name) S";
    for (unsigned field = 4; field <= 24; ++field)
        line << ' ' << (field == 14 ? 123 : field == 15 ? 456 : field == 24 ? 789 : -1);
    demo::ProcessSample process;
    assert(demo::parse_process_stat(line.str(), process));
    assert(process.ticks == 579 && process.resident_pages == 789);
    assert(!demo::parse_process_stat("123 (short) S", process));
    assert(!demo::parse_process_stat(line.str().substr(0, line.str().rfind(' ')) + " -1", process));
    demo::MediaMetrics metrics;
    metrics.update({});
    demo::Status status;
    metrics.apply(status);
    assert(status.rss_kib != demo::metric_unavailable);
    assert(status.cpu_percent_milli == demo::metric_unavailable); // No first delta yet.
    std::puts("metrics: actual proc RSS, parser, rate, restart/idle and saturation passed");
}
