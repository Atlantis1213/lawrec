#pragma once
#include <atomic>
#include <sys/types.h>
#include <cstdint>

struct lawrec_dhcp_stamp {
    uint64_t received_ms = 0;
    char boot_id[37] = {};
};

struct lawrec_dhcp_status {
    pid_t pid = 0;
    int bound = 0;
    int error = 0;
    unsigned lease_seconds = 0;
    unsigned remaining_seconds = 0;
    char address[16] = {};
};
/* BusyBox acquires once in foreground, then backgrounds and renews indefinitely. */
int lawrec_dhcp_acquire(const std::atomic<bool> &cancel);
/* Stops only a pidfd-pinned client whose exact wlan0/pid/hook arguments match. */
int lawrec_dhcp_stop(void);
int lawrec_dhcp_get(lawrec_dhcp_status *status);
/* Hook must belong to the current job; retired/foreign jobs cannot update it. */
int lawrec_dhcp_hook_verify(const char *job);
/* CLOCK_BOOTTIME includes suspend, unlike realtime/elapsed UI uptime. Capture
 * at hook entry so slow address/DNS setup cannot extend an offered lease. */
int lawrec_dhcp_stamp_now(lawrec_dhcp_stamp *stamp);
int lawrec_dhcp_hook_result(const char *job, int error, const char *address, unsigned lease,
                            const lawrec_dhcp_stamp *received = nullptr);
