#include "lawrec_media_diagnostics.h"
#include <cassert>
#include <string>

int main() {
    using namespace lawrec_diagnostics_detail;
    uint64_t value = 123;
    // Fields 4..13 must be skipped, even negative values and a tricky comm.
    assert(cpu_ticks("42 (a ) tricky name) S 1 2 3 4 -5 6 7 8 9 10 111 222 999\n", value));
    assert(value == 333);
    assert(!cpu_ticks("42 (missing tail)", value));
    assert(!cpu_ticks("42 (name) S 1 2 3", value));
    assert(!cpu_ticks("42 (name) S 1 2 3 4 5 6 7 8 9 10 -1 2", value));
    assert(!cpu_ticks("42 (name) S 1 2 3 4 5 6 7 8 9 10 1x 2", value));
    assert(!cpu_ticks("42 (name) S 1 2 3 4 5 6 7 8 9 10 18446744073709551615 1", value));
    assert(!cpu_ticks("42 (name) S 1 2 3 4 5 6 7 8 9 10 18446744073709551616 0", value));
    assert(kib("VmRSS:\t  900 kB\n", "VmRSS:", value) && value == 900);
    assert(kib("MemAvailable: 0 kB\n", "MemAvailable:", value) && !value);
    assert(!kib("VmHWM: 32 kB\n", "VmRSS:", value));
    assert(!kib("VmRSS: -1 kB\n", "VmRSS:", value));
    assert(!kib("VmRSS: 10 MB\n", "VmRSS:", value));
    assert(!kib("VmRSS: 10 kB garbage\n", "VmRSS:", value));
    assert(!kib("VmRSS: 18446744073709551616 kB\n", "VmRSS:", value));

    const auto first = lawrec_process_stats();
    const unsigned required = LawrecProcessStats::CPU | LawrecProcessStats::RSS |
        LawrecProcessStats::HWM | LawrecProcessStats::FDS | LawrecProcessStats::AVAILABLE;
    assert((first.valid & required) == required && first.ticks_per_second > 0);
    FILE *extra = std::fopen("/dev/null", "r");
    assert(extra && lawrec_process_stats().fds == first.fds + 1);
    std::fclose(extra);
    for (unsigned i = 0; i < 20; ++i) {
        const auto next = lawrec_process_stats();
        assert(next.fds == first.fds && next.cpu_ticks >= first.cpu_ticks);
        assert(next.hwm_kib >= next.rss_kib && next.valid == first.valid);
    }
    lawrec_log_process_stats("test", "snapshot");
    puts("media diagnostics: process-name parsing, units, overflow, validity and fd cleanup passed");
}
