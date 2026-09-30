#include "lawrec_media_clock.h"
#include <cassert>
#include <cstdio>
int main() {
    LawrecMediaClock clock;
    uint64_t value;
    assert(clock.map(1000000, 100000000, value) && value == 100000000);
    // Different arrival latency and wall-clock adjustments do not shift PTS.
    assert(clock.map(1040000, 200000000, value) && value == 100040000);
    assert(clock.map(980000, 300000000, value) && value == 99980000);
    assert(clock.map(1040000, 0, value) && value == 100040000);
    clock.reset();
    assert(clock.map(0, 10, value) && value == 10);
    assert(!clock.map(UINT64_MAX, 10, value));
    clock.reset();
    assert(clock.map(100, 10, value));
    assert(!clock.map(0, 10, value));
    puts("media clock: shared epoch, delayed audio, wall jumps, reset and bounds passed");
}
