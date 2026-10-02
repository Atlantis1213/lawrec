#include "lawrec_demux.h"
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <dirent.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <chrono>
#include <thread>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>
#include <mutex>

extern char **environ;
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kMagic = 0x4c524d50;
constexpr unsigned kMaxFrame = 4*1024*1024;
enum Operation { Info = 1, Frame = 2 };
std::mutex retired_lock;
pid_t retired = -1;
bool owner_reserved = false;
struct Reply {
    uint32_t magic = kMagic, version = 1, operation = 0;
    int32_t error = 0;
    uint32_t length = 0, codec = 0, eof = 0;
    uint64_t pts = 0;
    k_mp4_file_info_s info{};
    k_mp4_track_info_s tracks[2]{};
};

int transfer(int socket, void *buffer, size_t size, bool writing,
             Clock::time_point deadline, const std::atomic<bool> *cancel)
{
    auto *bytes = static_cast<unsigned char *>(buffer);
    size_t done = 0;
    while (done < size) {
        if (cancel && cancel->load()) return -ECANCELED;
        if (Clock::now() >= deadline) return -ETIMEDOUT;
        pollfd p{socket, short(writing ? POLLOUT : POLLIN), 0};
        int ready = poll(&p, 1, 50);
        if (ready < 0) { if (errno == EINTR) continue; return -errno; }
        if (!ready) continue;
        ssize_t n = writing ? send(socket, bytes+done, size-done, MSG_NOSIGNAL | MSG_DONTWAIT) :
                              recv(socket, bytes+done, size-done, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return n ? -errno : -EPIPE;
        done += n;
    }
    return 0;
}

int child_parse(int file, int socket)
{
    KD_HANDLE handle = nullptr;
    k_mp4_config_s config{};
    config.config_type = K_MP4_CONFIG_DEMUXER;
    snprintf(config.demuxer_config.file_name, sizeof(config.demuxer_config.file_name), "/proc/self/fd/%d", file);
    int error = kd_mp4_create(&handle, &config) ? -EBADMSG : 0;
    for (;;) {
        uint32_t operation = 0;
        // No idle timeout: pause and PTS pacing belong to the parent. A closed
        // private socket wakes this read when the parent exits or stops playback.
        ssize_t n;
        do { n = recv(socket, &operation, sizeof(operation), MSG_WAITALL); } while (n < 0 && errno == EINTR);
        if (n != sizeof(operation)) break;
        Reply reply;
        reply.operation = operation;
        reply.error = error;
        k_mp4_frame_data_s frame{};
        if (!reply.error && operation == Info) {
            if (kd_mp4_get_file_info(handle, &reply.info)) reply.error = -EBADMSG;
            else if (!reply.info.track_num || reply.info.track_num > 2) reply.error = -ENOTSUP;
            for (unsigned i = 0; !reply.error && i < reply.info.track_num; ++i)
                if (kd_mp4_get_track_by_index(handle, i, &reply.tracks[i])) reply.error = -EBADMSG;
        } else if (!reply.error && operation == Frame) {
            if (kd_mp4_get_frame(handle, &frame)) reply.error = -EBADMSG;
            else if (frame.eof) reply.eof = 1;
            else if (!frame.data || !frame.data_length || frame.data_length > kMaxFrame ||
                     frame.codec_id < K_MP4_CODEC_ID_H264 || frame.codec_id >= K_MP4_CODEC_ID_BUTT)
                reply.error = -EBADMSG;
            else { reply.length = frame.data_length; reply.codec = frame.codec_id; reply.pts = frame.time_stamp; }
        } else if (!reply.error) reply.error = -EPROTO;
        auto deadline = Clock::now()+std::chrono::seconds(8);
        if (transfer(socket, &reply, sizeof(reply), true, deadline, nullptr)) break;
        if (reply.length && transfer(socket, frame.data, reply.length, true, deadline, nullptr)) break;
        if (reply.error) break;
    }
    // Destruction itself may assert or hang. The parent still owns the child
    // and enforces its close deadline; no media resource exists in this process.
    int result = handle && kd_mp4_destroy(handle) ? 1 : 0;
    close(file); close(socket);
    return result;
}
}

LawrecDemux::~LawrecDemux() { close(); }

int LawrecDemux::start(int file, k_mp4_file_info_s &info, k_mp4_track_info_s tracks[2])
{
    if (reserved_ || pid_ > 0 || socket_ >= 0 || file < 0) return -EINVAL;
    if (cancel_) return -ECANCELED;
    // Validate before allocating descriptors: a closed input number could be
    // reused by socketpair and otherwise masquerade as a live input descriptor.
    int flags = fcntl(file, F_GETFL);
    if (flags < 0) return -errno;
    if ((flags & O_ACCMODE) != O_RDONLY) return -EINVAL;
    struct stat st{};
    if (fstat(file, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) return -EBADMSG;
    {
        std::lock_guard<std::mutex> guard(retired_lock);
        if (owner_reserved) return -EBUSY;
        if (retired > 0) {
            int status;
            pid_t done = waitpid(retired, &status, WNOHANG);
            if (done == 0 || (done < 0 && errno == EINTR)) return -EBUSY;
            retired = -1;
        }
        // Playback has one parser owner. Enforce this here too so a retired
        // kernel-I/O child cannot be overwritten by a concurrent client.
        owner_reserved = reserved_ = true;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
        int error = errno; close(); return -error;
    }
    // Pin sources above the child targets so adddup2 cannot clobber another
    // source when the caller's descriptors happen to be 3 or 4.
    int source_file = fcntl(file, F_DUPFD_CLOEXEC, 10);
    int error = source_file < 0 ? errno : 0;
    int source_socket = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    if (source_socket < 0 && !error) error = errno;
    if (source_file < 0 || source_socket < 0) {
        if (source_file >= 0) ::close(source_file);
        if (source_socket >= 0) ::close(source_socket);
        ::close(sockets[0]); ::close(sockets[1]);
        close();
        return -error;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int ret = posix_spawn_file_actions_init(&actions);
    bool actions_ready = !ret, attr_ready = false;
    if (!ret) { ret = posix_spawnattr_init(&attr); attr_ready = !ret; }
    sigset_t mask, defaults;
    sigemptyset(&mask); sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGPIPE);
    if (!ret) ret = posix_spawnattr_setsigmask(&attr, &mask);
    if (!ret) ret = posix_spawnattr_setsigdefault(&attr, &defaults);
    if (!ret) ret = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!ret) ret = posix_spawn_file_actions_adddup2(&actions, source_file, 3);
    if (!ret) ret = posix_spawn_file_actions_adddup2(&actions, source_socket, 4);
    if (!ret) ret = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    char executable[] = "/proc/self/exe", flag[] = "--mp4-demux";
    char owner[32];
    snprintf(owner, sizeof(owner), "%ld", (long)getpid());
    char *args[] = {executable, flag, owner, nullptr};
    if (!ret) ret = posix_spawn(&pid_, executable, &actions, &attr, args, environ);
    if (attr_ready) posix_spawnattr_destroy(&attr);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    ::close(source_file); ::close(source_socket); ::close(sockets[1]);
    if (ret) { ::close(sockets[0]); pid_ = -1; close(); return -ret; }
    socket_ = sockets[0];
    Reply reply;
    ret = exchange(Info, &reply);
    if (!ret) { info = reply.info; memcpy(tracks, reply.tracks, sizeof(reply.tracks)); }
    fprintf(stderr, "[demux] start pid=%ld result=%d tracks=%u\n", (long)pid_, ret, ret ? 0 : info.track_num);
    return ret;
}

int LawrecDemux::exchange(unsigned operation, void *response)
{
    if (socket_ < 0) return -EBADF;
    auto deadline = Clock::now()+std::chrono::seconds(8);
    uint32_t command = operation;
    int ret = transfer(socket_, &command, sizeof(command), true, deadline, &cancel_);
    auto &reply = *static_cast<Reply *>(response);
    if (!ret) ret = transfer(socket_, &reply, sizeof(reply), false, deadline, &cancel_);
    if (!ret && (reply.magic != kMagic || reply.version != 1 || reply.operation != operation ||
        reply.error > 0 || reply.error < -4095 || reply.eof > 1 || reply.length > kMaxFrame ||
        (reply.error && reply.length))) ret = -EPROTO;
    if (!ret) ret = reply.error;
    if (!ret && operation == Info && (reply.length || reply.eof || !reply.info.track_num || reply.info.track_num > 2)) ret = -EPROTO;
    if (!ret && operation == Frame && ((!reply.eof && !reply.length) || (reply.eof && reply.length) ||
        reply.codec >= K_MP4_CODEC_ID_BUTT)) ret = -EPROTO;
    if (!ret && reply.length) {
        try { bytes_.resize(reply.length); }
        catch (...) { ret = -ENOMEM; }
        if (!ret) ret = transfer(socket_, bytes_.data(), bytes_.size(), false, deadline, &cancel_);
    }
    if (ret == -EPIPE || ret == -ECONNRESET) ret = -EBADMSG;
    if (ret) fprintf(stderr, "[demux] operation=%u pid=%ld result=%d\n", operation, (long)pid_, ret);
    return ret;
}

int LawrecDemux::next(k_mp4_frame_data_s &frame)
{
    Reply reply;
    int ret = exchange(Frame, &reply);
    if (!ret) {
        frame = {};
        frame.codec_id = static_cast<k_mp4_codec_id_e>(reply.codec);
        frame.eof = reply.eof;
        frame.time_stamp = reply.pts;
        frame.data_length = reply.length;
        frame.data = reply.length ? bytes_.data() : nullptr;
    }
    return ret;
}

int LawrecDemux::close()
{
    if (socket_ >= 0) { ::close(socket_); socket_ = -1; }
    struct Release {
        bool &reserved;
        ~Release() {
            if (reserved) {
                std::lock_guard<std::mutex> guard(retired_lock);
                owner_reserved = reserved = false;
            }
        }
    } release{reserved_};
    if (pid_ <= 0) return 0;
    auto deadline = Clock::now()+std::chrono::milliseconds(300);
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_ || (result < 0 && errno != EINTR)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (Clock::now() < deadline);
    bool forced = result == 0 || (result < 0 && errno == EINTR);
    if (forced) {
        // The child is unreaped, so its PID cannot be reused. It owns only
        // parser memory and read-only input, never VB/decoder/display resources.
        kill(pid_, SIGKILL);
        deadline = Clock::now()+std::chrono::milliseconds(300);
        do {
            result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno != EINTR)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (Clock::now() < deadline);
        if (result == 0 || (result < 0 && errno == EINTR)) {
            // A child stuck in kernel I/O may survive SIGKILL temporarily.
            // Keep at most one retired owner and refuse another parser until
            // it is reaped, rather than blocking UI shutdown or losing its PID.
            std::lock_guard<std::mutex> guard(retired_lock);
            retired = pid_;
        }
    }
    int ret = result == pid_ ? (forced ? -ETIMEDOUT :
              WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EBADMSG) : forced ? -ETIMEDOUT : -ECHILD;
    fprintf(stderr, "[demux] close pid=%ld exit_status=%d forced=%d result=%d\n", (long)pid_, status, forced, ret);
    pid_ = -1;
    return ret;
}

extern "C" int lawrec_demux_helper(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--mp4-demux")) return 2;
    char *end;
    errno = 0;
    long owner = strtol(argv[2], &end, 10);
    if (errno || !*argv[2] || *end || owner <= 1 || owner > INT_MAX) return 2;
    struct stat st{};
    int type; socklen_t size = sizeof(type);
    int flags = fcntl(3, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(3, &st) ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 || getsockopt(4, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_STREAM)
        return 2;
    // Check the original owner after installing PDEATHSIG; reparenting to a
    // subreaper is not necessarily PID 1 and must not bypass the death race.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner) return 2;
    // Do not retain parent-owned MAPI/socket/DRM descriptors across parsing.
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return 2;
    while (dirent *entry = readdir(directory)) {
        char *end; long fd = strtol(entry->d_name, &end, 10);
        if (!*end && fd > 4 && fd != dirfd(directory)) ::close(fd);
    }
    closedir(directory);
    rlimit core{0, 0}, memory{128ULL*1024*1024, 128ULL*1024*1024};
    if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_AS, &memory) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 2;
    try { return child_parse(3, 4); }
    catch (...) { return 1; }
}
