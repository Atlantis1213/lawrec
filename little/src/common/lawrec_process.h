#pragma once
#include <atomic>
/* Run an argv vector without a shell. Own a dedicated process group so a timed
 * out DHCP client cannot leave its lease hook behind. Return negative errno,
 * zero on success, -ECHILD for a nonzero child exit. Never log argv/secrets. */
/* own_group=false is only for leaf /sbin/ip commands inside a DHCP hook: they
 * stay in the client's group so cancellation cannot orphan config commands. */
int lawrec_process_run(const char *path, char *const argv[], unsigned timeout_ms,
                       const std::atomic<bool> &cancel, const char *log_path,
                       bool own_group = true);
