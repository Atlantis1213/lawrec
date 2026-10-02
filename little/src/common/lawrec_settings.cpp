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
#include <sstream>

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

int save_file_at(const std::string &dir, const char *name, const std::string &data)
{
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

int save_file(const char *name, const std::string &data)
{
    return save_file_at(directory(), name, data);
}

bool valid_record_dir(const char *path)
{
    if (!path) return false;
    size_t length = strnlen(path, LAWREC_RECORD_DIR_MAX + 1);
    if (length < 2 || length > LAWREC_RECORD_DIR_MAX || path[0] != '/' || path[length-1] == '/') return false;
    size_t start = 1;
    for (size_t i = 1; i <= length; ++i) {
        if (i == length || path[i] == '/') {
            size_t size = i-start;
            if (!size || (size == 1 && path[start] == '.') ||
                (size == 2 && path[start] == '.' && path[start+1] == '.')) return false;
            start = i+1;
        } else {
            unsigned char c = path[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        }
    }
    return true;
}

int read_record_dir(std::string &path)
{
    path = LAWREC_RECORD_DEFAULT_OUTPUT_DIR;
    std::string config = directory() + "/lawrec-record-dir";
    int fd = open(config.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -errno;
    struct stat st{};
    char bytes[LAWREC_RECORD_DIR_MAX + 3];
    int ret = fstat(fd, &st) ? -errno : 0;
    if (!ret && (!S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size >= (off_t)sizeof(bytes))) ret = -EINVAL;
    ssize_t size = ret ? -1 : read(fd, bytes, sizeof(bytes));
    if (!ret && size != st.st_size) ret = size < 0 ? -errno : -EIO;
    close(fd);
    if (ret) return ret;
    std::string value(bytes, size);
    if (!value.empty() && value.back() == '\n') {
        value.pop_back();
        if (!value.empty() && value.back() == '\r') value.pop_back();
    }
    if (value.find('\0') != std::string::npos || !valid_record_dir(value.c_str())) return -EINVAL;
    path = value;
    return 0;
}

struct RecordDirectory { std::string path; int error; };
const RecordDirectory &current_record_dir()
{
    static const RecordDirectory value = [] {
        std::lock_guard<std::mutex> guard(settings_lock);
        RecordDirectory result{LAWREC_RECORD_DEFAULT_OUTPUT_DIR, 0};
        const char *override_path = getenv("LAWREC_RECORD_DIR");
        if (override_path && *override_path) {
            if (!valid_record_dir(override_path)) result.error = -EINVAL;
            else result.path = override_path;
        } else result.error = read_record_dir(result.path);
        fprintf(stderr, "[settings] record_dir load result=%d path=%s override=%d restart_required=1\n",
                result.error, result.path.c_str(), !!(override_path && *override_path));
        return result;
    }();
    return value;
}

int copy_record_dir(const std::string &source, char *path, size_t capacity, int error)
{
    if (!path || !capacity) return -EINVAL;
    if (source.size() >= capacity) { path[0] = 0; return -ENAMETOOLONG; }
    memcpy(path, source.c_str(), source.size()+1);
    return error;
}

std::string runtime_directory()
{
    const char *p = getenv("LAWREC_NETWORK_RUN_DIR");
    return p && *p ? p : "/var/run";
}

lawrec_ipv4_settings ipv4_defaults() { return {1, 1, 24, {}, {}, {}, {}}; }

bool unicast(const char *text, uint32_t &number)
{
    if (strnlen(text, 16) >= 16) return false;
    in_addr address{};
    if (inet_pton(AF_INET, text, &address) != 1) return false;
    number = ntohl(address.s_addr);
    unsigned first = number >> 24;
    return first != 0 && first != 127 && first < 224;
}

bool valid_ipv4(const lawrec_ipv4_settings &s)
{
    if (s.version != 1 || s.dhcp > 1 || s.prefix < 1 || s.prefix > 30) return false;
    // DHCP has no stale static fields: boot and online recovery see one policy.
    if (s.dhcp) return !s.address[0] && !s.gateway[0] && !s.dns1[0] && !s.dns2[0];
    uint32_t address, gateway, dns;
    if (!unicast(s.address, address) || !unicast(s.dns1, dns) ||
        (s.dns2[0] && !unicast(s.dns2, dns))) return false;
    uint32_t host_mask = (1u << (32-s.prefix))-1;
    if (!(address & host_mask) || (address & host_mask) == host_mask) return false;
    if (s.gateway[0] && (!unicast(s.gateway, gateway) || gateway == address ||
        (gateway & ~host_mask) != (address & ~host_mask) ||
        !(gateway & host_mask) || (gateway & host_mask) == host_mask)) return false;
    return true;
}

std::string ipv4_text(const lawrec_ipv4_settings &s)
{
    char text[256];
    snprintf(text, sizeof(text), "version=%u\ndhcp=%u\nprefix=%u\naddress=%s\ngateway=%s\ndns1=%s\ndns2=%s\n",
             s.version, s.dhcp, s.prefix, s.address, s.gateway, s.dns1, s.dns2);
    return text;
}

int read_ipv4(const std::string &dir, lawrec_ipv4_settings &result,
              const char *name = "lawrec-ipv4.conf", bool required = false)
{
    result = ipv4_defaults();
    std::string path = dir + "/" + name;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT && !required ? 0 : -errno;
    struct stat st{};
    char text[512];
    int ret = fstat(fd, &st) ? -errno : 0;
    if (!ret && (!S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size >= (off_t)sizeof(text))) ret = -EINVAL;
    ssize_t n = ret ? -1 : read(fd, text, sizeof(text));
    if (!ret && n != st.st_size) ret = n < 0 ? -errno : -EIO;
    close(fd);
    if (ret) return ret;
    std::istringstream lines(std::string(text, n));
    std::string line;
    lawrec_ipv4_settings parsed{};
    unsigned seen = 0;
    while (std::getline(lines, line)) {
        size_t equal = line.find('=');
        if (equal == std::string::npos) return -EINVAL;
        std::string key = line.substr(0, equal), value = line.substr(equal+1);
        unsigned bit;
        if (key == "version" || key == "dhcp" || key == "prefix") {
            unsigned number = 0;
            if (value.empty()) return -EINVAL;
            for (unsigned char c : value) {
                if (c < '0' || c > '9' || number > 100) return -EINVAL;
                number = number*10 + c-'0';
            }
            if (key == "version") { parsed.version = number; bit = 1; }
            else if (key == "dhcp") { parsed.dhcp = number; bit = 2; }
            else { parsed.prefix = number; bit = 4; }
        } else {
            char *field;
            if (key == "address") { field = parsed.address; bit = 8; }
            else if (key == "gateway") { field = parsed.gateway; bit = 16; }
            else if (key == "dns1") { field = parsed.dns1; bit = 32; }
            else if (key == "dns2") { field = parsed.dns2; bit = 64; }
            else return -EINVAL;
            if (value.size() >= 16 || value.find('\0') != std::string::npos) return -EINVAL;
            memcpy(field, value.c_str(), value.size()+1);
        }
        if (seen & bit) return -EINVAL;
        seen |= bit;
    }
    if (seen != 127 || !valid_ipv4(parsed)) return -EINVAL;
    result = parsed;
    return 0;
}

lawrec_media_settings defaults() { return {LAWREC_MEDIA_SETTINGS_VERSION, LAWREC_RTSP_DEFAULT_PORT, 4000, 0, 0, 30}; }
bool valid_media(const lawrec_media_settings &s) {
    return s.version == LAWREC_MEDIA_SETTINGS_VERSION && s.audio_enabled <= 1 && s.rtsp_port >= 1024 && s.rtsp_port <= 65535 &&
           (s.video_frame_rate == 15 || s.video_frame_rate == 30) &&
           s.video_bitrate_kbps >= 1000 && s.video_bitrate_kbps <= 8000 &&
           (!s.record_segment_seconds || (s.record_segment_seconds >= 60 && s.record_segment_seconds <= 3600));
}
int read_media(lawrec_media_settings &result) {
    result = defaults();
    const std::string path = directory() + "/lawrec-media.conf";
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) return -errno;
        result.rtsp_port = load_port(); // Import the old one-field configuration.
        return 0;
    }
    struct stat st{};
    int error = 0;
    char text[512] = {};
    if (fstat(fd, &st)) error = -errno;
    else if (!S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size >= (off_t)sizeof(text)) error = -EINVAL;
    ssize_t n = error ? -1 : read(fd, text, sizeof(text)-1);
    if (!error && n != st.st_size) error = n < 0 ? -errno : -EIO;
    close(fd);
    if (error) return error;
    std::istringstream lines(std::string(text, n));
    std::string line;
    lawrec_media_settings parsed{};
    unsigned seen = 0;
    while (std::getline(lines, line)) {
        size_t equal = line.find('=');
        if (equal == std::string::npos || equal+1 == line.size()) return -EINVAL;
        std::string key = line.substr(0, equal), value = line.substr(equal+1);
        unsigned number = 0;
        for (unsigned char c : value) {
            if (c < '0' || c > '9' || number > 65535) return -EINVAL;
            number = number*10 + c-'0';
        }
        unsigned bit;
        if (key == "version") { parsed.version = number; bit = 1; }
        else if (key == "rtsp_port") { parsed.rtsp_port = number; bit = 2; }
        else if (key == "video_bitrate_kbps") { parsed.video_bitrate_kbps = number; bit = 4; }
        else if (key == "record_segment_seconds") { parsed.record_segment_seconds = number; bit = 8; }
        else if (key == "audio_enabled") { parsed.audio_enabled = number; bit = 16; }
        else if (key == "video_frame_rate") { parsed.video_frame_rate = number; bit = 32; }
        else return -EINVAL;
        if (seen & bit) return -EINVAL;
        seen |= bit;
    }
    if (parsed.version == 1) {
        // Preserve existing v1 port/bitrate/audio/segment policies. Merely
        // reading upgrades the snapshot, not the file; Save writes v2 later.
        if ((seen & 7) != 7 || (seen & 32)) return -EINVAL;
        parsed.version = LAWREC_MEDIA_SETTINGS_VERSION;
        parsed.video_frame_rate = 30;
    } else if (parsed.version != LAWREC_MEDIA_SETTINGS_VERSION || seen != 63) return -EINVAL;
    if (!valid_media(parsed)) return -EINVAL;
    result = parsed;
    return 0;
}
const lawrec_media_settings &current_media() {
    static const lawrec_media_settings current = [] {
        lawrec_media_settings value;
        int ret = read_media(value);
        fprintf(stderr, "[settings] media load result=%d version=%u port=%u bitrate_kbps=%u target_fps=%u segment_seconds=%u audio=%u\n",
                ret, value.version, value.rtsp_port, value.video_bitrate_kbps, value.video_frame_rate,
                value.record_segment_seconds, value.audio_enabled);
        return value;
    }();
    return current;
}
int save_media(const lawrec_media_settings &s) {
    char data[192];
    int size = snprintf(data, sizeof(data), "version=%u\nrtsp_port=%u\nvideo_bitrate_kbps=%u\nrecord_segment_seconds=%u\naudio_enabled=%u\nvideo_frame_rate=%u\n",
                        s.version, s.rtsp_port, s.video_bitrate_kbps, s.record_segment_seconds, s.audio_enabled, s.video_frame_rate);
    if (size < 0 || size_t(size) >= sizeof(data)) return -EOVERFLOW;
    int ret = save_file("lawrec-media.conf", data);
    fprintf(stderr, "[settings] media save result=%d version=%u port=%u bitrate_kbps=%u target_fps=%u segment_seconds=%u audio=%u restart_required=1\n",
            ret, s.version, s.rtsp_port, s.video_bitrate_kbps, s.video_frame_rate, s.record_segment_seconds, s.audio_enabled);
    return ret;
}
}

extern "C" int lawrec_settings_port(void)
{
    // Snapshot never changes while an asynchronous media worker is alive.
    return current_media().rtsp_port;
}

extern "C" int lawrec_settings_record_dir_validate(const char *path) {
    return valid_record_dir(path) ? 0 : -EINVAL;
}
extern "C" int lawrec_settings_record_dir_current(char *path, size_t capacity) {
    const auto &value = current_record_dir();
    return copy_record_dir(value.path, path, capacity, value.error);
}
extern "C" int lawrec_settings_record_dir_pending(char *path, size_t capacity) {
    if (!path || !capacity) return -EINVAL;
    std::lock_guard<std::mutex> guard(settings_lock);
    std::string value;
    int ret = read_record_dir(value);
    return copy_record_dir(value, path, capacity, ret);
}
extern "C" int lawrec_settings_record_dir_save(const char *path) {
    if (!valid_record_dir(path)) return -EINVAL;
    (void)current_record_dir();
    std::lock_guard<std::mutex> guard(settings_lock);
    int ret = save_file("lawrec-record-dir", std::string(path)+"\n");
    fprintf(stderr, "[settings] record_dir save result=%d restart_required=1\n", ret);
    return ret;
}

extern "C" int lawrec_settings_bitrate(void) { return current_media().video_bitrate_kbps; }
extern "C" int lawrec_settings_frame_rate(void) { return current_media().video_frame_rate; }
extern "C" int lawrec_settings_segment_seconds(void) { return current_media().record_segment_seconds; }
extern "C" int lawrec_settings_audio_enabled(void) { return current_media().audio_enabled; }
extern "C" void lawrec_settings_media_current(lawrec_media_settings *result) {
    if (result) *result = current_media();
}
extern "C" int lawrec_settings_media_pending(lawrec_media_settings *result) {
    if (!result) return -EINVAL;
    std::lock_guard<std::mutex> guard(settings_lock);
    return read_media(*result);
}
extern "C" int lawrec_settings_media_save(const lawrec_media_settings *s) {
    if (!s || !valid_media(*s)) return -EINVAL;
    (void)current_media();
    std::lock_guard<std::mutex> guard(settings_lock);
    return save_media(*s);
}

extern "C" int lawrec_settings_save_port(unsigned port)
{
    if (port < 1024 || port > 65535) return -EINVAL;
    (void)lawrec_settings_port();
    std::lock_guard<std::mutex> guard(settings_lock);
    lawrec_media_settings pending;
    int ret = read_media(pending);
    if (ret) return ret; // Do not silently discard an unreadable/newer config.
    pending.rtsp_port = port;
    return save_media(pending);
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

extern "C" int lawrec_settings_ipv4_validate(const lawrec_ipv4_settings *s)
{
    return s && valid_ipv4(*s) ? 0 : -EINVAL;
}
extern "C" int lawrec_settings_ipv4_pending(lawrec_ipv4_settings *result)
{
    if (!result) return -EINVAL;
    std::lock_guard<std::mutex> guard(settings_lock);
    return read_ipv4(directory(), *result);
}
extern "C" int lawrec_settings_ipv4_active(lawrec_ipv4_settings *result)
{
    if (!result) return -EINVAL;
    std::lock_guard<std::mutex> guard(settings_lock);
    *result = ipv4_defaults();
    struct stat st{};
    std::string incomplete = runtime_directory() + "/lawrec-ipv4-incomplete";
    if (!lstat(incomplete.c_str(), &st)) return -EIO;
    if (errno != ENOENT) return -errno;
    return read_ipv4(runtime_directory(), *result);
}
extern "C" int lawrec_settings_ipv4_stage_boot(void)
{
    std::lock_guard<std::mutex> guard(settings_lock);
    lawrec_ipv4_settings settings;
    int ret = read_ipv4(directory(), settings);
    if (!ret) ret = save_file_at(runtime_directory(), "lawrec-ipv4-boot.conf", ipv4_text(settings));
    fprintf(stderr, "[network] stage ipv4 result=%d\n", ret);
    return ret;
}
extern "C" int lawrec_settings_ipv4_boot(lawrec_ipv4_settings *result)
{
    if (!result) return -EINVAL;
    std::lock_guard<std::mutex> guard(settings_lock);
    return read_ipv4(runtime_directory(), *result, "lawrec-ipv4-boot.conf", true);
}
extern "C" int lawrec_settings_ipv4_begin_apply(void)
{
    std::lock_guard<std::mutex> guard(settings_lock);
    return save_file_at(runtime_directory(), "lawrec-ipv4-incomplete", "1\n");
}
extern "C" int lawrec_settings_ipv4_save(const lawrec_ipv4_settings *s)
{
    if (!s || !valid_ipv4(*s)) return -EINVAL;
    std::lock_guard<std::mutex> guard(settings_lock);
    int ret = save_file("lawrec-ipv4.conf", ipv4_text(*s));
    fprintf(stderr, "[settings] ipv4 save result=%d dhcp=%u next_boot=1\n", ret, s->dhcp);
    return ret;
}
extern "C" int lawrec_settings_ipv4_note_active(const lawrec_ipv4_settings *s)
{
    if (!s || !valid_ipv4(*s)) return -EINVAL;
    std::lock_guard<std::mutex> guard(settings_lock);
    int ret = save_file_at(runtime_directory(), "lawrec-ipv4.conf", ipv4_text(*s));
    std::string incomplete = runtime_directory() + "/lawrec-ipv4-incomplete";
    if (!ret && unlink(incomplete.c_str()) && errno != ENOENT) ret = -errno;
    return ret;
}
