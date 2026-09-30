#include "lawrec_storage.h"
#include "lawrec_config.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" int lawrec_storage_open_recording(const char *name)
{
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\')) return -EINVAL;
    size_t length = strlen(name);
    if (length < 5 || length >= 128 || strcmp(name + length - 4, ".mp4")) return -EINVAL;
    int directory = open(lawrec_storage_dir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -errno;
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
    static const std::string dir = [] {
        const char *override_dir = getenv("LAWREC_RECORD_DIR");
        if (override_dir && override_dir[0] == '/') return std::string(override_dir);
        char path[256] = {};
        FILE *fp = fopen("/etc/lawrec-record-dir", "r");
        if (fp) {
            if (fgets(path, sizeof(path), fp)) path[strcspn(path, "\r\n")] = 0;
            fclose(fp);
        }
        return std::string(path[0] == '/' ? path : LAWREC_RECORD_DEFAULT_OUTPUT_DIR);
    }();
    return dir.c_str();
}

extern "C" int lawrec_storage_check(uint64_t *available)
{
    struct statfs fs;
    struct statvfs space;
    if (statfs(lawrec_storage_dir(), &fs) || statvfs(lawrec_storage_dir(), &space))
        return -errno;
    // Reject tmpfs/ramfs: a successful recording must survive a normal reboot.
    if (fs.f_type == 0x01021994 || fs.f_type == 0x858458f6) return -ENODEV;
    if (space.f_flag & ST_RDONLY) return -EROFS;
    uint64_t bytes = uint64_t(space.f_bavail) * space.f_frsize;
    if (available) *available = bytes;
    return bytes < 128ULL * 1024 * 1024 ? -ENOSPC : 0;
}

extern "C" int lawrec_storage_reserve(char *path, size_t capacity)
{
    if (!path || !capacity) return -EINVAL;
    int ret = lawrec_storage_check(nullptr);
    if (ret) return ret;
    char date[32];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(date, sizeof(date), "%Y%m%d_%H%M%S", &tm);
    int n = snprintf(path, capacity, "%s/lawrec_%s_XXXXXX.part", lawrec_storage_dir(), date);
    if (n < 0 || size_t(n) >= capacity) return -ENAMETOOLONG;
    int fd = mkstemps(path, 5);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
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
    int directory = open(lawrec_storage_dir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -errno;
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
    size_t len = strlen(name);
    return !strchr(name, '/') && strncmp(name, "lawrec_", 7) == 0 &&
           len > 4 && strcmp(name + len - 4, ".mp4") == 0;
}

extern "C" int lawrec_storage_list(char *text, size_t capacity)
{
    if (!text || !capacity) return -EINVAL;
    text[0] = 0;
    DIR *dir = opendir(lawrec_storage_dir());
    if (!dir) return -errno;
    std::vector<std::string> names;
    while (dirent *e = readdir(dir)) {
        struct stat st;
        if (finished(e->d_name) && !fstatat(dirfd(dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode))
            names.emplace_back(e->d_name);
        if (names.size() >= 1000) break;
    }
    closedir(dir);
    std::sort(names.rbegin(), names.rend());
    size_t used = 0;
    for (const auto &name : names) {
        if (used + name.size() + 2 > capacity) break;
        memcpy(text + used, name.c_str(), name.size());
        used += name.size();
        text[used++] = '\n'; text[used] = 0;
    }
    return 0;
}

extern "C" int lawrec_storage_delete(const char *name)
{
    if (!name || !finished(name)) return -EINVAL;
    int fd = open(lawrec_storage_dir(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -errno;
    struct stat st;
    int ret = fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) ? -errno : 0;
    if (!ret && !S_ISREG(st.st_mode)) ret = -EINVAL;
    if (!ret && unlinkat(fd, name, 0)) ret = -errno;
    if (!ret && fsync(fd)) ret = -errno;
    close(fd);
    return ret;
}
