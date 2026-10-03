#include "metrics.h"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <unistd.h>

namespace demo {
bool parse_process_stat(const std::string &line, ProcessSample &sample) {
    // comm may contain spaces and ')' characters; fields start after its last ')'.
    auto end = line.rfind(')');
    if (end == std::string::npos || line.find('(') == std::string::npos) return false;
    std::istringstream fields(line.substr(end + 1));
    uint64_t user = 0, system = 0, pages = 0;
    for (unsigned field = 3; field <= 24; ++field) {
        std::string token;
        if (!(fields >> token)) return false;
        if (field != 14 && field != 15 && field != 24) continue;
        uint64_t value = 0;
        auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return false;
        if (field == 14) user = value;
        if (field == 15) system = value;
        if (field == 24) pages = value;
    }
    if (system > std::numeric_limits<uint64_t>::max() - user) return false;
    sample = {user + system, pages};
    return true;
}
void MediaMetrics::update(const SourceStats &source) {
    auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (last_us_ && now - last_us_ < 1000000) return;
    last_us_ = now;
    fps_ = frames_.sample(now, source.video_frames, source.generation, 1000000000);
    kbps_ = bytes_.sample(now, source.video_bytes, source.generation, 8000);
    ProcessSample process;
    std::ifstream stat("/proc/self/stat");
    std::string line;
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    if (hz <= 0 || page <= 0 || !std::getline(stat, line) || !parse_process_stat(line, process)) {
        cpu_ = {}; cpu_percent_ = rss_ = metric_unavailable;
    } else {
        bool warm = cpu_.initialized;
        cpu_percent_ = cpu_.sample(now, process.ticks, 0, 100000000000ULL / uint64_t(hz));
        if (!warm) cpu_percent_ = metric_unavailable;
        rss_ = bounded_rate(process.resident_pages, uint64_t(page), 1024);
    }
    std::fprintf(stderr, "[media-metrics] video=%.2f fps %u kbps cpu_milli=%u rss=%u KiB (media process only; UINT32_MAX=unavailable)\n",
        fps_ / 1000.0, kbps_, cpu_percent_, rss_);
}
void MediaMetrics::apply(Status &status) const {
    status.video_fps_milli = fps_; status.bitrate_kbps = kbps_;
    status.cpu_percent_milli = cpu_percent_; status.rss_kib = rss_;
}
}
