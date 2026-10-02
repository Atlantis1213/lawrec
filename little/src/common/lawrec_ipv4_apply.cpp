#include "lawrec_ipv4_apply.h"
#include <cerrno>
#include <sstream>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>

int lawrec_ipv4_apply_static(LawrecIpv4Backend &b, const lawrec_ipv4_settings &s)
{
    int ret = lawrec_settings_ipv4_validate(&s);
    if (ret || s.dhcp) return ret ? ret : -EINVAL;
    ret = b.probe(s);
    return ret ? ret : lawrec_ipv4_apply_lease(b, s);
}

int lawrec_ipv4_apply_lease(LawrecIpv4Backend &b, const lawrec_ipv4_settings &s, bool keep_address)
{
    lawrec_ipv4_settings validated = s;
    // DHCP may legitimately omit DNS; clear only its old tagged entries.
    if (!validated.dns1[0]) snprintf(validated.dns1, sizeof(validated.dns1), "1.1.1.1");
    int ret = lawrec_settings_ipv4_validate(&validated);
    if (ret || s.dhcp) return ret ? ret : -EINVAL;
    ret = b.run({"/sbin/ip", "link", "set", "dev", "wlan0", "up"});
    if (!ret) ret = b.run({"/sbin/ip", "-4", "route", "flush", "default", "dev", "wlan0"});
    if (!ret && !keep_address) ret = b.run({"/sbin/ip", "-4", "addr", "flush", "dev", "wlan0", "scope", "global"});
    if (!ret) ret = b.run({"/sbin/ip", "-4", "addr", "replace",
                          std::string(s.address) + "/" + std::to_string(s.prefix), "dev", "wlan0"});
    if (!ret && s.gateway[0]) ret = b.run({"/sbin/ip", "-4", "route", "add", "default",
                                         "via", s.gateway, "dev", "wlan0", "metric", "600"});
    if (!ret) ret = b.dns(s);
    return ret;
}

int lawrec_ipv4_lease_settings(const char *address, const char *subnet, const char *routers,
                              const char *dns, lawrec_ipv4_settings *s)
{
    if (!s || !address || !subnet || strnlen(address, 16) >= 16 || strnlen(subnet, 16) >= 16 ||
        (routers && strnlen(routers, 256) >= 256) || (dns && strnlen(dns, 256) >= 256)) return -EINVAL;
    *s = lawrec_ipv4_settings{1, 0, 0, {}, {}, {}, {}};
    in_addr mask{};
    if (inet_pton(AF_INET, subnet, &mask) != 1) return -EINVAL;
    uint32_t bits = ntohl(mask.s_addr);
    while (bits & 0x80000000u) { ++s->prefix; bits <<= 1; }
    if (bits || s->prefix < 1 || s->prefix > 30) return -EINVAL;
    snprintf(s->address, sizeof(s->address), "%s", address);
    auto addresses = [](const char *list, char *first, char *second) {
        std::istringstream words(list ? list : "");
        std::string word;
        unsigned count = 0;
        while (words >> word) {
            in_addr value{};
            if (word.size() >= 16 || inet_pton(AF_INET, word.c_str(), &value) != 1 ||
                !(ntohl(value.s_addr) >> 24) || (ntohl(value.s_addr) >> 24) == 127 ||
                (ntohl(value.s_addr) >> 24) >= 224 || ++count > 16) return -EINVAL;
            if (count == 1) snprintf(first, 16, "%s", word.c_str());
            else if (count == 2 && second) snprintf(second, 16, "%s", word.c_str());
        }
        return 0;
    };
    int ret = addresses(routers, s->gateway, nullptr);
    if (!ret) ret = addresses(dns, s->dns1, s->dns2);
    lawrec_ipv4_settings validated = *s;
    if (!validated.dns1[0]) snprintf(validated.dns1, sizeof(validated.dns1), "1.1.1.1");
    return ret ? ret : lawrec_settings_ipv4_validate(&validated);
}

std::string lawrec_ipv4_resolver_text(const lawrec_ipv4_settings &s, const std::string &existing)
{
    std::string text;
    if (s.dns1[0]) text = std::string("nameserver ") + s.dns1 + " # wlan0\n";
    if (s.dns2[0]) text += std::string("nameserver ") + s.dns2 + " # wlan0\n";
    std::istringstream lines(existing);
    std::string line;
    while (std::getline(lines, line)) {
        size_t end = line.find_last_not_of(" \t\r");
        if (end != std::string::npos) line.resize(end+1);
        if (line.size() < 7 || line.compare(line.size()-7, 7, "# wlan0")) text += line + "\n";
    }
    return text;
}
