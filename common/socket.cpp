#include "socket.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace demo {
int exchange(const char *path, const Request &request, Status &status, int timeout_ms) {
    if (!path || timeout_ms <= 0) return -EINVAL;
    sockaddr_un address{};
    if (std::strlen(path) >= sizeof(address.sun_path)) return -ENAMETOOLONG;
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    // One deadline covers connect, send and receive, not one timeout per stage.
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto wait = [&](short event) {
        for (;;) {
            int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                end - std::chrono::steady_clock::now()).count());
            if (remaining <= 0) return -ETIMEDOUT;
            pollfd p{fd, event, 0};
            int n = poll(&p, 1, remaining);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return n == 0 ? -ETIMEDOUT : -errno;
            if (p.revents & event) return 0;
            return -ECONNRESET;
        }
    };
    int ret = 0;
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) ret = -errno;
        else {
            ret = wait(POLLOUT);
            int error = 0;
            socklen_t size = sizeof(error);
            if (!ret && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) ret = -errno;
            if (!ret && error) ret = -error;
        }
    }
    if (!ret) ret = wait(POLLOUT);
    if (!ret) {
        ssize_t sent = send(fd, &request, sizeof(request), MSG_NOSIGNAL);
        if (sent != sizeof(request)) ret = sent < 0 ? -errno : -EIO;
    }
    if (!ret) ret = wait(POLLIN);
    if (!ret) {
        Status reply;
        ssize_t bytes = recv(fd, &reply, sizeof(reply), MSG_TRUNC);
        if (bytes < 0) ret = -errno;
        else if (bytes != sizeof(reply) || !valid_status(reply, request.id)) ret = -EPROTO;
        else status = reply;
    }
    close(fd);
    return ret;
}
}
