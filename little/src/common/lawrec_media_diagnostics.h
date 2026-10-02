#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include "lawrec_frame_queue.h"

struct LawrecProcessStats {
    enum : unsigned { CPU = 1, RSS = 2, HWM = 4, FDS = 8, AVAILABLE = 16 };
    unsigned valid = 0;
    uint64_t cpu_ticks = 0, rss_kib = 0, hwm_kib = 0, available_kib = 0;
    long ticks_per_second = 0;
    unsigned fds = 0;
};

namespace lawrec_diagnostics_detail {
inline bool cpu_ticks(const char *stat, uint64_t &ticks) {
    // Field 2 is parenthesized and can contain spaces and ')'. Fields 14/15
    // are process user/system CPU ticks, NOT elapsed time or child CPU time.
    const char *p = std::strrchr(stat, ')');
    if (!p || p[1] != ' ' || !p[2] || p[3] != ' ') return false;
    p += 4;
    uint64_t sum = 0;
    for (unsigned field = 4; field <= 15; ++field) {
        while (*p == ' ') ++p;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\n') ++end;
        if (p == end) return false;
        if (field >= 14) {
            if (*p < '0' || *p > '9') return false;
            errno = 0;
            char *parsed = nullptr;
            const auto value = std::strtoull(p, &parsed, 10);
            if (errno || parsed != end || value > UINT64_MAX - sum) return false;
            sum += value;
        }
        p = end;
    }
    ticks = sum;
    return true;
}

inline bool kib(const char *line, const char *key, uint64_t &value) {
    const size_t length = std::strlen(key);
    if (std::strncmp(line, key, length)) return false;
    const char *p = line + length;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end = nullptr;
    const auto parsed = std::strtoull(p, &end, 10);
    if (errno) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (std::strcmp(end, "kB\n") && std::strcmp(end, "kB")) return false;
    value = parsed;
    return true;
}
}

/* Read-only, low-frequency diagnostics. Missing /proc fields are explicit via
 * valid bits; collection never changes media state or calls an SDK API. RSS
 * is Linux process memory, not big-core VB/VENC or total system allocation. */
inline LawrecProcessStats lawrec_process_stats() {
    LawrecProcessStats result;
    char line[2048];
    FILE *fp = std::fopen("/proc/self/stat", "r");
    if (fp) {
        size_t size = std::fread(line, 1, sizeof(line)-1, fp);
        line[size] = '\0';
        result.ticks_per_second = sysconf(_SC_CLK_TCK);
        if (!std::ferror(fp) && size < sizeof(line)-1 && result.ticks_per_second > 0 &&
            lawrec_diagnostics_detail::cpu_ticks(line, result.cpu_ticks))
            result.valid |= LawrecProcessStats::CPU;
        std::fclose(fp);
    }
    fp = std::fopen("/proc/self/status", "r");
    if (fp) {
        while (std::fgets(line, sizeof(line), fp)) {
            if (lawrec_diagnostics_detail::kib(line, "VmRSS:", result.rss_kib)) result.valid |= LawrecProcessStats::RSS;
            if (lawrec_diagnostics_detail::kib(line, "VmHWM:", result.hwm_kib)) result.valid |= LawrecProcessStats::HWM;
        }
        if (std::ferror(fp)) result.valid &= ~(LawrecProcessStats::RSS | LawrecProcessStats::HWM);
        std::fclose(fp);
    }
    fp = std::fopen("/proc/meminfo", "r");
    if (fp) {
        while (std::fgets(line, sizeof(line), fp))
            if (lawrec_diagnostics_detail::kib(line, "MemAvailable:", result.available_kib))
                result.valid |= LawrecProcessStats::AVAILABLE;
        if (std::ferror(fp)) result.valid &= ~LawrecProcessStats::AVAILABLE;
        std::fclose(fp);
    }
    DIR *directory = opendir("/proc/self/fd");
    if (directory) {
        const int own_fd = dirfd(directory);
        while (true) {
            errno = 0;
            const auto *entry = readdir(directory);
            if (!entry) {
                if (!errno) result.valid |= LawrecProcessStats::FDS;
                break;
            }
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            char *end = nullptr;
            const long fd = std::strtol(entry->d_name, &end, 10);
            if (!*end && fd != own_fd) ++result.fds;
        }
        closedir(directory);
    }
    return result;
}

inline void lawrec_log_process_stats(const char *owner, const char *phase) {
    const auto s = lawrec_process_stats();
    std::fprintf(stderr, "[lawrec-%s] stats phase=%s proc_valid=0x%x cpu_ticks=%llu clk_tck=%ld rss_kib=%llu hwm_kib=%llu fds=%u mem_available_kib=%llu\n",
        owner, phase, s.valid, (unsigned long long)s.cpu_ticks, s.ticks_per_second,
        (unsigned long long)s.rss_kib, (unsigned long long)s.hwm_kib,
        s.fds, (unsigned long long)s.available_kib);
}

inline void lawrec_log_queue_stats(const char *owner, const char *phase,
                                   const char *track, const LawrecFrameQueue &queue) {
    const auto s = queue.stats();
    std::fprintf(stderr, "[lawrec-%s] queue phase=%s track=%s depth=%zu bytes=%zu peak_depth=%zu peak_bytes=%zu accepted=%llu popped=%llu discarded=%llu rejected=%llu oldest_us=%llu max_residence_us=%llu closed=%d error=%d\n",
        owner, phase, track, s.depth, s.bytes, s.peak_depth, s.peak_bytes,
        (unsigned long long)s.accepted, (unsigned long long)s.popped,
        (unsigned long long)s.discarded, (unsigned long long)s.rejected,
        (unsigned long long)s.oldest_age_us, (unsigned long long)s.max_residence_us,
        s.closed, s.error);
}
