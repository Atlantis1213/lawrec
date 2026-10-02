#include "lawrec_storage.h"
#include "lawrec_config.h"
#include "lawrec_settings.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <unistd.h>

namespace {
struct Location {
    char path[LAWREC_RECORD_DIR_MAX+1]{};
    int error;
    Location() : error(lawrec_settings_record_dir_current(path, sizeof(path))) {}
};
const Location &location() { static const Location value; return value; }

// Reject symlinks in every component, not just the last directory name.
int open_directory(const char *path)
{
    int ret = lawrec_settings_record_dir_validate(path);
    if (ret) return ret;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    std::string value(path);
    size_t begin = 1;
    while (begin < value.size()) {
        size_t end = value.find('/', begin);
        std::string part = value.substr(begin, end-begin);
        int next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        close(fd);
        if (next < 0) return -error;
        fd = next;
        if (end == std::string::npos) break;
        begin = end+1;
    }
    return fd;
}

int open_current_directory()
{
    const auto &value = location();
    return value.error ? value.error : open_directory(value.path);
}

int check_space(int fd, uint64_t *available)
{
    struct statfs fs;
    struct statvfs space;
    if (fstatfs(fd, &fs) || fstatvfs(fd, &space)) return -errno;
    if (fs.f_type == 0x01021994 || fs.f_type == 0x858458f6) return -ENODEV;
    if (space.f_flag & ST_RDONLY) return -EROFS;
    uint64_t bytes = uint64_t(space.f_bavail) * space.f_frsize;
    if (available) *available = bytes;
    return bytes < 128ULL * 1024 * 1024 ? -ENOSPC : 0;
}

int suffix(char text[17])
{
    unsigned char bytes[8];
    size_t done = 0;
    while (done < sizeof(bytes)) {
        ssize_t n = getrandom(bytes+done, sizeof(bytes)-done, GRND_NONBLOCK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        done += n;
    }
    const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        text[i*2] = digits[bytes[i] >> 4]; text[i*2+1] = digits[bytes[i] & 15];
    }
    text[16] = 0;
    return 0;
}
}

extern "C" int lawrec_storage_open_recording(const char *name)
{
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\')) return -EINVAL;
    size_t length = strlen(name);
    if (length < 5 || length >= 128 || strcmp(name + length - 4, ".mp4")) return -EINVAL;
    int directory = open_current_directory();
    if (directory < 0) return directory;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int error = errno;
    close(directory);
    if (fd < 0) return -error;
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size == 0) {
        close(fd); return -EINVAL;
    }
    return fd;
}

extern "C" const char *lawrec_storage_dir(void)
{
    return location().path;
}

extern "C" int lawrec_storage_check(uint64_t *available)
{
    if (available) *available = 0;
    int fd = open_current_directory();
    if (fd < 0) return fd;
    int ret = check_space(fd, available);
    close(fd);
    return ret;
}

extern "C" int lawrec_storage_validate_dir(const char *path, uint64_t *available)
{
    if (available) *available = 0;
    int directory = open_directory(path);
    if (directory < 0) return directory;
    int ret = check_space(directory, available);
    char random[17], name[48];
    if (!ret) ret = suffix(random);
    if (!ret) {
        snprintf(name, sizeof(name), ".lawrec-probe-%s", random);
        int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) ret = -errno;
        else {
            ssize_t bytes;
            do { bytes = write(fd, "probe", 5); } while (bytes < 0 && errno == EINTR);
            if (bytes != 5) ret = bytes < 0 ? -errno : -EIO;
            if (!ret && fsync(fd)) ret = -errno;
            if (close(fd) && !ret) ret = -errno;
            if (unlinkat(directory, name, 0) && !ret) ret = -errno;
            if (fsync(directory) && !ret) ret = -errno;
        }
    }
    close(directory);
    fprintf(stderr, "[storage] directory probe result=%d path=%s\n", ret, path ? path : "(null)");
    return ret;
}

extern "C" int lawrec_storage_reserve(char *path, size_t capacity)
{
    if (!path || !capacity) return -EINVAL;
    int directory = open_current_directory();
    if (directory < 0) return directory;
    int ret = check_space(directory, nullptr);
    if (ret) { close(directory); return ret; }
    char date[32];
    time_t now = time(nullptr);
    struct tm tm;
    if (!localtime_r(&now, &tm) || !strftime(date, sizeof(date), "%Y%m%d_%H%M%S", &tm)) {
        close(directory); return -ERANGE;
    }
    char random[17], name[64];
    ret = suffix(random);
    if (!ret) {
        snprintf(name, sizeof(name), "lawrec_%s_%s.part", date, random);
        int n = snprintf(path, capacity, "%s/%s", lawrec_storage_dir(), name);
        if (n < 0 || size_t(n) >= capacity || n >= 128) ret = -ENAMETOOLONG;
        else {
            int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0) ret = -errno;
            else if (close(fd)) ret = -errno;
        }
    }
    close(directory);
    return ret;
}

extern "C" int lawrec_storage_publish(const char *partial, char *final_path, size_t capacity)
{
    if (!partial || !final_path || !capacity) return -EINVAL;
    std::string path(partial);
    const std::string prefix = std::string(lawrec_storage_dir()) + "/";
    if (path.compare(0, prefix.size(), prefix) || path.size() <= prefix.size() + 5 ||
        path.find('/', prefix.size()) != std::string::npos ||
        path.substr(path.size()-5) != ".part") return -EINVAL;
    std::string final = path.substr(0, path.size()-5) + ".mp4";
    if (final.size() >= capacity) return -ENAMETOOLONG;
    int directory = open_current_directory();
    if (directory < 0) return directory;
    int fd = openat(directory, path.c_str() + prefix.size(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { int ret = -errno; close(directory); return ret; }
    struct stat st{};
    int ret = fstat(fd, &st) ? -errno : 0;
    if (!ret && (!S_ISREG(st.st_mode) || st.st_size <= 0)) ret = -EINVAL;
    if (!ret && fsync(fd)) ret = -errno;
    close(fd);
    // Never overwrite a completed recording. Unsupported filesystems fail
    // closed and retain the .part file rather than falling back to rename().
    if (!ret && syscall(SYS_renameat2, directory, path.c_str() + prefix.size(),
                        directory, final.c_str() + prefix.size(), 1 /* RENAME_NOREPLACE */)) ret = -errno;
    if (!ret) {
        snprintf(final_path, capacity, "%s", final.c_str());
        ret = fsync(directory) ? -errno : 0;
    }
    close(directory);
    return ret;
}

static bool finished(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 12 || len >= 128 || strncmp(name, "lawrec_", 7) || strcmp(name + len - 4, ".mp4")) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
    return true;
}

extern "C" int lawrec_storage_page(const char *anchor, int newer, lawrec_recording_entry *entries,
                                    size_t capacity, size_t *count, int *more)
{
    if (!entries || !count || !more || !capacity || capacity > 32 || (newer != 0 && newer != 1) ||
        (anchor && *anchor && !finished(anchor)) || (newer && (!anchor || !*anchor))) return -EINVAL;
    *count = 0; *more = 0;
    int directory = open_current_directory();
    if (directory < 0) return directory;
    DIR *dir = fdopendir(directory);
    if (!dir) { const int ret = -errno; close(directory); return ret; }
    size_t used = 0;
    int ret = 0;
    for (;;) {
        errno = 0;
        dirent *file = readdir(dir);
        if (!file) { if (errno) ret = -errno; break; }
        if (!finished(file->d_name)) continue;
        const int comparison = anchor && *anchor ? strcmp(file->d_name, anchor) : -1;
        if (anchor && *anchor && (newer ? comparison <= 0 : comparison >= 0)) continue;
        struct stat st{};
        if (fstatat(dirfd(dir), file->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) continue; // Concurrently removed entries are not page failures.
            ret = -errno; break;
        }
        if (!S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        size_t position = 0;
        // Keep only the closest capacity candidates, independent of directory size/order.
        while (position < used && (newer ? strcmp(entries[position].name, file->d_name) < 0 :
                                          strcmp(entries[position].name, file->d_name) > 0)) ++position;
        if (used == capacity) *more = 1;
        if (position >= capacity) continue;
        if (used < capacity) ++used;
        for (size_t i = used - 1; i > position; --i) entries[i] = entries[i - 1];
        auto &entry = entries[position];
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.name, file->d_name, strlen(file->d_name) + 1);
        entry.bytes = st.st_size; entry.device = st.st_dev; entry.inode = st.st_ino;
        entry.modified_seconds = st.st_mtim.tv_sec; entry.modified_nanoseconds = st.st_mtim.tv_nsec;
    }
    if (closedir(dir) && !ret) ret = -errno;
    if (ret) {
        *more = 0;
        fprintf(stderr, "[storage] page result=%d anchor=%s newer=%d\n", ret, anchor ? anchor : "(newest)", newer);
        return ret;
    }
    if (newer) std::reverse(entries, entries + used);
    *count = used;
    return 0;
}

extern "C" int lawrec_storage_list(char *text, size_t capacity)
{
    if (!text || !capacity) return -EINVAL;
    text[0] = 0;
    lawrec_recording_entry page[32];
    size_t count = 0; int more = 0;
    int ret = lawrec_storage_page(nullptr, 0, page, 32, &count, &more);
    if (ret) return ret;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t length = strlen(page[i].name);
        if (used + length + 2 > capacity) return -ENOSPC;
        memcpy(text + used, page[i].name, length);
        used += length;
        text[used++] = '\n'; text[used] = 0;
    }
    return more ? -ENOSPC : 0;
}

static int delete_recording(const char *name, const lawrec_recording_entry *expected)
{
    if (!name || !finished(name)) return -EINVAL;
    int fd = open_current_directory();
    if (fd < 0) return fd;
    struct stat st;
    int ret = fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) ? -errno : 0;
    if (!ret && !S_ISREG(st.st_mode)) ret = -EINVAL;
    if (!ret && expected && (expected->device != (uint64_t)st.st_dev || expected->inode != (uint64_t)st.st_ino ||
        expected->bytes != (uint64_t)st.st_size || expected->modified_seconds != st.st_mtim.tv_sec ||
        expected->modified_nanoseconds != st.st_mtim.tv_nsec)) ret = -ESTALE;
    if (!ret && unlinkat(fd, name, 0)) ret = -errno;
    if (!ret && fsync(fd)) ret = -errno;
    close(fd);
    fprintf(stderr, "[storage] delete result=%d name=%s identity_checked=%d\n", ret, name, expected != nullptr);
    return ret;
}

extern "C" int lawrec_storage_delete(const char *name) { return delete_recording(name, nullptr); }
extern "C" int lawrec_storage_delete_matching(const lawrec_recording_entry *expected)
{
    if (!expected || !memchr(expected->name, 0, sizeof(expected->name))) return -EINVAL;
    return delete_recording(expected->name, expected);
}
