#include "lawrec_process.h"
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <thread>

extern char **environ;

int lawrec_process_run(const char *path, char *const argv[], unsigned timeout_ms,
                       const std::atomic<bool> &cancel, const char *log_path)
{
    if (!path || path[0] != '/' || !argv || !argv[0] || !timeout_ms || !log_path)
        return -EINVAL;
    if (cancel.load()) return -ECANCELED;
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -errno;
    if (fd < 3) {
        int safe_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        int error = errno;
        close(fd);
        if (safe_fd < 0) return -error;
        fd = safe_fd;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int ret = posix_spawn_file_actions_init(&actions);
    if (ret) { close(fd); return -ret; }
    ret = posix_spawnattr_init(&attr);
    if (ret) { posix_spawn_file_actions_destroy(&actions); close(fd); return -ret; }
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGPIPE);
    if (!(ret = posix_spawnattr_setpgroup(&attr, 0)) &&
        !(ret = posix_spawnattr_setsigmask(&attr, &mask)) &&
        !(ret = posix_spawnattr_setsigdefault(&attr, &defaults)) &&
        !(ret = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF)) &&
        !(ret = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)) &&
        !(ret = posix_spawn_file_actions_adddup2(&actions, fd, 1)))
        ret = posix_spawn_file_actions_adddup2(&actions, fd, 2);
    pid_t pid = -1;
    if (!ret) ret = posix_spawn(&pid, path, &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(fd);
    if (ret) return -ret;
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            fprintf(stderr, "[process] pid=%ld exit_status=%d\n", (long)pid, status);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -ECHILD;
        }
        if (done < 0 && errno != EINTR) return -errno;
        if (cancel.load() || Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ret = cancel.load() ? -ECANCELED : -ETIMEDOUT;
    // Do not signal unrelated clients. pid is unreaped and owns this group.
    kill(-pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    fprintf(stderr, "[process] pid=%ld stopped result=%d\n", (long)pid, ret);
    return ret;
}
