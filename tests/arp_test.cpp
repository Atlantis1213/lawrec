#include "lawrec_arp.h"
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <vector>

static const LawrecArpMac own{{0x02,0,0,0,0,1}}, peer{{0x02,0,0,0,0,2}};
static uint32_t ip(const char *text) { in_addr value{}; assert(inet_pton(AF_INET, text, &value) == 1); return value.s_addr; }
struct Fake : LawrecArpIo {
    uint64_t now = 0;
    uint64_t open_time = 0;
    int open_error = 0, link_error = 0, send_error = 0, receive_error = 0, random_error = 0;
    unsigned links = 0, fail_link = 0;
    std::array<unsigned, 3> jitter{{0,1000,1000}};
    bool bad_mac = false, flood = false;
    std::atomic<bool> *cancel = nullptr;
    uint64_t abort_at = UINT64_MAX;
    struct Event { uint64_t when; std::vector<uint8_t> data; };
    std::vector<Event> events;
    std::vector<uint64_t> sends;
    uint64_t now_ms() override { return now; }
    int open(LawrecArpMac &mac) override { now += open_time; mac = bad_mac ? LawrecArpMac{} : own; return open_error; }
    int check_link() override { return ++links == fail_link ? link_error : 0; }
    int delays(std::array<unsigned, 3> &ms) override { ms = jitter; return random_error; }
    int send(const LawrecArpPacket &packet) override {
        assert(packet == lawrec_arp_packet(ip("192.168.10.74"), own));
        sends.push_back(now); return send_error;
    }
    int receive(unsigned timeout, std::array<uint8_t, 128> &data, size_t &size) override {
        assert(timeout <= 50);
        if (receive_error) return receive_error;
        if (flood) { ++now; size = 1; data[0] = 0; return 1; }
        if (!events.empty() && events[0].when <= now+timeout) {
            now = events[0].when; size = events[0].data.size();
            memcpy(data.data(), events[0].data.data(), size);
            events.erase(events.begin()); return 1;
        }
        now += timeout;
        if (cancel && now >= abort_at) *cancel = true;
        return 0;
    }
    void event(uint64_t at, const LawrecArpPacket &packet) {
        events.push_back({at, std::vector<uint8_t>(packet.begin(), packet.end())});
    }
};

int main()
{
    uint32_t candidate = ip("192.168.10.74");
    auto probe = lawrec_arp_packet(candidate, peer);
    assert(probe[0] == 0 && probe[1] == 1 && probe[4] == 6 && probe[5] == 4 && probe[7] == 1);
    for (unsigned i = 14; i < 24; ++i) assert(probe[i] == 0);
    assert(!memcmp(probe.data()+24, &candidate, 4));
    LawrecArpMac found{};
    assert(lawrec_arp_conflict(probe.data(), probe.size(), candidate, own, &found) && found == peer);
    auto claim = probe; claim[7] = 2; memcpy(claim.data()+14, &candidate, 4);
    assert(lawrec_arp_conflict(claim.data(), claim.size(), candidate, own));
    claim[7] = 1; assert(lawrec_arp_conflict(claim.data(), claim.size(), candidate, own));
    // Ignore our own traffic/reflection, unrelated targets, invalid headers and short input.
    auto self = lawrec_arp_packet(candidate, own);
    assert(!lawrec_arp_conflict(self.data(), self.size(), candidate, own));
    auto other = lawrec_arp_packet(ip("192.168.10.75"), peer);
    assert(!lawrec_arp_conflict(other.data(), other.size(), candidate, own));
    assert(!lawrec_arp_conflict(nullptr, 28, candidate, own));
    for (size_t size = 0; size < 28; ++size) assert(!lawrec_arp_conflict(probe.data(), size, candidate, own));
    for (unsigned field : {0,1,2,3,4,5,6,7}) {
        auto malformed = probe; malformed[field] = 9;
        assert(!lawrec_arp_conflict(malformed.data(), malformed.size(), candidate, own));
    }
    auto bad = probe; memset(bad.data()+8, 0, 6);
    assert(!lawrec_arp_conflict(bad.data(), bad.size(), candidate, own));
    bad = probe; bad[8] |= 1;
    assert(!lawrec_arp_conflict(bad.data(), bad.size(), candidate, own));
    std::array<uint8_t, 128> padded{}; memcpy(padded.data(), claim.data(), 28);
    assert(lawrec_arp_conflict(padded.data(), 46, candidate, own));

    std::atomic<bool> cancel{false};
    Fake clear;
    assert(lawrec_arp_probe(clear, "192.168.10.74", cancel) == 0);
    assert((clear.sends == std::vector<uint64_t>{0,1000,2000}) && clear.now == 4000 && clear.links == 4);
    Fake latest; latest.jitter = {{1000,2000,2000}};
    assert(lawrec_arp_probe(latest, "192.168.10.74", cancel) == 0 && latest.now == 7000);
    for (auto packet : {probe, claim}) {
        Fake conflict; conflict.event(20, packet);
        assert(lawrec_arp_probe(conflict, "192.168.10.74", cancel) == -EADDRINUSE);
        assert(conflict.sends.size() == 1 && conflict.now == 20);
    }
    Fake late; late.event(3999, claim);
    assert(lawrec_arp_probe(late, "192.168.10.74", cancel) == -EADDRINUSE && late.sends.size() == 3);
    Fake ignored; ignored.event(20, self); ignored.event(50, other);
    assert(lawrec_arp_probe(ignored, "192.168.10.74", cancel) == 0 && ignored.sends.size() == 3);
    for (int stage = 0; stage < 5; ++stage) {
        Fake failed;
        if (stage == 0) failed.open_error = -EPERM;
        if (stage == 1) failed.random_error = -EAGAIN;
        if (stage == 2) { failed.link_error = -ENETDOWN; failed.fail_link = 2; }
        if (stage == 3) failed.send_error = -ENOBUFS;
        if (stage == 4) failed.receive_error = -EIO;
        int expected[] = {-EPERM, -EAGAIN, -ENETDOWN, -ENOBUFS, -EIO};
        assert(lawrec_arp_probe(failed, "192.168.10.74", cancel) == expected[stage]);
    }
    Fake invalid_mac; invalid_mac.bad_mac = true;
    assert(lawrec_arp_probe(invalid_mac, "192.168.10.74", cancel) == -EINVAL && invalid_mac.sends.empty());
    Fake busy; busy.flood = true;
    assert(lawrec_arp_probe(busy, "192.168.10.74", cancel) == 0 && busy.now == 4000);
    Fake timeout; timeout.open_time = 9000;
    assert(lawrec_arp_probe(timeout, "192.168.10.74", cancel) == -ETIMEDOUT && timeout.sends.empty());
    Fake stale; stale.link_error = -ESTALE; stale.fail_link = 4;
    assert(lawrec_arp_probe(stale, "192.168.10.74", cancel) == -ESTALE && stale.sends.size() == 3);
    Fake aborting; aborting.cancel = &cancel; aborting.abort_at = 75;
    assert(lawrec_arp_probe(aborting, "192.168.10.74", cancel) == -ECANCELED && aborting.now <= 125);
    Fake stopped; assert(lawrec_arp_probe(stopped, "192.168.10.74", cancel) == -ECANCELED && stopped.sends.empty());
    cancel = false;
    for (const char *text : {"0.0.0.0", "127.0.0.1", "224.0.0.1", "192.168.010.74", "bad;command"}) {
        Fake invalid; assert(lawrec_arp_probe(invalid, text, cancel) == -EINVAL && invalid.sends.empty());
    }
    puts("ARP: exact probes, jitter/quiet window, claims/concurrent probes, malformed/self frames, fail-closed transport, cancellation and flood bounds passed; simulated I/O");
}
