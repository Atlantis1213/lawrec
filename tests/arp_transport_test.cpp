// Run only in the network-none Docker test with a private veth pair.
#include "lawrec_arp.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <cstdio>

static void setup()
{
    // A small netlink fixture avoids an iproute2 dependency in the SDK image.
    // The launcher verifies a network-none namespace with only lo beforehand.
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE); assert(fd >= 0);
    alignas(nlmsghdr) uint8_t bytes[1024]{};
    auto *header = reinterpret_cast<nlmsghdr *>(bytes);
    header->nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    header->nlmsg_type = RTM_NEWLINK;
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    header->nlmsg_seq = 1;
    auto attribute = [&](unsigned type, const void *data, unsigned size) {
        unsigned offset = NLMSG_ALIGN(header->nlmsg_len);
        assert(offset+RTA_SPACE(size) <= sizeof(bytes));
        auto *attr = reinterpret_cast<rtattr *>(bytes+offset);
        attr->rta_type = type; attr->rta_len = RTA_LENGTH(size);
        if (size) memcpy(RTA_DATA(attr), data, size);
        header->nlmsg_len = offset+RTA_SPACE(size);
        return attr;
    };
    auto finish = [&](rtattr *attr) { attr->rta_len = header->nlmsg_len-(reinterpret_cast<uint8_t *>(attr)-bytes); };
    attribute(IFLA_IFNAME, "wlan0", 6);
    auto *info = attribute(IFLA_LINKINFO | NLA_F_NESTED, nullptr, 0);
    attribute(IFLA_INFO_KIND, "veth", 5);
    auto *data = attribute(IFLA_INFO_DATA | NLA_F_NESTED, nullptr, 0);
    ifinfomsg peer{};
    auto *description = attribute(VETH_INFO_PEER | NLA_F_NESTED, &peer, sizeof(peer));
    attribute(IFLA_IFNAME, "arp-peer", 9);
    finish(description); finish(data); finish(info);
    sockaddr_nl kernel{}; kernel.nl_family = AF_NETLINK;
    assert(sendto(fd, bytes, header->nlmsg_len, 0, reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel)) == header->nlmsg_len);
    pollfd p{fd, POLLIN, 0}; assert(poll(&p, 1, 2000) == 1);
    ssize_t length = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
    assert(length >= ssize_t(NLMSG_LENGTH(sizeof(nlmsgerr))) && header->nlmsg_type == NLMSG_ERROR);
    auto *ack = reinterpret_cast<nlmsgerr *>(NLMSG_DATA(header)); assert(ack->error == 0);
    close(fd);
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); assert(fd >= 0);
    unsigned last = 1;
    for (const char *name : {"wlan0", "arp-peer"}) {
        ifreq request{}; strcpy(request.ifr_name, name);
        request.ifr_hwaddr.sa_family = ARPHRD_ETHER;
        request.ifr_hwaddr.sa_data[0] = 2; request.ifr_hwaddr.sa_data[5] = last++;
        assert(ioctl(fd, SIOCSIFHWADDR, &request) == 0);
        assert(ioctl(fd, SIOCGIFFLAGS, &request) == 0);
        request.ifr_flags |= IFF_UP;
        assert(ioctl(fd, SIOCSIFFLAGS, &request) == 0);
    }
    close(fd);
}

int main()
{
    setup();
    const LawrecArpMac own{{0x02,0,0,0,0,1}}, peer{{0x02,0,0,0,0,2}};
    in_addr address{}; assert(inet_pton(AF_INET, "192.0.2.74", &address) == 1);
    int fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ARP));
    assert(fd >= 0);
    sockaddr_ll iface{}; iface.sll_family = AF_PACKET; iface.sll_protocol = htons(ETH_P_ARP);
    iface.sll_ifindex = if_nametoindex("arp-peer"); assert(iface.sll_ifindex > 0);
    assert(bind(fd, reinterpret_cast<sockaddr *>(&iface), sizeof(iface)) == 0);
    sockaddr_ll broadcast = iface; broadcast.sll_halen = 6;
    memset(broadcast.sll_addr, 0xff, 6);
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        std::atomic<bool> done{false}, cancel{false};
        unsigned probes = 0;
        std::thread server([&] {
            while (!done) {
                pollfd p{fd, POLLIN, 0};
                if (poll(&p, 1, 20) <= 0) continue;
                uint8_t data[128]; sockaddr_ll from{}; socklen_t len = sizeof(from);
                ssize_t n = recvfrom(fd, data, sizeof(data), 0, reinterpret_cast<sockaddr *>(&from), &len);
                if (n < 0 || from.sll_pkttype == PACKET_OUTGOING || n < 28) continue;
                auto expected = lawrec_arp_packet(address.s_addr, own);
                assert(!memcmp(data, expected.data(), expected.size()));
                ++probes;
                if (scenario) {
                    auto response = lawrec_arp_packet(address.s_addr, peer);
                    if (scenario == 1) { response[7] = 2; memcpy(response.data()+14, &address.s_addr, 4); }
                    assert(sendto(fd, response.data(), response.size(), 0,
                                  reinterpret_cast<sockaddr *>(&broadcast), sizeof(broadcast)) == ssize_t(response.size()));
                }
            }
        });
        int result = lawrec_arp_probe("192.0.2.74", cancel);
        done = true; server.join();
        assert(result == (scenario ? -EADDRINUSE : 0));
        assert(probes == (scenario ? 1 : 3));
    }
    std::atomic<bool> cancel{false};
    // Root in this private container can drop privilege without modifying a
    // host interface. Missing CAP_NET_RAW must be an error, never a free IP.
    assert(seteuid(65534) == 0);
    assert(lawrec_arp_probe("192.0.2.74", cancel) == -EPERM);
    assert(seteuid(0) == 0);
    ifreq request{}; strcpy(request.ifr_name, "wlan0");
    assert(ioctl(fd, SIOCGIFFLAGS, &request) == 0);
    request.ifr_flags &= ~IFF_UP;
    assert(ioctl(fd, SIOCSIFFLAGS, &request) == 0);
    assert(lawrec_arp_probe("192.0.2.74", cancel) == -ENETDOWN);
    close(fd);
    puts("ARP transport: real packet socket, three probes, peer claim/concurrent probe, permission failure and link-down passed; private Docker veth, no IPv4 configured");
}
