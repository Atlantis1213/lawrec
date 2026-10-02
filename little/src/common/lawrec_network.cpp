#include "lawrec_network.h"
#include "lawrec_process.h"
#include "lawrec_wifi_transaction.h"
#include "lawrec_ipv4_apply.h"
#include "lawrec_ipv4_transaction.h"
#include "lawrec_dhcp.h"
#include "lawrec_arp.h"
#include <ifaddrs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
struct Runtime {
    std::mutex lock;
    std::thread worker;
    std::atomic<bool> cancel{false};
    bool closing = false;
    Clock::time_point last_lease_poll{};
    lawrec_network_snapshot snapshot{};
    ~Runtime() { cancel = true; if (worker.joinable()) worker.join(); }
} runtime;

std::string ipv4_lock_path() {
    const char *dir = getenv("LAWREC_NETWORK_RUN_DIR");
    return std::string(dir && *dir ? dir : "/var/run") + "/lawrec-ipv4-boot.lock";
}

struct Ipv4ApplyLock {
    int fd = -1;
    ~Ipv4ApplyLock() { if (fd >= 0) close(fd); }
    int acquire() {
        std::string path = ipv4_lock_path();
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (fd < 0) return -errno;
        struct stat st{};
        if (fstat(fd, &st)) return -errno;
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) return -EPERM;
        if (flock(fd, LOCK_EX | LOCK_NB)) return errno == EWOULDBLOCK ? -EBUSY : -errno;
        return 0;
    }
};

struct Control {
    int fd = -1;
    bool directory_created = false;
    bool scan_ready = false, scan_failed = false;
    bool cancelable = true;
    char dir[64] = "/tmp/lawrec-wpa-XXXXXX";
    std::string path;
    ~Control() {
        if (fd >= 0) close(fd);
        if (!path.empty()) unlink(path.c_str());
        if (directory_created) rmdir(dir);
    }
    int open_socket() {
        const char *peer = getenv("LAWREC_WPA_CTRL_PATH");
        if (!peer || !*peer) peer = "/var/run/wpa_supplicant/wlan0";
        sockaddr_un remote{};
        remote.sun_family = AF_UNIX;
        if (strlen(peer) >= sizeof(remote.sun_path)) return -ENAMETOOLONG;
        strcpy(remote.sun_path, peer);
        if (!mkdtemp(dir)) return -errno;
        directory_created = true;
        path = std::string(dir) + "/control";
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return -errno;
        sockaddr_un local{};
        local.sun_family = AF_UNIX;
        strcpy(local.sun_path, path.c_str());
        if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local))) return -errno;
        if (connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote))) return -errno;
        return 0;
    }
    int receive(std::string &reply, Clock::time_point deadline) {
        while (!cancelable || !runtime.cancel) {
            if (Clock::now() >= deadline) return -ETIMEDOUT;
            pollfd p{fd, POLLIN, 0};
            int ret = poll(&p, 1, 100);
            if (ret < 0) { if (errno == EINTR) continue; return -errno; }
            if (!ret) continue;
            if (!(p.revents & POLLIN)) return -EIO;
            char data[16384];
            ssize_t n = recv(fd, data, sizeof(data), MSG_TRUNC);
            if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; return -errno; }
            if ((size_t)n >= sizeof(data)) return -EMSGSIZE;
            reply.assign(data, n);
            return 0;
        }
        return -ECANCELED;
    }
    int request(const char *command, std::string &reply) {
        if (cancelable && runtime.cancel) return -ECANCELED;
        ssize_t n = send(fd, command, strlen(command), MSG_NOSIGNAL);
        if (n < 0) return -errno;
        if ((size_t)n != strlen(command)) return -EIO;
        auto deadline = Clock::now() + std::chrono::seconds(2);
        for (;;) {
            int ret = receive(reply, deadline);
            if (ret) return ret;
            // Events can arrive between a command and its response.
            if (!reply.empty() && reply[0] == '<') {
                if (reply.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) scan_ready = true;
                if (reply.find("CTRL-EVENT-SCAN-FAILED") != std::string::npos) scan_failed = true;
                continue;
            }
            if (reply.compare(0, 4, "FAIL") == 0) return -EIO;
            if (reply.compare(0, 7, "UNKNOWN") == 0) return -ENOTSUP;
            return 0;
        }
    }
};

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Decode supplicant's printf_encode format, not a shell or C expression.
bool decode_ssid(const std::string &text, char out[33]) {
    std::string decoded;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = text[i];
        if (c == '\\') {
            if (++i >= text.size()) return false;
            c = text[i];
            if (c == 'x') {
                if (i + 2 >= text.size() || hex(text[i+1]) < 0 || hex(text[i+2]) < 0) return false;
                c = hex(text[i+1]) * 16 + hex(text[i+2]); i += 2;
            } else if (c != '\\' && c != '"') return false;
        }
        // Embedded control bytes cannot be represented safely in the UI fields.
        if (c < 32 || c == 127 || decoded.size() >= 32) return false;
        decoded += c;
    }
    memcpy(out, decoded.c_str(), decoded.size()+1);
    return true;
}

void parse_status(const std::string &reply, lawrec_network_snapshot &s) {
    std::istringstream lines(reply);
    std::string line;
    s.state[0] = s.ssid[0] = s.ipv4[0] = 0;
    while (std::getline(lines, line)) {
        if (line.compare(0, 10, "wpa_state=") == 0)
            snprintf(s.state, sizeof(s.state), "%s", line.c_str()+10);
        else if (line.compare(0, 5, "ssid=") == 0)
            decode_ssid(line.substr(5), s.ssid);
        else if (line.compare(0, 11, "ip_address=") == 0)
            snprintf(s.ipv4, sizeof(s.ipv4), "%s", line.c_str()+11);
    }
}

void parse_scan(const std::string &reply, lawrec_network_snapshot &s) {
    s.count = 0;
    std::istringstream lines(reply);
    std::string line;
    std::getline(lines, line); // Header: bssid / frequency / signal / flags / ssid.
    while (s.count < LAWREC_WIFI_AP_MAX && std::getline(lines, line)) {
        size_t tabs[4], begin = 0;
        bool valid = true;
        for (int i = 0; i < 4; ++i) {
            tabs[i] = line.find('\t', begin);
            if (tabs[i] == std::string::npos) { valid = false; break; }
            begin = tabs[i]+1;
        }
        if (!valid || tabs[0] != 17) continue;
        lawrec_wifi_ap ap{};
        if (!decode_ssid(line.substr(begin), ap.ssid) || !ap.ssid[0]) continue;
        std::string level = line.substr(tabs[1]+1, tabs[2]-tabs[1]-1);
        char *end;
        long signal = strtol(level.c_str(), &end, 10);
        if (end == level.c_str() || *end || signal < -127 || signal > 0) continue;
        ap.signal_dbm = signal;
        memcpy(ap.bssid, line.data(), 17);
        snprintf(ap.flags, sizeof(ap.flags), "%s", line.substr(tabs[2]+1, tabs[3]-tabs[2]-1).c_str());
        s.aps[s.count++] = ap;
    }
}

int collect(bool scan, lawrec_network_snapshot &s, bool recovery = false) {
    Control control;
    control.cancelable = !recovery;
    int ret = control.open_socket();
    if (ret) return ret;
    std::string reply;
    ret = control.request("STATUS", reply);
    if (ret) return ret;
    parse_status(reply, s);
    if (!s.state[0]) return -EPROTO;
    lawrec_dhcp_status lease;
    int lease_error = lawrec_dhcp_get(&lease);
    s.dhcp_pid = lease.pid;
    s.dhcp_bound = lease.bound;
    s.dhcp_error = lease_error ? lease_error : lease.error;
    s.lease_seconds = lease.lease_seconds;
    s.lease_remaining_seconds = lease.remaining_seconds;
    if (!scan) return 0;
    s.count = 0;
    ret = control.request("ATTACH", reply);
    if (ret || reply != "OK\n") return ret ? ret : -EPROTO;
    control.scan_ready = control.scan_failed = false;
    ret = control.request("SCAN", reply);
    if (ret || reply != "OK\n") return ret ? ret : -EPROTO;
    auto deadline = Clock::now() + std::chrono::seconds(12);
    while (!control.scan_ready) {
        if (control.scan_failed) return -EIO;
        ret = control.receive(reply, deadline);
        if (ret) return ret;
        if (reply.find("CTRL-EVENT-SCAN-FAILED") != std::string::npos) return -EIO;
        if (reply.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) break;
    }
    ret = control.request("SCAN_RESULTS", reply);
    if (ret) return ret;
    if (reply.compare(0, 5, "bssid") != 0) return -EPROTO;
    parse_scan(reply, s);
    return 0;
}

int check_dhcp_busy(bool boot = false, bool lock_owned = false) {
    // Refuse overlapping DHCP clients rather than killing a boot worker or a
    // client belonging to another interface. No global route deletion here.
    if (!boot && !lock_owned) {
        int fd = open(ipv4_lock_path().c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd >= 0) {
            struct stat st{};
            int ret = fstat(fd, &st) ? -errno : 0;
            if (!ret && (!S_ISREG(st.st_mode) || st.st_uid != geteuid())) ret = -EPERM;
            if (!ret && flock(fd, LOCK_EX | LOCK_NB)) ret = -errno;
            close(fd);
            if (ret) return ret == -EWOULDBLOCK ? -EBUSY : ret;
        } else if (errno != ENOENT) return -errno;
    }
    lawrec_dhcp_status owned;
    int owner_error = lawrec_dhcp_get(&owned);
    if (owner_error) return owner_error;
    DIR *proc = opendir("/proc");
    if (!proc) return -errno;
    bool occupied = false;
    while (dirent *entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[320], name[64] = {};
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(name, sizeof(name), fp) && !strncmp(name, "udhcpc\n", 7) &&
            strtol(entry->d_name, nullptr, 10) != owned.pid) occupied = true;
        fclose(fp);
        // The boot worker can be sleeping/associating before spawning DHCP.
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        fp = fopen(path, "r");
        if (fp) {
            char command[1024] = {};
            size_t n = fread(command, 1, sizeof(command)-1, fp);
            fclose(fp);
            for (size_t i = 0; i < n; ++i) if (!command[i]) command[i] = ' ';
            if (!boot && strstr(command, "/etc/init.d/S45wifi") && strstr(command, "worker")) occupied = true;
        }
    }
    closedir(proc);
    return occupied ? -EBUSY : 0;
}

int renew_dhcp(lawrec_network_snapshot &s, bool recovery = false, bool boot = false,
               bool lock_owned = false) {
    int ret = check_dhcp_busy(boot, lock_owned);
    if (ret) return ret;
    ret = collect(false, s, recovery);
    if (ret) return ret;
    if (strcmp(s.state, "COMPLETED")) return -ENOTCONN;
    const std::atomic<bool> no_cancel{false};
    ret = lawrec_dhcp_acquire(recovery ? no_cancel : runtime.cancel);
    if (ret) return ret;
    ret = collect(false, s, recovery);
    if (ret) return ret;
    in_addr address{};
    lawrec_dhcp_status lease;
    ret = lawrec_dhcp_get(&lease);
    if (ret) return ret;
    if (!lease.bound || strcmp(s.ipv4, lease.address) || strcmp(s.state, "COMPLETED") || inet_pton(AF_INET, s.ipv4, &address) != 1 ||
        address.s_addr == INADDR_ANY) return -EADDRNOTAVAIL;
    return 0;
}

int static_dns(const lawrec_ipv4_settings &s) {
    // The SDK resolver is a symlink into /tmp. Replace its target atomically,
    // not the symlink itself, and preserve entries owned by other interfaces.
    char link[128] = {};
    ssize_t n = readlink("/etc/resolv.conf", link, sizeof(link)-1);
    if (n < 0 || (strcmp(link, "../tmp/resolv.conf") && strcmp(link, "/tmp/resolv.conf"))) return -ENOTSUP;
    std::string existing;
    int fd = open("/tmp/resolv.conf", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno != ENOENT) return -errno;
    if (fd >= 0) {
        struct stat st{};
        int ret = fstat(fd, &st) ? -errno : 0;
        if (!ret && (!S_ISREG(st.st_mode) || st.st_size > 8192)) ret = -EINVAL;
        char data[8193];
        ssize_t bytes = ret ? -1 : read(fd, data, sizeof(data));
        if (!ret && bytes != st.st_size) ret = bytes < 0 ? -errno : -EIO;
        close(fd);
        if (ret) return ret;
        existing.assign(data, bytes);
    }
    const std::string text = lawrec_ipv4_resolver_text(s, existing);
    char temp[] = "/tmp/lawrec-resolv-XXXXXX";
    fd = mkstemp(temp);
    if (fd < 0) return -errno;
    int ret = fchmod(fd, 0644) ? -errno : 0;
    size_t done = 0;
    while (!ret && done < text.size()) {
        ssize_t bytes = write(fd, text.data()+done, text.size()-done);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) ret = bytes < 0 ? -errno : -EIO;
        else done += bytes;
    }
    if (!ret && fsync(fd)) ret = -errno;
    if (close(fd) && !ret) ret = -errno;
    if (!ret && rename(temp, "/tmp/resolv.conf")) ret = -errno;
    unlink(temp);
    return ret;
}

struct Ipv4Backend : LawrecIpv4Backend {
    bool recovery;
    bool hook;
    explicit Ipv4Backend(bool value, bool from_hook = false) : recovery(value), hook(from_hook) {}
    int run(const std::vector<std::string> &args) override {
        std::vector<char *> argv;
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        const std::atomic<bool> no_cancel{false};
        return lawrec_process_run(args[0].c_str(), argv.data(), 5000,
                                  recovery ? no_cancel : runtime.cancel, "/tmp/lawrec-ipv4.log", !hook);
    }
    int dns(const lawrec_ipv4_settings &s) override { return static_dns(s); }
    int probe(const lawrec_ipv4_settings &s) override {
        const std::atomic<bool> no_cancel{false};
        return lawrec_arp_probe(s.address, recovery ? no_cancel : runtime.cancel);
    }
};

int configure_static(const lawrec_ipv4_settings &settings, bool recovery, bool prepared = false) {
    Ipv4Backend backend(recovery);
    // Only the live transaction's already-checked candidate may skip a second
    // probe; boot, WiFi switching and static rollback always probe themselves.
    int ret = prepared ? lawrec_ipv4_apply_lease(backend, settings) : lawrec_ipv4_apply_static(backend, settings);
    fprintf(stderr, "[network] phase=static-ipv4 result=%d recovery=%d\n", ret, recovery);
    return ret;
}

struct WifiBackend : LawrecWifiBackend {
    const lawrec_ipv4_settings policy;
    explicit WifiBackend(const lawrec_ipv4_settings &active) : policy(active) {}
    int begin() override {
        int ret = lawrec_settings_ipv4_begin_apply();
        fprintf(stderr, "[network] phase=wifi-begin result=%d dhcp=%u\n", ret, policy.dhcp);
        return ret;
    }
    int before_select() override { return lawrec_dhcp_stop(); }
    int before_rollback() override {
        int ret = lawrec_dhcp_stop();
        fprintf(stderr, "[network] phase=wifi-recovery-stop result=%d\n", ret);
        return ret;
    }
    int restore_disconnected() override {
        Ipv4Backend backend(true);
        int ret = backend.run({"/sbin/ip", "-4", "route", "flush", "default", "dev", "wlan0"});
        if (!ret) ret = backend.run({"/sbin/ip", "-4", "addr", "flush", "dev", "wlan0", "scope", "global"});
        lawrec_ipv4_settings empty{};
        if (!ret) ret = backend.dns(empty);
        fprintf(stderr, "[network] phase=wifi-recovery-disconnected result=%d\n", ret);
        return ret;
    }
    int commit(bool recovery) override {
        int ret = lawrec_settings_ipv4_note_active(&policy);
        fprintf(stderr, "[network] phase=wifi-commit result=%d recovery=%d dhcp=%u\n", ret, recovery, policy.dhcp);
        return ret;
    }
    int request(const std::string &command, std::string &reply, bool recovery) override {
        // Each request owns a fresh socket: a delayed response after a timeout
        // must never be mistaken for a rollback command's acknowledgement.
        Control c;
        c.cancelable = !recovery;
        int ret = c.open_socket();
        return ret ? ret : c.request(command.c_str(), reply);
    }
    int wait_connected(int id, bool recovery) override {
        fprintf(stderr, "[network] phase=associate id=%d recovery=%d\n", id, recovery);
        auto deadline = Clock::now() + std::chrono::seconds(30);
        while (Clock::now() < deadline) {
            if (!recovery && runtime.cancel) return -ECANCELED;
            std::string reply;
            int ret = request("STATUS", reply, recovery);
            if (ret) return ret;
            lawrec_network_snapshot status{};
            parse_status(reply, status);
            std::istringstream lines(reply);
            std::string line;
            bool matching = false;
            while (std::getline(lines, line)) {
                if (line == "id=" + std::to_string(id)) matching = true;
            }
            if (matching && !strcmp(status.state, "COMPLETED")) return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return -ETIMEDOUT;
    }
    int dhcp(bool recovery) override {
        // Save is a next-boot draft. A live switch/rollback uses the policy that
        // actually succeeded at boot, not a newly edited future configuration.
        // The marker deliberately makes active() unreadable during switching.
        // Use the validated policy captured under the application lock instead.
        if (!policy.dhcp) {
            int ret = configure_static(policy, recovery);
            lawrec_network_snapshot status{};
            if (!ret) ret = collect(false, status, recovery);
            if (!ret && (strcmp(status.state, "COMPLETED") || strcmp(status.ipv4, policy.address)))
                ret = -EADDRNOTAVAIL;
            return ret;
        }
        fprintf(stderr, "[network] phase=dhcp recovery=%d\n", recovery);
        lawrec_network_snapshot s{};
        return renew_dhcp(s, recovery, false, true);
    }
};

int connect_wifi(const std::string &ssid, const std::string &password, lawrec_network_snapshot &s) {
    // Serialize with headless IPv4 boot/application through the entire switch,
    // including old DHCP stop, association, hook completion and recovery.
    Ipv4ApplyLock lock;
    int ret = lock.acquire();
    if (!ret) ret = check_dhcp_busy(false, true);
    if (ret) return ret;
    lawrec_ipv4_settings active;
    ret = lawrec_settings_ipv4_active(&active);
    if (ret) return ret; // Refuse before SELECT_NETWORK if boot left an uncertain policy.
    WifiBackend backend(active);
    LawrecWifiResult result = lawrec_wifi_connect(backend, ssid, password);
    s.rollback_error = result.rollback_error;
    s.connection_changed = result.changed;
    if (!result.error || !result.rollback_error) {
        int status_error;
        try { status_error = collect(false, s, true); }
        catch (...) { status_error = -EFAULT; }
        // A late UI refresh failure does not undo a committed network change.
        if (status_error) fprintf(stderr, "[network] phase=wifi-refresh result=%d\n", status_error);
    }
    fprintf(stderr, "[network] phase=connect result=%d rollback=%d changed=%d\n",
            result.error, result.rollback_error, result.changed);
    return result.error;
}

struct Ipv4TransactionBackend : LawrecIpv4TransactionBackend {
    bool prepared = false;
    int preflight(const lawrec_ipv4_settings &desired) override {
        int ret = check_dhcp_busy(false, true);
        lawrec_network_snapshot status{};
        if (!ret) ret = collect(false, status);
        if (!ret && strcmp(status.state, "COMPLETED")) ret = -ENOTCONN;
        if (!ret && !desired.dhcp) ret = lawrec_arp_probe(desired.address, runtime.cancel);
        prepared = !ret && !desired.dhcp;
        return ret;
    }
    int begin() override { return lawrec_settings_ipv4_begin_apply(); }
    int stop_dhcp() override { return lawrec_dhcp_stop(); }
    int apply(const lawrec_ipv4_settings &settings, bool recovery) override {
        lawrec_network_snapshot status{};
        if (settings.dhcp) return renew_dhcp(status, recovery, true);
        int ret = configure_static(settings, recovery, prepared && !recovery);
        prepared = false;
        if (!ret) ret = collect(false, status, recovery);
        if (!ret && (strcmp(status.state, "COMPLETED") || strcmp(status.ipv4, settings.address)))
            ret = -EADDRNOTAVAIL;
        if (!ret) {
            struct ifaddrs *list;
            if (getifaddrs(&list)) return -errno;
            bool matches = false;
            in_addr expected{};
            inet_pton(AF_INET, settings.address, &expected);
            for (auto *p = list; p; p = p->ifa_next) {
                if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET || strcmp(p->ifa_name, "wlan0")) continue;
                auto *address = reinterpret_cast<sockaddr_in *>(p->ifa_addr);
                auto *mask = reinterpret_cast<sockaddr_in *>(p->ifa_netmask);
                if (address->sin_addr.s_addr == expected.s_addr &&
                    ntohl(mask->sin_addr.s_addr) == (0xffffffffu << (32-settings.prefix))) matches = true;
            }
            freeifaddrs(list);
            if (!matches) ret = -EADDRNOTAVAIL;
        }
        return ret;
    }
    int commit(const lawrec_ipv4_settings &settings) override {
        return lawrec_settings_ipv4_note_active(&settings);
    }
};

int apply_ipv4(const lawrec_ipv4_settings &desired, lawrec_network_snapshot &status) {
    // Share the boot lock with S45wifi, and hold it through rollback. The UI
    // worker serializes its own requests; this lock also excludes headless boot.
    Ipv4ApplyLock lock;
    int ret = lock.acquire();
    lawrec_ipv4_settings previous{};
    if (!ret) ret = lawrec_settings_ipv4_active(&previous);
    if (!ret) {
        Ipv4TransactionBackend backend;
        LawrecIpv4Result result = lawrec_ipv4_switch(backend, previous, desired);
        ret = result.error;
        status.rollback_error = result.rollback_error;
        status.ipv4_changed = result.changed;
        int observed;
        try { observed = collect(false, status, true); }
        catch (...) { observed = -EFAULT; }
        // Applying and committing succeeded; an unrelated status refresh error
        // must not retrospectively claim that a rollback was performed.
        if (observed) fprintf(stderr, "[ipv4] result refresh error=%d\n", observed);
    }
    return ret;
}
}

static int start_request(int operation, const std::string &ssid = {}, const std::string &password = {},
                         lawrec_ipv4_settings ipv4 = {}) {
    std::lock_guard<std::mutex> guard(runtime.lock);
    if (runtime.closing) return -ESHUTDOWN;
    if (runtime.snapshot.busy) return -EBUSY;
    if (runtime.worker.joinable()) runtime.worker.join();
    runtime.snapshot.busy = 1;
    runtime.snapshot.error = 0;
    try {
        runtime.worker = std::thread([operation, ssid, password, ipv4]() {
            lawrec_network_snapshot next{};
            next.scan_result = operation == 1;
            int ret;
            try {
                ret = operation == 4 ? apply_ipv4(ipv4, next) :
                      operation == 3 ? connect_wifi(ssid, password, next) :
                      operation == 2 ? renew_dhcp(next) : collect(operation == 1, next);
            }
            catch (...) { ret = -EFAULT; }
            fprintf(stderr, "[network] request=%s result=%d aps=%d\n",
                    operation == 4 ? "ipv4" : operation == 3 ? "connect" : operation == 2 ? "dhcp" : operation == 1 ? "scan" : "status", ret, next.count);
            std::lock_guard<std::mutex> guard(runtime.lock);
            next.generation = runtime.snapshot.generation + 1;
            next.scan_generation = operation == 1 && !ret ? next.generation : runtime.snapshot.scan_generation;
            next.error = ret;
            runtime.snapshot = next;
        });
    } catch (...) {
        runtime.snapshot.busy = 0;
        runtime.snapshot.error = -EAGAIN;
        ++runtime.snapshot.generation;
        return -EAGAIN;
    }
    return 0;
}

extern "C" int lawrec_network_refresh_async(int scan) { return start_request(scan ? 1 : 0); }
extern "C" int lawrec_network_renew_async(void) {
    lawrec_ipv4_settings settings;
    int ret = lawrec_settings_ipv4_active(&settings);
    if (ret) return ret;
    if (!settings.dhcp) return -ENOTSUP;
    return start_request(2);
}
extern "C" int lawrec_network_connect_async(const char *ssid, const char *password) {
    if (!ssid || !password || !*ssid || strnlen(ssid, 33) > 32 ||
        strnlen(password, 64) < 8 || strnlen(password, 64) > 63) return -EINVAL;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(password); *p; ++p)
        if (*p < 32 || *p > 126 || *p == '"' || *p == '\\') return -EINVAL;
    try { return start_request(3, ssid, password); }
    catch (...) { return -ENOMEM; }
}

extern "C" int lawrec_network_ipv4_apply_async(const lawrec_ipv4_settings *settings) {
    int ret = lawrec_settings_ipv4_validate(settings);
    if (ret) return ret;
    return start_request(4, {}, {}, *settings);
}

extern "C" void lawrec_network_get(lawrec_network_snapshot *result) {
    if (!result) return;
    unsigned generation;
    {
        std::lock_guard<std::mutex> guard(runtime.lock);
        *result = runtime.snapshot;
        const auto now = Clock::now();
        if (runtime.closing || runtime.snapshot.busy || now-runtime.last_lease_poll < std::chrono::seconds(1)) return;
        runtime.last_lease_poll = now;
        generation = runtime.snapshot.generation;
    }
    // A background udhcpc outlives the UI/network worker. Read its status at
    // most once a second, outside the UI snapshot mutex; no IP mutation here.
    lawrec_dhcp_status lease;
    int error;
    try { error = lawrec_dhcp_get(&lease); }
    catch (...) { error = -ENOMEM; }
    const int lease_error = error ? error : lease.error;
    if (error) { lease.bound = 0; lease.lease_seconds = lease.remaining_seconds = 0; }
    bool transition = false;
    {
        std::lock_guard<std::mutex> guard(runtime.lock);
        auto &s = runtime.snapshot;
        if (!runtime.closing && !s.busy && s.generation == generation &&
            (s.dhcp_pid != lease.pid || s.dhcp_bound != lease.bound || s.dhcp_error != lease_error ||
             s.lease_seconds != lease.lease_seconds || s.lease_remaining_seconds != lease.remaining_seconds)) {
            transition = s.dhcp_pid != lease.pid || s.dhcp_bound != lease.bound || s.dhcp_error != lease_error;
            s.dhcp_pid = lease.pid; s.dhcp_bound = lease.bound;
            s.dhcp_error = lease_error; s.lease_seconds = lease.lease_seconds;
            s.lease_remaining_seconds = lease.remaining_seconds;
            ++s.generation;
        }
        *result = s;
    }
    if (transition) fprintf(stderr, "[network] phase=dhcp-observe generation=%u pid=%d bound=%d error=%d lease_seconds=%u remaining_seconds=%u\n",
        result->generation, result->dhcp_pid, result->dhcp_bound, result->dhcp_error,
        result->lease_seconds, result->lease_remaining_seconds);
}

extern "C" int lawrec_network_boot_configure(void) {
    // This entry is intentionally separate from the UI/network worker. It is
    // called by S45wifi only after association and never starts DRM or media.
    Ipv4ApplyLock lock;
    int ret = lock.acquire();
    lawrec_ipv4_settings settings{};
    lawrec_network_snapshot status{};
    if (!ret) ret = lawrec_settings_ipv4_begin_apply();
    if (!ret) ret = lawrec_settings_ipv4_boot(&settings);
    if (!ret) ret = check_dhcp_busy(true);
    if (!ret) ret = collect(false, status);
    if (!ret && strcmp(status.state, "COMPLETED")) ret = -ENOTCONN;
    if (!ret) ret = lawrec_dhcp_stop();
    if (!ret) ret = settings.dhcp ? renew_dhcp(status, false, true) : configure_static(settings, false);
    if (!ret) ret = lawrec_settings_ipv4_note_active(&settings);
    fprintf(stderr, "[network] boot ipv4 result=%d dhcp=%u\n", ret, settings.dhcp);
    return ret;
}

extern "C" int lawrec_network_dhcp_stop(void) { return lawrec_dhcp_stop(); }

extern "C" int lawrec_network_dhcp_hook(const char *event, const char *job) {
    int ret = lawrec_dhcp_hook_verify(job);
    const char *iface = getenv("interface");
    if (ret) return ret;
    if (!event || !iface || strcmp(iface, "wlan0")) return -EINVAL;
    lawrec_dhcp_stamp received;
    ret = lawrec_dhcp_stamp_now(&received);
    if (ret) return ret;
    Ipv4Backend backend(true, true);
    if (!strcmp(event, "deconfig")) {
        ret = lawrec_dhcp_hook_result(job, -EINPROGRESS, "", 0);
        if (!ret) ret = backend.run({"/sbin/ip", "-4", "route", "flush", "default", "dev", "wlan0"});
        if (!ret) ret = backend.run({"/sbin/ip", "-4", "addr", "flush", "dev", "wlan0", "scope", "global"});
        lawrec_ipv4_settings empty{};
        if (!ret) ret = backend.dns(empty);
        int marker = lawrec_dhcp_hook_result(job, ret ? ret : -ENETDOWN, "", 0);
        return ret ? ret : marker;
    }
    if (!strcmp(event, "nak") || !strcmp(event, "leasefail"))
        return lawrec_dhcp_hook_result(job, -ENETDOWN, "", 0);
    if (strcmp(event, "bound") && strcmp(event, "renew")) return -ENOTSUP;
    lawrec_ipv4_settings settings;
    ret = lawrec_ipv4_lease_settings(getenv("ip"), getenv("subnet"), getenv("router"), getenv("dns"), &settings);
    const char *routes = getenv("staticroutes");
    if (!ret && routes && *routes) ret = -ENOTSUP;
    const char *lease = getenv("lease");
    unsigned seconds = 0;
    if (!lease || !*lease || strnlen(lease, 16) >= 16) ret = -EINVAL;
    else {
        for (const unsigned char *p = (const unsigned char *)lease; *p; ++p) {
            if (*p < '0' || *p > '9' || seconds > (0xffffffffu-(*p-'0'))/10) { ret = -EINVAL; break; }
            seconds = seconds*10+*p-'0';
        }
        if (!seconds) ret = -EINVAL;
    }
    if (!ret) ret = lawrec_dhcp_hook_result(job, -EINPROGRESS, "", 0);
    bool keep_address = false;
    if (!ret) {
        struct ifaddrs *list;
        if (getifaddrs(&list)) ret = -errno;
        else {
            in_addr desired{};
            inet_pton(AF_INET, settings.address, &desired);
            for (auto *p = list; p; p = p->ifa_next) {
                if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET || strcmp(p->ifa_name, "wlan0")) continue;
                auto *address = reinterpret_cast<sockaddr_in *>(p->ifa_addr);
                auto *mask = reinterpret_cast<sockaddr_in *>(p->ifa_netmask);
                if (address->sin_addr.s_addr == desired.s_addr &&
                    ntohl(mask->sin_addr.s_addr) == (0xffffffffu << (32-settings.prefix))) keep_address = true;
            }
            freeifaddrs(list);
        }
    }
    // BusyBox's built-in ARP errors are not all fail-closed. Independently
    // verify a new lease before changing any IP/routes/DNS or marking it bound.
    if (!ret && !keep_address) ret = backend.probe(settings);
    if (!ret) ret = lawrec_ipv4_apply_lease(backend, settings, keep_address);
    int marker = lawrec_dhcp_hook_result(job, ret, ret ? "" : settings.address, ret ? 0 : seconds, &received);
    fprintf(stderr, "[dhcp] hook event=%s result=%d marker=%d keep_address=%d\n", event, ret, marker, keep_address);
    return ret ? ret : marker;
}

extern "C" void lawrec_network_shutdown(void) {
    std::thread worker;
    {
        std::lock_guard<std::mutex> guard(runtime.lock);
        runtime.closing = true;
        runtime.cancel = true;
        worker = std::move(runtime.worker);
    }
    if (worker.joinable()) worker.join();
}
