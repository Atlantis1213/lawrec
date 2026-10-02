#include "lawrec_dhcp.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <thread>
#include <string>
#include <time.h>

static std::atomic<int64_t> boottime_offset_ms{0};
static std::atomic<bool> clock_failed{false};
extern "C" int __real_clock_gettime(clockid_t, timespec *);
extern "C" int __wrap_clock_gettime(clockid_t id, timespec *value) {
    if (id == CLOCK_BOOTTIME && clock_failed) { errno = EIO; return -1; }
    int ret = __real_clock_gettime(id, value);
    if (!ret && id == CLOCK_BOOTTIME) {
        const int64_t ms = boottime_offset_ms.load();
        value->tv_sec += ms/1000;
        value->tv_nsec += (ms%1000)*1000000;
        if (value->tv_nsec >= 1000000000) { ++value->tv_sec; value->tv_nsec -= 1000000000; }
    }
    return ret;
}

static volatile sig_atomic_t stopped;
static void stop_signal(int) { stopped = 1; }
static void write_text(const std::string &path, const std::string &text) {
    std::string temp = path+".new";
    FILE *fp = fopen(temp.c_str(), "w"); assert(fp);
    assert(fwrite(text.data(), 1, text.size(), fp) == text.size());
    assert(fclose(fp) == 0 && rename(temp.c_str(), path.c_str()) == 0);
}
static std::string read_text(const std::string &path) {
    FILE *fp = fopen(path.c_str(), "r"); assert(fp);
    char text[512]; assert(fgets(text, sizeof(text), fp)); fclose(fp);
    return text;
}

// Simulates only BusyBox's foreground/background contract, never runs a hook
// or sends DHCP packets. Production has no executable override.
static int fake_client(int argc, char **argv) {
    std::string pidfile, hook;
    bool arping = false;
    for (int i = 1; i < argc; ++i) {
        assert(strcmp(argv[i], "-q") && strcmp(argv[i], "-f") && strcmp(argv[i], "-b"));
        if (!strcmp(argv[i], "-a2000")) arping = true;
        if (!strcmp(argv[i], "-i")) assert(i+1 < argc && !strcmp(argv[i+1], "wlan0"));
        if (!strcmp(argv[i], "-p")) pidfile = argv[i+1];
        if (!strcmp(argv[i], "-s")) hook = argv[i+1];
    }
    assert(!pidfile.empty() && !hook.empty() && arping);
    std::string job = pidfile.substr(0, pidfile.size()-4);
    const char *mode = getenv("LAWREC_DHCP_TEST_MODE");
    write_text(pidfile, std::to_string(getpid())+"\n");
    if (mode && !strcmp(mode, "slow")) std::this_thread::sleep_for(std::chrono::seconds(5));
    bool error = mode && !strcmp(mode, "hook-error");
    assert(lawrec_dhcp_hook_result(job.c_str(), error ? -ENOSPC : 0,
        error ? "" : "192.168.10.74", error ? 0 : 60) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (child) return 0;
    assert(setsid() >= 0);
    signal(SIGTERM, stop_signal);
    int nullfd = open("/dev/null", O_RDWR); assert(nullfd >= 0);
    dup2(nullfd, 0); dup2(nullfd, 1); dup2(nullfd, 2); close(nullfd);
    // Delayed PID rewrite exercises discovery after the parent has exited.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    write_text(pidfile, std::to_string(getpid())+"\n");
    unsigned lease = 60;
    while (!stopped) {
        if (!error && !(mode && !strcmp(mode, "frozen")))
            assert(lawrec_dhcp_hook_result(job.c_str(), 0, "192.168.10.74", ++lease) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    _exit(0);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-n")) return fake_client(argc, argv);
    assert(argc == 2);
    assert(mkdir(argv[1], 0700) == 0);
    char executable[1024]; assert(realpath(argv[0], executable));
    setenv("LAWREC_DHCP_TEST_PROGRAM", executable, 1);
    setenv("LAWREC_NETWORK_RUN_DIR", argv[1], 1);
    std::string dir = argv[1];
    std::atomic<bool> cancel{false};
    lawrec_dhcp_status status;
    assert(lawrec_dhcp_get(&status) == 0 && status.pid == 0);
    assert(lawrec_dhcp_stop() == 0);
    assert(lawrec_dhcp_acquire(cancel) == 0);
    assert(lawrec_dhcp_get(&status) == 0 && status.pid && status.bound);
    assert(!strcmp(status.address, "192.168.10.74"));
    pid_t first = status.pid;
    unsigned initial = status.lease_seconds;
    std::this_thread::sleep_for(std::chrono::milliseconds(240));
    assert(lawrec_dhcp_get(&status) == 0 && status.pid == first && status.lease_seconds > initial);
    std::string job = read_text(dir+"/lawrec-dhcp-current"); job.pop_back();
    assert(lawrec_dhcp_hook_verify(job.c_str()) == 0);
    assert(lawrec_dhcp_hook_verify((job+"/../escape").c_str()) == -EINVAL);
    int lock = open((dir+"/lawrec-dhcp.lock").c_str(), O_RDWR); assert(lock >= 0);
    assert(!flock(lock, LOCK_EX | LOCK_NB));
    assert(lawrec_dhcp_stop() == -EBUSY && lawrec_dhcp_acquire(cancel) == -EBUSY);
    close(lock);
    write_text(job+"/pid", std::to_string(getpid())+"\n");
    assert(lawrec_dhcp_stop() == -EPERM); // Must not signal this unrelated process.
    assert(kill(first, 0) == 0);
    write_text(job+"/pid", std::to_string(first)+"\n");
    int pinned = syscall(SYS_pidfd_open, first, 0); assert(pinned >= 0);
    assert(lawrec_dhcp_acquire(cancel) == 0);
    pollfd dead{pinned, POLLIN, 0}; assert(poll(&dead, 1, 1000) == 1); close(pinned);
    assert(lawrec_dhcp_get(&status) == 0 && status.pid && status.pid != first);
    assert(lawrec_dhcp_hook_verify(job.c_str()) == -EINVAL);
    assert(lawrec_dhcp_stop() == 0);
    assert(lawrec_dhcp_get(&status) == 0 && !status.pid);
    assert(lawrec_dhcp_acquire(cancel) == 0);
    assert(lawrec_dhcp_get(&status) == 0 && status.bound);
    pinned = syscall(SYS_pidfd_open, status.pid, 0); assert(pinned >= 0);
    assert(syscall(SYS_pidfd_send_signal, pinned, SIGTERM, nullptr, 0) == 0);
    dead = pollfd{pinned, POLLIN, 0};
    assert(poll(&dead, 1, 1000) == 1); close(pinned);
    assert(lawrec_dhcp_get(&status) == 0 && !status.bound && status.error == -ENETDOWN);
    assert(lawrec_dhcp_stop() == 0); // Retired lease data must not mask client death.
    // Keep one real pinned owner alive while varying only metadata/test clock.
    setenv("LAWREC_DHCP_TEST_MODE", "frozen", 1);
    assert(lawrec_dhcp_acquire(cancel) == 0);
    assert(lawrec_dhcp_get(&status) == 0 && status.bound && status.remaining_seconds <= 60);
    job = read_text(dir+"/lawrec-dhcp-current"); job.pop_back();
    const std::string valid = read_text(job+"/lease");
    lawrec_dhcp_stamp received;
    assert(lawrec_dhcp_stamp_now(nullptr) == -EINVAL);
    assert(lawrec_dhcp_stamp_now(&received) == 0);
    boottime_offset_ms = 59000;
    assert(lawrec_dhcp_get(&status) == 0 && status.bound && status.remaining_seconds <= 1);
    boottime_offset_ms = 61000;
    assert(lawrec_dhcp_get(&status) == 0 && status.pid && !status.bound && status.error == -ETIMEDOUT);
    assert(status.lease_seconds == 60 && !status.remaining_seconds);
    // A slow hook cannot restart the duration at the time of successful apply.
    assert(lawrec_dhcp_hook_result(job.c_str(), 0, "192.168.10.74", 60, &received) == -ETIMEDOUT);
    assert(lawrec_dhcp_get(&status) == 0 && !status.bound && status.error == -ETIMEDOUT);
    boottime_offset_ms = 0;
    assert(lawrec_dhcp_hook_result(job.c_str(), 0, "192.168.10.74", 60) == 0);
    assert(lawrec_dhcp_get(&status) == 0 && status.bound && status.remaining_seconds);
    clock_failed = true;
    assert(lawrec_dhcp_get(&status) == -EIO && !status.bound);
    clock_failed = false;
    std::string other_boot = received.boot_id;
    other_boot[0] = other_boot[0] == 'a' ? 'b' : 'a';
    auto metadata = [&](const std::string &seconds, const std::string &stamp, const std::string &boot) {
        return "v2 0 "+seconds+" 192.168.10.74 "+stamp+" "+boot+"\n";
    };
    write_text(job+"/lease", metadata("60", std::to_string(received.received_ms), other_boot));
    assert(lawrec_dhcp_get(&status) == 0 && !status.bound && status.error == -ESTALE);
    write_text(job+"/lease", metadata("60", std::to_string(received.received_ms+100000), received.boot_id));
    assert(lawrec_dhcp_get(&status) == -EPROTO && !status.bound);
    for (const auto &bad : {std::string("0 60 192.168.10.74\n"),
        metadata("-1", "0", received.boot_id),
        metadata("-18446744073709551615", "0", received.boot_id),
        metadata("4294967296", "0", received.boot_id),
        metadata("60", "18446744073709551616", received.boot_id),
        metadata("60", "-18446744073709551615", received.boot_id),
        metadata("60", "0", "bad-boot"),
        metadata("60", "0", received.boot_id)+"extra\n"}) {
        write_text(job+"/lease", bad);
        assert(lawrec_dhcp_get(&status) == -EPROTO && !status.bound);
    }
    assert(lawrec_dhcp_hook_result(job.c_str(), 0, "239.0.0.1", 60) == -EINVAL);
    assert(lawrec_dhcp_hook_result(job.c_str(), 0, "192.168.10.74", 0) == -EINVAL);
    assert(lawrec_dhcp_hook_result(job.c_str(), -EIO, "192.168.10.74", 0) == -EINVAL);
    assert(lawrec_dhcp_hook_result(job.c_str(), -EIO, "", 60) == -EINVAL);
    received.received_ms += 100000;
    assert(lawrec_dhcp_hook_result(job.c_str(), 0, "192.168.10.74", 60, &received) == -EINVAL);
    snprintf(received.boot_id, sizeof(received.boot_id), "%s", other_boot.c_str());
    assert(lawrec_dhcp_hook_result(job.c_str(), 0, "192.168.10.74", 60, &received) == -ESTALE);
    write_text(job+"/lease", valid);
    assert(lawrec_dhcp_get(&status) == 0 && status.bound);
    assert(lawrec_dhcp_stop() == 0);
    setenv("LAWREC_DHCP_TEST_MODE", "hook-error", 1);
    assert(lawrec_dhcp_acquire(cancel) == -ENOSPC);
    assert(lawrec_dhcp_get(&status) == 0 && !status.pid);
    setenv("LAWREC_DHCP_TEST_MODE", "slow", 1);
    std::thread aborter([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); cancel = true; });
    assert(lawrec_dhcp_acquire(cancel) == -ECANCELED);
    aborter.join();
    assert(lawrec_dhcp_get(&status) == 0 && !status.pid);
    assert(lawrec_dhcp_acquire(cancel) == -ECANCELED);
    unlink((dir+"/lawrec-dhcp.lock").c_str());
    assert(rmdir(dir.c_str()) == 0);
    puts("dhcp: continuous owner, PID rewrite, scoped stop, lease expiration/boot identity, slow hook, strict metadata, cancellation and locking passed; no real DHCP");
}
