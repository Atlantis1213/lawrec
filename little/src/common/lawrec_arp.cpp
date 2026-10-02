#include "lawrec_arp.h"
#include <algorithm>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {
using Clock = std::chrono::steady_clock;
bool valid_mac(const uint8_t *mac)
{
    return !(mac[0] & 1) && std::any_of(mac, mac+6, [](uint8_t v) { return v != 0; });
}
struct PacketIo : LawrecArpIo {
    int fd = -1, index = 0;
    LawrecArpMac mac{};
    ~PacketIo() { if (fd >= 0) close(fd); }
    uint64_t now_ms() override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    }
    int identity(LawrecArpMac &value, int &iface) {
        ifreq request{};
        strcpy(request.ifr_name, "wlan0");
        if (ioctl(fd, SIOCGIFINDEX, &request)) return -errno;
        iface = request.ifr_ifindex;
        if (ioctl(fd, SIOCGIFFLAGS, &request)) return -errno;
        if (!(request.ifr_flags & IFF_UP) || !(request.ifr_flags & IFF_RUNNING)) return -ENETDOWN;
        if (ioctl(fd, SIOCGIFHWADDR, &request)) return -errno;
        if (request.ifr_hwaddr.sa_family != ARPHRD_ETHER) return -ENOTSUP;
        memcpy(value.data(), request.ifr_hwaddr.sa_data, value.size());
        return iface > 0 && valid_mac(value.data()) ? 0 : -EINVAL;
    }
    int open(LawrecArpMac &value) override {
        fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ARP));
        if (fd < 0) return -errno;
        int ret = identity(mac, index);
        if (ret) return ret;
        sockaddr_ll local{};
        local.sll_family = AF_PACKET; local.sll_protocol = htons(ETH_P_ARP); local.sll_ifindex = index;
        if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local))) return -errno;
        value = mac; return 0;
    }
    int check_link() override {
        LawrecArpMac current{}; int iface = 0;
        int ret = identity(current, iface);
        return ret ? ret : current != mac || iface != index ? -ESTALE : 0;
    }
    int delays(std::array<unsigned, 3> &ms) override {
        std::array<uint32_t, 3> random{};
        ssize_t size = getrandom(random.data(), sizeof(random), GRND_NONBLOCK);
        if (size != sizeof(random)) return size < 0 ? -errno : -EIO;
        ms = {{random[0] % 1001, 1000+random[1] % 1001, 1000+random[2] % 1001}};
        return 0;
    }
    int send(const LawrecArpPacket &packet) override {
        sockaddr_ll destination{};
        destination.sll_family = AF_PACKET; destination.sll_protocol = htons(ETH_P_ARP);
        destination.sll_ifindex = index; destination.sll_halen = 6;
        memset(destination.sll_addr, 0xff, 6);
        ssize_t size = sendto(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                              reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
        return size < 0 ? -errno : size_t(size) == packet.size() ? 0 : -EIO;
    }
    int receive(unsigned timeout, std::array<uint8_t, 128> &packet, size_t &size) override {
        size = 0;
        pollfd descriptor{fd, POLLIN, 0};
        int ret = poll(&descriptor, 1, timeout);
        if (ret < 0) return errno == EINTR ? 0 : -errno;
        if (!ret) return 0;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
        if (!(descriptor.revents & POLLIN)) return -EIO;
        sockaddr_ll source{}; socklen_t length = sizeof(source);
        ssize_t bytes = recvfrom(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_TRUNC,
                                 reinterpret_cast<sockaddr *>(&source), &length);
        if (bytes < 0) return errno == EAGAIN || errno == EINTR ? 0 : -errno;
        if (length < sizeof(source) || source.sll_family != AF_PACKET || source.sll_ifindex != index ||
            source.sll_pkttype == PACKET_OUTGOING || source.sll_protocol != htons(ETH_P_ARP) ||
            size_t(bytes) > packet.size()) return 0;
        size = bytes; return 1;
    }
};
}

LawrecArpPacket lawrec_arp_packet(uint32_t address, const LawrecArpMac &mac)
{
    // RFC 5227 Probe: Ethernet/IPv4 request, SHA=ours, SPA=0, THA=0, TPA=candidate.
    LawrecArpPacket packet{{0,1,8,0,6,4,0,1}};
    memcpy(packet.data()+8, mac.data(), mac.size());
    memcpy(packet.data()+24, &address, sizeof(address));
    return packet;
}

bool lawrec_arp_conflict(const uint8_t *p, size_t size, uint32_t address,
                         const LawrecArpMac &own, LawrecArpMac *peer)
{
    if (!p || size < 28 || p[0] || p[1] != 1 || p[2] != 8 || p[3] || p[4] != 6 || p[5] != 4 ||
        p[6] || (p[7] != 1 && p[7] != 2) || !valid_mac(p+8) || !memcmp(p+8, own.data(), 6)) return false;
    uint32_t sender, target;
    memcpy(&sender, p+14, 4); memcpy(&target, p+24, 4);
    // A peer already using the address, or another peer probing it concurrently.
    bool conflict = sender == address || (p[7] == 1 && sender == 0 && target == address);
    if (conflict && peer) memcpy(peer->data(), p+8, peer->size());
    return conflict;
}

int lawrec_arp_probe(LawrecArpIo &io, const char *text, const std::atomic<bool> &cancel)
{
    in_addr address{};
    if (!text || strnlen(text, 16) >= 16 || inet_pton(AF_INET, text, &address) != 1 ||
        !(ntohl(address.s_addr) >> 24) || (ntohl(address.s_addr) >> 24) == 127 ||
        (ntohl(address.s_addr) >> 24) >= 224) return -EINVAL;
    LawrecArpMac own{}, peer{};
    unsigned sent = 0;
    auto begin = io.now_ms();
    int ret = cancel ? -ECANCELED : io.open(own);
    if (!ret && !valid_mac(own.data())) ret = -EINVAL;
    std::array<unsigned, 3> delay{};
    if (!ret) ret = io.delays(delay);
    if (!ret && (delay[0] > 1000 || delay[1] < 1000 || delay[1] > 2000 ||
                 delay[2] < 1000 || delay[2] > 2000)) ret = -EINVAL;
    auto packet = lawrec_arp_packet(address.s_addr, own);
    uint64_t due = io.now_ms()+delay[0];
    while (!ret) {
        uint64_t now = io.now_ms();
        if (cancel) { ret = -ECANCELED; break; }
        if (now-begin >= 9000) { ret = -ETIMEDOUT; break; }
        if (now >= due) {
            ret = io.check_link();
            if (ret || sent == 3) break;
            ret = io.send(packet);
            if (ret) break;
            ++sent;
            fprintf(stderr, "[arp] probe address=%s count=%u\n", text, sent);
            // Two-second quiet window after the last of three probes.
            due = io.now_ms()+(sent == 3 ? 2000 : delay[sent]);
        }
        std::array<uint8_t, 128> incoming{}; size_t size = 0;
        now = io.now_ms();
        if (now >= due) continue;
        int received = io.receive(std::min<uint64_t>(50, due-now), incoming, size);
        if (received < 0) ret = received;
        else if (received > 0 && lawrec_arp_conflict(incoming.data(), std::min(size, incoming.size()),
                                                    address.s_addr, own, &peer)) ret = -EADDRINUSE;
    }
    fprintf(stderr, "[arp] result address=%s error=%d probes=%u elapsed_ms=%llu peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
            text, ret, sent, (unsigned long long)(io.now_ms()-begin), peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
    return ret;
}

int lawrec_arp_probe(const char *address, const std::atomic<bool> &cancel)
{
    PacketIo io;
    return lawrec_arp_probe(io, address, cancel);
}
