#include "lawrec_network.h"
#include "lawrec_process.h"
#include "lawrec_wifi_transaction.h"
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
    lawrec_network_snapshot snapshot{};
    ~Runtime() { cancel = true; if (worker.joinable()) worker.join(); }
} runtime;

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

int check_dhcp_busy() {
    // Refuse overlapping DHCP clients rather than killing a boot worker or a
    // client belonging to another interface. No global route deletion here.
    DIR *proc = opendir("/proc");
    if (!proc) return -errno;
    bool occupied = false;
    while (dirent *entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[320], name[64] = {};
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(name, sizeof(name), fp) && !strncmp(name, "udhcpc\n", 7)) occupied = true;
        fclose(fp);
        // The boot worker can be sleeping/associating before spawning DHCP.
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        fp = fopen(path, "r");
        if (fp) {
            char command[1024] = {};
            size_t n = fread(command, 1, sizeof(command)-1, fp);
            fclose(fp);
            for (size_t i = 0; i < n; ++i) if (!command[i]) command[i] = ' ';
            if (strstr(command, "/etc/init.d/S45wifi") && strstr(command, "worker")) occupied = true;
        }
    }
    closedir(proc);
    return occupied ? -EBUSY : 0;
}

int renew_dhcp(lawrec_network_snapshot &s, bool recovery = false) {
    int ret = check_dhcp_busy();
    if (ret) return ret;
    ret = collect(false, s, recovery);
    if (ret) return ret;
    if (strcmp(s.state, "COMPLETED")) return -ENOTCONN;
    char executable[] = "/sbin/udhcpc";
    char iface[] = "wlan0", opt_i[] = "-i", opt_f[] = "-f", opt_q[] = "-q";
    char opt_n[] = "-n", opt_t[] = "-t", tries[] = "4", opt_T[] = "-T", seconds[] = "3";
    char *args[] = {executable, opt_i, iface, opt_f, opt_q, opt_n, opt_t, tries, opt_T, seconds, nullptr};
    const std::atomic<bool> no_cancel{false};
    ret = lawrec_process_run(executable, args, 20000, recovery ? no_cancel : runtime.cancel, "/tmp/lawrec-dhcp.log");
    if (ret) return ret;
    ret = collect(false, s, recovery);
    if (ret) return ret;
    in_addr address{};
    if (strcmp(s.state, "COMPLETED") || inet_pton(AF_INET, s.ipv4, &address) != 1 ||
        address.s_addr == INADDR_ANY) return -EADDRNOTAVAIL;
    return 0;
}

struct WifiBackend : LawrecWifiBackend {
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
        fprintf(stderr, "[network] phase=dhcp recovery=%d\n", recovery);
        lawrec_network_snapshot s{};
        return renew_dhcp(s, recovery);
    }
};

int connect_wifi(const std::string &ssid, const std::string &password, lawrec_network_snapshot &s) {
    int ret = check_dhcp_busy();
    if (ret) return ret;
    WifiBackend backend;
    LawrecWifiResult result = lawrec_wifi_connect(backend, ssid, password);
    s.rollback_error = result.rollback_error;
    s.connection_changed = result.changed;
    if (!result.error || !result.rollback_error) {
        int status_error = collect(false, s, true);
        if (!result.error && status_error) result.error = status_error;
    }
    fprintf(stderr, "[network] phase=connect result=%d rollback=%d changed=%d\n",
            result.error, result.rollback_error, result.changed);
    return result.error;
}
}

static int start_request(int operation, const std::string &ssid = {}, const std::string &password = {}) {
    std::lock_guard<std::mutex> guard(runtime.lock);
    if (runtime.closing) return -ESHUTDOWN;
    if (runtime.snapshot.busy) return -EBUSY;
    if (runtime.worker.joinable()) runtime.worker.join();
    runtime.snapshot.busy = 1;
    runtime.snapshot.error = 0;
    try {
        runtime.worker = std::thread([operation, ssid, password]() {
            lawrec_network_snapshot next{};
            next.scan_result = operation == 1;
            int ret;
            try {
                ret = operation == 3 ? connect_wifi(ssid, password, next) :
                      operation == 2 ? renew_dhcp(next) : collect(operation == 1, next);
            }
            catch (...) { ret = -EFAULT; }
            fprintf(stderr, "[network] request=%s result=%d aps=%d\n",
                    operation == 3 ? "connect" : operation == 2 ? "dhcp" : operation == 1 ? "scan" : "status", ret, next.count);
            std::lock_guard<std::mutex> guard(runtime.lock);
            next.generation = runtime.snapshot.generation + 1;
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
extern "C" int lawrec_network_renew_async(void) { return start_request(2); }
extern "C" int lawrec_network_connect_async(const char *ssid, const char *password) {
    if (!ssid || !password || !*ssid || strnlen(ssid, 33) > 32 ||
        strnlen(password, 64) < 8 || strnlen(password, 64) > 63) return -EINVAL;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(password); *p; ++p)
        if (*p < 32 || *p > 126 || *p == '"' || *p == '\\') return -EINVAL;
    try { return start_request(3, ssid, password); }
    catch (...) { return -ENOMEM; }
}

extern "C" void lawrec_network_get(lawrec_network_snapshot *result) {
    if (!result) return;
    std::lock_guard<std::mutex> guard(runtime.lock);
    *result = runtime.snapshot;
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
