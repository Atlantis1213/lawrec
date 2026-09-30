#include "lawrec_settings.h"
#include "lawrec_config.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {
std::mutex settings_lock;
// Test override is a directory, not a shell argument. Production uses /etc.
std::string directory()
{
    const char *p = getenv("LAWREC_SETTINGS_DIR");
    return p && *p ? p : "/etc";
}
int load_port()
{
    std::string path = directory() + "/lawrec-rtsp-port";
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT)
            fprintf(stderr, "[settings] read failed errno=%d; using default\n", errno);
        return LAWREC_RTSP_DEFAULT_PORT;
    }
    char text[32] = {};
    struct stat st{};
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode)) n = read(fd, text, sizeof(text)-1);
    close(fd);
    char *end = nullptr;
    errno = 0;
    long value = strtol(text, &end, 10);
    bool valid = n > 0 && n < (ssize_t)sizeof(text)-1 && !errno && end != text;
    while (end && (*end == '\n' || *end == '\r' || *end == ' ')) ++end;
    if (!valid || !end || *end || value < 1024 || value > 65535) {
        fprintf(stderr, "[settings] invalid port file; using default\n");
        return LAWREC_RTSP_DEFAULT_PORT;
    }
    return (int)value;
}

int save_file(const char *name, const std::string &data)
{
    std::string dir = directory();
    std::string path = dir + "/" + name;
    std::string temp = path + ".XXXXXX";
    // mkstemp creates mode 0600: WiFi credentials must not be world-readable.
    int fd = mkstemp(&temp[0]);
    if (fd < 0) return -errno;
    int ret = 0;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ret = n < 0 ? -errno : -EIO; break; }
        done += n;
    }
    if (!ret && fsync(fd)) ret = -errno;
    if (close(fd) && !ret) ret = -errno;
    if (!ret && rename(temp.c_str(), path.c_str())) ret = -errno;
    if (!ret) {
        int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0) ret = -errno;
        else { if (fsync(dfd)) ret = -errno; close(dfd); }
    }
    unlink(temp.c_str());
    return ret;
}
}

extern "C" int lawrec_settings_port(void)
{
    // Snapshot never changes while an asynchronous media worker is alive.
    static const int port = load_port();
    return port;
}

extern "C" int lawrec_settings_save_port(unsigned port)
{
    if (port < 1024 || port > 65535) return -EINVAL;
    (void)lawrec_settings_port();
    std::lock_guard<std::mutex> guard(settings_lock);
    char text[32];
    snprintf(text, sizeof(text), "%u\n", port);
    int ret = save_file("lawrec-rtsp-port", text);
    fprintf(stderr, "[settings] save port=%u result=%d restart_required=1\n", port, ret);
    return ret;
}

extern "C" int lawrec_settings_save_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password) return -EINVAL;
    size_t sn = strnlen(ssid, 33), pn = strnlen(password, 64);
    if (!sn || sn > 32 || pn < 8 || pn > 63) return -EINVAL;
    std::string hex, escaped;
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sn; ++i) {
        unsigned char c = ssid[i];
        hex += digits[c >> 4]; hex += digits[c & 15];
    }
    for (size_t i = 0; i < pn; ++i) {
        unsigned char c = password[i];
        // supplicant's quoted-string grammar is not C string escaping.
        if (c < 32 || c > 126 || c == '"' || c == '\\') return -EINVAL;
        escaped += c;
    }
    // SSID hex encoding and a validated PSK prevent config-line injection.
    std::string data = "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=0\nnetwork={\n"
                       "    ssid=" + hex + "\n    scan_ssid=1\n    key_mgmt=WPA-PSK\n"
                       "    proto=RSN\n    psk=\"" + escaped + "\"\n}\n";
    std::lock_guard<std::mutex> guard(settings_lock);
    int ret = save_file("wpa_supplicant.conf", data);
    // Never log SSID/password or the serialized configuration.
    fprintf(stderr, "[settings] save wifi result=%d next_boot=1\n", ret);
    return ret;
}

extern "C" int lawrec_network_addresses(char *buffer, size_t size)
{
    if (!buffer || !size) return -EINVAL;
    buffer[0] = 0;
    struct ifaddrs *list = nullptr;
    if (getifaddrs(&list)) return -errno;
    size_t used = 0;
    int ret = 0;
    for (auto *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET ||
            (p->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN];
        auto *addr = reinterpret_cast<sockaddr_in *>(p->ifa_addr);
        if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) continue;
        int n = snprintf(buffer + used, size - used, "%s %s\n%s\n",
                         p->ifa_name, (p->ifa_flags & IFF_UP) ? "UP" : "DOWN", ip);
        if (n < 0 || (size_t)n >= size - used) { ret = -ENOSPC; break; }
        used += n;
    }
    freeifaddrs(list);
    if (!used && !ret) snprintf(buffer, size, "No IPv4 address");
    return ret;
}
