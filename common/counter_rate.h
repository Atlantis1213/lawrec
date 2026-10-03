#pragma once
#include <cstdint>
#include <limits>

namespace demo {
inline uint32_t bounded_rate(uint64_t count, uint64_t scale, uint64_t elapsed) {
    if (!elapsed) return 0;
    long double result = static_cast<long double>(count) * scale / elapsed;
    constexpr auto limit = std::numeric_limits<uint32_t>::max() - 1;
    return result >= limit ? limit : static_cast<uint32_t>(result);
}
struct CounterRate {
    uint64_t time = 0, count = 0, epoch = 0;
    bool initialized = false;
    uint32_t sample(uint64_t now, uint64_t next, uint64_t generation, uint64_t scale) {
        uint32_t result = 0;
        if (initialized && now > time && next >= count && epoch == generation)
            result = bounded_rate(next - count, scale, now - time);
        time = now; count = next; epoch = generation; initialized = true;
        return result;
    }
};
}
