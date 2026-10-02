#include "lawrec_process.h"
#include <cassert>
#include <cerrno>
#include <thread>
#include <chrono>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

int main() {
    char log[] = "/tmp/lawrec-process-test-XXXXXX";
    int fd = mkstemp(log); assert(fd >= 0); close(fd);
    std::atomic<bool> cancel{false};
    char yes[] = "/bin/true", no[] = "/bin/false", sleep[] = "/bin/sleep", ten[] = "10";
    char *ok[] = {yes, nullptr}, *fail[] = {no, nullptr}, *slow[] = {sleep, ten, nullptr};
    assert(lawrec_process_run(yes, ok, 1000, cancel, log) == 0);
    assert(lawrec_process_run(no, fail, 1000, cancel, log) == -ECHILD);
    assert(lawrec_process_run("/no-such-lawrec-program", ok, 1000, cancel, log) == -ENOENT);
    assert(lawrec_process_run(sleep, slow, 50, cancel, log) == -ETIMEDOUT);
    assert(lawrec_process_run(sleep, slow, 50, cancel, log, false) == -ETIMEDOUT);
    std::string unsafe = std::string(log)+".unsafe";
    assert(symlink(log, unsafe.c_str()) == 0);
    assert(lawrec_process_run(yes, ok, 1000, cancel, unsafe.c_str()) == -ELOOP);
    unlink(unsafe.c_str());
    assert(mkfifo(unsafe.c_str(), 0600) == 0);
    assert(lawrec_process_run(yes, ok, 1000, cancel, unsafe.c_str()) == -ENXIO);
    unlink(unsafe.c_str());
    std::thread aborter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); cancel = true;
    });
    assert(lawrec_process_run(sleep, slow, 5000, cancel, log) == -ECANCELED);
    aborter.join();
    assert(lawrec_process_run(yes, ok, 1000, cancel, log) == -ECANCELED);
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    unlink(log);
    puts("process: success, failure, spawn failure, timeout, cancellation, reap passed");
}
