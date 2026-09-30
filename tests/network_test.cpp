#include "lawrec_network.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>

static lawrec_network_snapshot wait_done()
{
    lawrec_network_snapshot s{};
    for (int i = 0; i < 400; ++i) {
        lawrec_network_get(&s);
        if (!s.busy) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(!"network worker failed to finish");
    return s;
}

int main()
{
    char dir[] = "/tmp/lawrec-network-test-XXXXXX";
    assert(mkdtemp(dir));
    std::string path = std::string(dir)+"/wlan0";
    setenv("LAWREC_WPA_CTRL_PATH", path.c_str(), 1);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, path.c_str());
    assert(!bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)));
    std::atomic<bool> stop{false};
    std::atomic<int> mode{0}, scans{0}, unexpected{0};
    std::thread server([&] {
        while (!stop) {
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            char buf[256] = {};
            sockaddr_un peer{};
            socklen_t len = sizeof(peer);
            ssize_t n = recvfrom(fd, buf, sizeof(buf)-1, 0, reinterpret_cast<sockaddr *>(&peer), &len);
            assert(n > 0);
            auto send_reply = [&](const std::string &s) {
                sendto(fd, s.data(), s.size(), 0, reinterpret_cast<sockaddr *>(&peer), len);
            };
            if (!strcmp(buf, "STATUS")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                if (mode == 3) continue;
                if (mode == 2) { send_reply(std::string(17000, 'X')); continue; }
                if (mode == 4) { send_reply("not-a-status\n"); continue; }
                send_reply("wpa_state=COMPLETED\nssid=Test\\x20AP\nip_address=192.168.1.2\n");
            } else if (!strcmp(buf, "ATTACH")) send_reply("OK\n");
            else if (!strcmp(buf, "SCAN")) {
                ++scans;
                if (mode == 1) { send_reply("FAIL-BUSY\n"); continue; }
                // A scan event can arrive before the command's OK response.
                send_reply("<3>CTRL-EVENT-SCAN-RESULTS\n");
                send_reply("OK\n");
            } else if (!strcmp(buf, "SCAN_RESULTS")) {
                send_reply("bssid / frequency / signal level / flags / ssid\n"
                           "00:11:22:33:44:55\t2412\t-41\t[WPA2-PSK-CCMP][ESS]\tTest\\x20AP\n"
                           "00:11:22:33:44:56\t2412\t-42\t[ESS]\t\n"
                           "00:11:22:33:44:57\t2412\t-43\t[ESS]\tbad\\x00ssid\n"
                           "00:11:22:33:44:58\t2412\tbogus\t[ESS]\tbad\n"
                           "00:11:22:33:44:59\t2412\t-50\t[ESS]\tCafe\n");
            } else { ++unexpected; send_reply("UNKNOWN COMMAND\n"); }
        }
    });
    std::string missing = std::string(dir)+"/missing";
    setenv("LAWREC_WPA_CTRL_PATH", missing.c_str(), 1);
    assert(lawrec_network_refresh_async(0) == 0);
    auto missing_result = wait_done(); assert(missing_result.error == -ENOENT);
    setenv("LAWREC_WPA_CTRL_PATH", path.c_str(), 1);
    assert(lawrec_network_refresh_async(0) == 0);
    assert(lawrec_network_refresh_async(1) == -EBUSY);
    auto s = wait_done();
    assert(s.error == 0 && !strcmp(s.state, "COMPLETED"));
    assert(!strcmp(s.ssid, "Test AP") && !strcmp(s.ipv4, "192.168.1.2"));
    assert(lawrec_network_refresh_async(1) == 0);
    s = wait_done();
    assert(s.error == 0 && s.count == 2 && !strcmp(s.aps[0].ssid, "Test AP"));
    assert(s.aps[0].signal_dbm == -41 && !strcmp(s.aps[1].ssid, "Cafe"));
    mode = 1;
    assert(lawrec_network_refresh_async(1) == 0);
    s = wait_done(); assert(s.error == -EIO && s.count == 0);
    mode = 2;
    assert(lawrec_network_refresh_async(0) == 0);
    s = wait_done(); assert(s.error == -EMSGSIZE);
    mode = 4;
    assert(lawrec_network_refresh_async(0) == 0);
    s = wait_done(); assert(s.error == -EPROTO);
    mode = 3;
    assert(lawrec_network_refresh_async(0) == 0);
    s = wait_done(); assert(s.error == -ETIMEDOUT);
    assert(lawrec_network_refresh_async(0) == 0);
    auto begin = std::chrono::steady_clock::now();
    lawrec_network_shutdown();
    assert(std::chrono::steady_clock::now()-begin < std::chrono::seconds(1));
    assert(lawrec_network_refresh_async(0) == -ESHUTDOWN);
    stop = true; server.join();
    assert(scans == 2 && unexpected == 0);
    close(fd); unlink(path.c_str()); rmdir(dir);
    puts("network: status, scan events, SSID parsing, busy, truncation, timeout, shutdown passed");
}
