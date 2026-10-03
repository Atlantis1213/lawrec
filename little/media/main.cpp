#include "config.h"
#include "protocol.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
}
int main(int argc, char **argv) {
    bool mock = argc == 3 && std::strcmp(argv[1], "--mock") == 0;
    if (argc != 1 && !mock) {
        std::fprintf(stderr, "Usage: media_service [--mock SOCKET]\n");
        return 2;
    }
    const char *path = mock ? argv[2] : demo::socket_path;
    sockaddr_un address{};
    if (std::strlen(path) >= sizeof(address.sun_path)) return 2;
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);
    char lock_path[128];
    std::snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        std::fprintf(stderr, "[media] singleton lock failed errno=%d\n", errno);
        if (lock >= 0) close(lock);
        return 1;
    }
    struct stat old{};
    if (lstat(path, &old) == 0 && (!S_ISSOCK(old.st_mode) || unlink(path) < 0)) {
        std::fprintf(stderr, "[media] refusing non-socket/stale-path failure\n");
        close(lock);
        return 1;
    }
    int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server < 0 || bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(server, 4) < 0) {
        std::fprintf(stderr, "[media] socket setup failed errno=%d\n", errno);
        if (server >= 0) close(server);
        close(lock);
        return 1;
    }
    chmod(path, 0600);
    signal(SIGINT, stop); signal(SIGTERM, stop);
    demo::Status status;
    std::fprintf(stderr, "[media] mode=%s socket=%s\n", mock ? "MOCK, no hardware" : "skeleton", path);
    while (running) {
        pollfd listener{server, POLLIN, 0};
        if (poll(&listener, 1, 100) <= 0) continue;
        int client = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) continue;
        pollfd incoming{client, POLLIN, 0};
        if (poll(&incoming, 1, 500) > 0 && (incoming.revents & POLLIN)) {
            demo::Request request;
            ssize_t bytes = recv(client, &request, sizeof(request), MSG_TRUNC);
            if (bytes >= 0 && demo::decode_request(&request, static_cast<size_t>(bytes), request)) {
                status.id = request.id;
                status.result = mock ? 0 : -ENOSYS;
                if (mock && request.command) {
                    uint32_t flag = 1U << (request.command - 1);
                    status.flags = request.value ? status.flags | flag : status.flags & ~flag;
                }
                status.last_error = status.result;
                ssize_t sent = send(client, &status, sizeof(status), MSG_NOSIGNAL);
                if (sent != sizeof(status)) std::fprintf(stderr, "[media] reply failed errno=%d\n", errno);
            }
        }
        close(client);
    }
    close(server); unlink(path); close(lock);
    return 0;
}
