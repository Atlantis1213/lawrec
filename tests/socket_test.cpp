#include "socket.h"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    char path[80]; std::snprintf(path, sizeof(path), "/tmp/demo-test-%d.sock", getpid());
    pid_t child = fork(); assert(child >= 0);
    if (!child) { execl(argv[1], argv[1], "--mock", path, nullptr); _exit(127); }
    demo::Request request; demo::Status status;
    int result = -1;
    for (int i = 0; i < 30; ++i) {
        result = demo::exchange(path, request, status, 200);
        if (!result) break;
        usleep(10000);
    }
    assert(!result && !status.flags);
    for (unsigned command = 1; command <= 4; ++command) {
        request.id++; request.command = command; request.value = 1;
        assert(!demo::exchange(path, request, status));
        assert(!status.result && (status.flags & (1U << (command - 1))));
        assert(!demo::exchange(path, request, status));
    }
    request.id++; request.command = 3; request.value = 0;
    assert(!demo::exchange(path, request, status));
    assert(status.flags == (demo::Preview | demo::Ai | demo::Record));
    assert(kill(child, SIGTERM) == 0);
    int exit_status; assert(waitpid(child, &exit_status, 0) == child);
    assert(WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0);
    assert(demo::exchange(path, request, status, 100) == -ENOENT);
    char lock[96]; std::snprintf(lock, sizeof(lock), "%s.lock", path); unlink(lock);
    std::puts("socket MOCK ONLY: four commands, idempotence, independent stop, clean exit passed");
}
