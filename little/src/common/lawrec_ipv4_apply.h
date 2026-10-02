#pragma once
#include "lawrec_settings.h"
#include <string>
#include <vector>

class LawrecIpv4Backend {
public:
    virtual ~LawrecIpv4Backend() = default;
    virtual int run(const std::vector<std::string> &args) = 0;
    virtual int dns(const lawrec_ipv4_settings &settings) = 0;
    virtual int probe(const lawrec_ipv4_settings &settings) = 0;
};
/* Called only after association; changes wlan0 routes/addresses, not other NICs. */
int lawrec_ipv4_apply_static(LawrecIpv4Backend &backend, const lawrec_ipv4_settings &settings);
int lawrec_ipv4_lease_settings(const char *address, const char *subnet, const char *routers,
                              const char *dns, lawrec_ipv4_settings *settings);
int lawrec_ipv4_apply_lease(LawrecIpv4Backend &backend, const lawrec_ipv4_settings &settings,
                           bool keep_address = false);
/* Replace only wlan0-tagged resolver entries, preserving other NICs/search policy. */
std::string lawrec_ipv4_resolver_text(const lawrec_ipv4_settings &settings, const std::string &existing);
