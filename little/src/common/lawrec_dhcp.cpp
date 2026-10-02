#include "lawrec_dhcp.h"
#include "lawrec_process.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <sstream>
#include <dirent.h>
#include <cstdint>
#include <time.h>

namespace {
bool decimal(const std::string &text, uint64_t maximum, uint64_t &value) {
    value = 0;
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if (c < '0' || c > '9' || value > (maximum-(c-'0'))/10) return false;
        value = value*10+c-'0';
    }
    return true;
}
bool valid_boot_id(const std::string &id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}
bool valid_address(const char *ip) {
    in_addr address{};
    if (!ip || inet_pton(AF_INET, ip, &address) != 1) return false;
    const unsigned first = ntohl(address.s_addr) >> 24;
    return first && first != 127 && first < 224;
}
struct PinnedOwner {
    int fd = -1;
    ~PinnedOwner() { if (fd >= 0) close(fd); }
    int alive() const {
        pollfd p{fd, POLLIN, 0};
        int n;
        do { n = poll(&p, 1, 0); } while (n < 0 && errno == EINTR);
        if (n < 0) return -errno;
        if (n && (p.revents & POLLIN)) return -ENETDOWN;
        return p.revents & (POLLERR | POLLNVAL) ? -EIO : 0;
    }
};
std::string run_dir() {
    const char *p = getenv("LAWREC_NETWORK_RUN_DIR");
    return p && *p ? p : "/var/run";
}
const char *program() {
#ifdef LAWREC_DHCP_TESTING
    return getenv("LAWREC_DHCP_TEST_PROGRAM");
#else
    return "/sbin/udhcpc";
#endif
}
int read_file(const std::string &path, std::string &text, size_t maximum = 512) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct stat st{};
    int ret = fstat(fd, &st) ? -errno : 0;
    if (!ret && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size < 1 ||
                 st.st_size > (off_t)maximum)) ret = -EINVAL;
    std::vector<char> bytes(maximum+1);
    ssize_t n = ret ? -1 : read(fd, bytes.data(), bytes.size());
    if (!ret && n != st.st_size) ret = n < 0 ? -errno : -EIO;
    close(fd);
    if (!ret) text.assign(bytes.data(), n);
    return ret;
}
int write_file(const std::string &path, const std::string &text) {
    std::string temp = path+".XXXXXX";
    int fd = mkstemp(&temp[0]);
    if (fd < 0) return -errno;
    int ret = 0;
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = write(fd, text.data()+done, text.size()-done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ret = n < 0 ? -errno : -EIO; break; }
        done += n;
    }
    if (!ret && fsync(fd)) ret = -errno;
    if (close(fd) && !ret) ret = -errno;
    if (!ret && rename(temp.c_str(), path.c_str())) ret = -errno;
    unlink(temp.c_str());
    return ret;
}
bool valid_job(const std::string &job) {
    std::string prefix = run_dir()+"/lawrec-dhcp-";
    if (job.size() != prefix.size()+6 || job.compare(0, prefix.size(), prefix)) return false;
    for (unsigned char c : job.substr(prefix.size()))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    struct stat st{};
    return !lstat(job.c_str(), &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077);
}
int current_job(std::string &job) {
    int ret = read_file(run_dir()+"/lawrec-dhcp-current", job);
    if (ret) return ret;
    if (!job.empty() && job.back() == '\n') job.pop_back();
    return valid_job(job) ? 0 : -EINVAL;
}
struct Lock {
    int fd = -1;
    ~Lock() { if (fd >= 0) close(fd); }
    int acquire() {
        fd = open((run_dir()+"/lawrec-dhcp.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return -errno;
        if (flock(fd, LOCK_EX | LOCK_NB)) return errno == EWOULDBLOCK ? -EBUSY : -errno;
        return 0;
    }
};

// Hold the pidfd before checking /proc. Even if the PID is reused after this
// check, signalling this descriptor cannot hit the replacement process.
int pin_owner(const std::string &job, pid_t value, int &pidfd) {
    pidfd = -1;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    pidfd = syscall(SYS_pidfd_open, value, 0);
#else
    return -ENOTSUP;
#endif
    if (pidfd < 0) return -errno;
    pollfd exited{pidfd, POLLIN, 0};
    if (poll(&exited, 1, 0) > 0) { close(pidfd); pidfd = -1; return -ESRCH; }
    std::string path = "/proc/"+std::to_string(value)+"/cmdline";
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    char bytes[2048];
    ssize_t n = fd < 0 ? -1 : read(fd, bytes, sizeof(bytes));
    if (fd >= 0) close(fd);
    std::vector<std::string> args;
    if (n > 0 && n < (ssize_t)sizeof(bytes) && !bytes[n-1]) {
        size_t offset = 0;
        while (offset < (size_t)n) {
            args.emplace_back(bytes+offset);
            offset += args.back().size()+1;
        }
    }
    bool iface = false, pidfile = false, hook = false;
    bool correct = !args.empty() && program() && args[0] == program();
    for (size_t i = 1; i+1 < args.size(); ++i) {
        if (args[i] == "-i") iface = args[i+1] == "wlan0";
        if (args[i] == "-p") pidfile = args[i+1] == job+"/pid";
        if (args[i] == "-s") hook = args[i+1] == job+"/hook";
    }
    if (!correct || !iface || !pidfile || !hook) {
        close(pidfd); pidfd = -1;
        return -EPERM;
    }
    return 0;
}
int owner(const std::string &job, pid_t &pid, int &pidfd) {
    pid = 0; pidfd = -1;
    std::string text;
    int ret = read_file(job+"/pid", text, 32);
    if (!ret) {
        char *end;
        errno = 0;
        long value = strtol(text.c_str(), &end, 10);
        if (!errno && end != text.c_str() && value >= 2 && value <= 2147483647L) {
            if (*end == '\n') ++end;
            if (!*end) {
                ret = pin_owner(job, value, pidfd);
                if (!ret) { pid = value; return 0; }
                if (ret != -ESRCH) return ret;
            }
        }
    } else if (ret != -ENOENT && ret != -EINVAL) return ret;
    // During daemonization the PID file can still identify the exited parent.
    // A /proc scan finds its already-created child by the unique job arguments;
    // this is not permission to signal arbitrary processes named udhcpc.
    DIR *proc = opendir("/proc");
    if (!proc) return -errno;
    ret = -ESRCH;
    while (dirent *entry = readdir(proc)) {
        char *end;
        long value = strtol(entry->d_name, &end, 10);
        if (*end || value < 2 || value > 2147483647L) continue;
        std::string path = "/proc/"+std::to_string(value)+"/cmdline";
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char bytes[2048];
        ssize_t n = read(fd, bytes, sizeof(bytes));
        close(fd);
        std::string command;
        if (n > 0 && n < (ssize_t)sizeof(bytes)) command.assign(bytes, n);
        if (command.find(job+"/pid") == std::string::npos) continue;
        int pinned;
        int error = pin_owner(job, value, pinned);
        if (error) { if (error != -ESRCH) ret = error; continue; }
        if (pidfd >= 0) { close(pinned); close(pidfd); pidfd = -1; ret = -EBUSY; break; }
        pid = value; pidfd = pinned; ret = 0;
    }
    closedir(proc);
    if (ret && pidfd >= 0) { close(pidfd); pidfd = -1; }
    return ret;
}
void remove_job(const std::string &job) {
    // Only called after verified exit. Never unlink a live client's lease hook.
    unlink((job+"/pid").c_str()); unlink((job+"/hook").c_str());
    unlink((job+"/lease").c_str()); unlink((job+"/events.log").c_str()); rmdir(job.c_str());
    unlink((run_dir()+"/lawrec-dhcp-current").c_str());
}
int stop_job(const std::string &job) {
    auto end = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end) {
        pid_t pid; int pidfd;
        int ret = owner(job, pid, pidfd);
        if (ret == -ENOENT || ret == -ESRCH) { remove_job(job); return 0; }
        if (ret) return ret;
#if defined(SYS_pidfd_send_signal)
        ret = syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, nullptr, 0) ? -errno : 0;
#endif
        if (!ret || ret == -ESRCH) {
            pollfd p{pidfd, POLLIN, 0};
            ret = -ETIMEDOUT;
            while (std::chrono::steady_clock::now() < end) {
                int n = poll(&p, 1, 100);
                if (n > 0 && (p.revents & POLLIN)) { ret = 0; break; }
                if (n < 0 && errno != EINTR) { ret = -errno; break; }
            }
        }
        close(pidfd);
        fprintf(stderr, "[dhcp] stop pid=%ld result=%d\n", (long)pid, ret);
        // Recheck for a daemon child created just before the original owner exited.
        if (ret) return ret;
    }
    // Do not force-kill a client in a lease hook, or delete uncertain metadata.
    return -ETIMEDOUT;
}
}

int lawrec_dhcp_stop(void) {
    Lock lock;
    int ret = lock.acquire();
    if (ret) return ret;
    std::string job;
    ret = current_job(job);
    if (ret == -ENOENT) return 0;
    return ret ? ret : stop_job(job);
}

int lawrec_dhcp_get(lawrec_dhcp_status *s) {
    if (!s) return -EINVAL;
    *s = lawrec_dhcp_status{};
    std::string job;
    int ret = current_job(job);
    if (ret == -ENOENT) return 0;
    if (ret) return ret;
    PinnedOwner pinned;
    ret = owner(job, s->pid, pinned.fd);
    if (ret == -ENOENT || ret == -ESRCH) { s->error = -ENETDOWN; return 0; }
    if (ret) return ret;
    std::string lease;
    ret = read_file(job+"/lease", lease);
    if (ret == -ENOENT) return 0;
    if (ret) return ret;
    uint64_t magnitude, seconds, received_ms;
    std::string version, error_text, seconds_text, stamp_text, ip, boot, extra;
    std::istringstream fields(lease);
    if (!(fields >> version >> error_text >> seconds_text >> ip >> stamp_text >> boot) ||
        (fields >> extra) || version != "v2" || ip.size() >= 16 || !valid_boot_id(boot) ||
        !decimal(seconds_text, 0xffffffffULL, seconds) || !decimal(stamp_text, UINT64_MAX, received_ms)) return -EPROTO;
    int error = 0;
    if (error_text != "0") {
        if (error_text.empty() || error_text[0] != '-' ||
            !decimal(error_text.substr(1), 4095, magnitude) || !magnitude) return -EPROTO;
        error = -(int)magnitude;
    }
    if ((!error && (!seconds || !valid_address(ip.c_str()))) ||
        (error && (seconds || ip != "-"))) return -EPROTO;
    lawrec_dhcp_stamp now;
    ret = lawrec_dhcp_stamp_now(&now);
    if (ret) return ret;
    std::string current;
    ret = current_job(current);
    if (ret) return ret;
    if (current != job) return -EAGAIN;
    ret = pinned.alive();
    if (ret == -ENETDOWN) { s->pid = 0; s->error = ret; return 0; }
    if (ret) return ret;
    if (boot != now.boot_id) { s->error = -ESTALE; return 0; }
    if (received_ms > now.received_ms || received_ms > UINT64_MAX-seconds*1000) return -EPROTO;
    s->error = error;
    s->lease_seconds = seconds;
    if (!error) {
        const uint64_t end = received_ms + seconds*1000;
        if (now.received_ms >= end) { s->error = -ETIMEDOUT; return 0; }
        s->remaining_seconds = (end-now.received_ms+999)/1000;
        snprintf(s->address, sizeof(s->address), "%s", ip.c_str());
        s->bound = 1;
    }
    return 0;
}

int lawrec_dhcp_hook_verify(const char *path) {
    if (!path || !valid_job(path)) return -EINVAL;
    std::string job;
    int ret = current_job(job);
    if (ret) return ret;
    if (job != path) return -ESTALE;
    pid_t pid; int pidfd;
    ret = owner(job, pid, pidfd);
    if (pidfd >= 0) close(pidfd);
    return ret;
}
int lawrec_dhcp_stamp_now(lawrec_dhcp_stamp *stamp) {
    if (!stamp) return -EINVAL;
    *stamp = {};
#ifdef CLOCK_BOOTTIME
    timespec ts{};
    if (clock_gettime(CLOCK_BOOTTIME, &ts)) return -errno;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 ||
        (uint64_t)ts.tv_sec > (UINT64_MAX-999)/1000) return -EOVERFLOW;
    stamp->received_ms = (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#else
    return -ENOTSUP;
#endif
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    char bytes[38];
    ssize_t n;
    do { n = read(fd, bytes, sizeof(bytes)); } while (n < 0 && errno == EINTR);
    const int saved = errno;
    close(fd);
    if (n < 0) return -saved;
    if (n != 37 || bytes[36] != '\n' || !valid_boot_id(std::string(bytes, 36))) return -EPROTO;
    memcpy(stamp->boot_id, bytes, 36);
    return 0;
}

int lawrec_dhcp_hook_result(const char *job, int error, const char *address, unsigned lease,
                            const lawrec_dhcp_stamp *received) {
    int ret = lawrec_dhcp_hook_verify(job);
    if (ret) return ret;
    if (!address || strnlen(address, 16) >= 16 || error > 0 || error < -4095) return -EINVAL;
    if ((!error && (!lease || !valid_address(address))) || (error && (lease || address[0]))) return -EINVAL;
    lawrec_dhcp_stamp now;
    ret = lawrec_dhcp_stamp_now(&now);
    if (ret) return ret;
    const auto &stamp = received ? *received : now;
    if (strnlen(stamp.boot_id, sizeof(stamp.boot_id)) != 36 ||
        !valid_boot_id(stamp.boot_id) || strcmp(stamp.boot_id, now.boot_id)) return -ESTALE;
    if (stamp.received_ms > now.received_ms ||
        stamp.received_ms > UINT64_MAX-uint64_t(lease)*1000) return -EINVAL;
    int expired = 0;
    if (!error && now.received_ms >= stamp.received_ms+uint64_t(lease)*1000) {
        // Never publish an ACK whose lease expired during a slow hook.
        error = expired = -ETIMEDOUT; lease = 0; address = "";
    }
    char text[160];
    snprintf(text, sizeof(text), "v2 %d %u %s %llu %s\n", error, lease,
        address[0] ? address : "-", (unsigned long long)stamp.received_ms, stamp.boot_id);
    ret = write_file(std::string(job)+"/lease", text);
    return ret ? ret : expired;
}

int lawrec_dhcp_acquire(const std::atomic<bool> &cancel) {
    if (cancel) return -ECANCELED;
    if (!program() || program()[0] != '/') return -EINVAL;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    int probe = syscall(SYS_pidfd_open, getpid(), 0);
    if (probe < 0) return -errno;
    close(probe);
#else
    return -ENOTSUP;
#endif
    Lock lock;
    int ret = lock.acquire();
    if (ret) return ret;
    std::string previous;
    ret = current_job(previous);
    if (!ret) ret = stop_job(previous);
    if (ret && ret != -ENOENT) return ret;
    std::string job = run_dir()+"/lawrec-dhcp-XXXXXX";
    if (!mkdtemp(&job[0])) return -errno;
    const std::string hook = "#!/bin/sh\nexec /app/lawrec/ui/ui --dhcp-hook \"$1\" \"${0%/hook}\" >> \"${0%/hook}/events.log\" 2>&1\n";
    ret = write_file(job+"/hook", hook);
    if (!ret && chmod((job+"/hook").c_str(), 0700)) ret = -errno;
    if (!ret) ret = write_file(run_dir()+"/lawrec-dhcp-current", job+"\n");
    if (ret) { remove_job(job); return ret; }
    std::string pidfile = job+"/pid", script = job+"/hook";
    // BusyBox 1.33 optional arguments must be attached. Conflict replies cause
    // DHCPDECLINE before the bound hook; the hook also checks transport errors.
    std::vector<std::string> args{program(), "-n", "-a2000", "-i", "wlan0", "-p", pidfile, "-s", script, "-t", "4", "-T", "3"};
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(&arg[0]);
    argv.push_back(nullptr);
    ret = lawrec_process_run(program(), argv.data(), 30000, cancel, "/tmp/lawrec-dhcp.log");
    // BusyBox's original process exits before the daemon has rewritten its PID
    // file. Wait for a verified owner AND a successful configuration hook.
    auto end = std::chrono::steady_clock::now()+std::chrono::seconds(1);
    int failure = ret;
    while (!ret && std::chrono::steady_clock::now() < end) {
        if (cancel) { failure = -ECANCELED; break; }
        lawrec_dhcp_status status;
        int error = lawrec_dhcp_get(&status);
        if (!error && status.pid && status.bound) {
            fprintf(stderr, "[dhcp] acquired pid=%ld lease_seconds=%u continuous=1\n", (long)status.pid, status.lease_seconds);
            return 0;
        }
        if (!error && status.error) { failure = status.error; break; }
        if (error && error != -EPERM) { failure = error; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!failure) failure = -EIO;
    int cleanup = stop_job(job);
    fprintf(stderr, "[dhcp] acquire result=%d cleanup=%d\n", failure, cleanup);
    return cleanup ? cleanup : failure;
}
