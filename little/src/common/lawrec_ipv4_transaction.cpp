#include "lawrec_ipv4_transaction.h"
#include <cerrno>
#include <cstdio>

namespace {
template<class F> int guarded(F operation) {
    try { return operation(); }
    catch (...) { return -EFAULT; }
}
}

LawrecIpv4Result lawrec_ipv4_switch(LawrecIpv4TransactionBackend &backend,
                                  const lawrec_ipv4_settings &previous,
                                  const lawrec_ipv4_settings &desired)
{
    LawrecIpv4Result result;
    result.error = lawrec_settings_ipv4_validate(&previous);
    if (!result.error) result.error = lawrec_settings_ipv4_validate(&desired);
    if (!result.error) result.error = guarded([&] { return backend.preflight(desired); });
    if (!result.error) result.error = guarded([&] { return backend.begin(); });
    if (result.error) {
        // No network mutation before the marker exists.
        fprintf(stderr, "[ipv4] preflight result=%d changed=0 desired_dhcp=%u\n", result.error, desired.dhcp);
        return result;
    }
    result.changed = true;
    result.error = guarded([&] { return backend.stop_dhcp(); });
    if (!result.error) result.error = guarded([&] { return backend.apply(desired, false); });
    if (!result.error) result.error = guarded([&] { return backend.commit(desired); });
    if (result.error) {
        // A failed new DHCP client can still be alive. Do not overwrite its
        // network from rollback until its synchronous lease hook has exited.
        result.rollback_error = guarded([&] { return backend.stop_dhcp(); });
        if (!result.rollback_error)
            result.rollback_error = guarded([&] { return backend.apply(previous, true); });
        if (!result.rollback_error)
            result.rollback_error = guarded([&] { return backend.commit(previous); });
    }
    fprintf(stderr, "[ipv4] switch result=%d rollback=%d changed=%d desired_dhcp=%u\n",
            result.error, result.rollback_error, result.changed, desired.dhcp);
    return result;
}
