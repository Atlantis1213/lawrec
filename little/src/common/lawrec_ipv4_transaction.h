#pragma once
#include "lawrec_settings.h"

struct LawrecIpv4TransactionBackend {
    virtual ~LawrecIpv4TransactionBackend() = default;
    // Includes static-address probing before begin/stop_dhcp mutate anything.
    virtual int preflight(const lawrec_ipv4_settings &desired) = 0;
    virtual int begin() = 0;
    virtual int stop_dhcp() = 0;
    // Recovery ignores cancellation but each operation remains bounded.
    virtual int apply(const lawrec_ipv4_settings &settings, bool recovery) = 0;
    virtual int commit(const lawrec_ipv4_settings &settings) = 0;
};
struct LawrecIpv4Result {
    int error = 0;
    int rollback_error = 0;
    bool changed = false;
};
LawrecIpv4Result lawrec_ipv4_switch(LawrecIpv4TransactionBackend &backend,
                                  const lawrec_ipv4_settings &previous,
                                  const lawrec_ipv4_settings &desired);
