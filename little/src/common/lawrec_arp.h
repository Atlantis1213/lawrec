#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

using LawrecArpMac = std::array<uint8_t, 6>;
using LawrecArpPacket = std::array<uint8_t, 28>;

// Small transport seam for deterministic tests; production uses an interface-
// bound packet socket, never executes a command or changes an address here.
struct LawrecArpIo {
    virtual ~LawrecArpIo() = default;
    virtual uint64_t now_ms() = 0;
    virtual int open(LawrecArpMac &mac) = 0;
    virtual int check_link() = 0;
    virtual int delays(std::array<unsigned, 3> &ms) = 0;
    virtual int send(const LawrecArpPacket &packet) = 0;
    // 1 = incoming ARP payload, 0 = timeout/ignored, negative = transport error.
    virtual int receive(unsigned timeout_ms, std::array<uint8_t, 128> &packet, size_t &size) = 0;
};

LawrecArpPacket lawrec_arp_packet(uint32_t address, const LawrecArpMac &mac);
bool lawrec_arp_conflict(const uint8_t *packet, size_t size, uint32_t address,
                         const LawrecArpMac &own_mac, LawrecArpMac *peer = nullptr);
int lawrec_arp_probe(LawrecArpIo &io, const char *address, const std::atomic<bool> &cancel);
// wlan0 only. No privilege/transport errors are interpreted as a free address.
int lawrec_arp_probe(const char *address, const std::atomic<bool> &cancel);
