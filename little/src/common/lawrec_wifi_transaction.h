#pragma once
#include <string>

/* Transport and DHCP are injected so rollback is tested without modifying the
 * host network. Recovery operations must ignore cancellation but stay bounded. */
struct LawrecWifiBackend {
    virtual ~LawrecWifiBackend() = default;
    virtual int request(const std::string &command, std::string &reply, bool recovery) = 0;
    virtual int wait_connected(int id, bool recovery) = 0;
    virtual int dhcp(bool recovery) = 0;
};
struct LawrecWifiResult {
    int error = 0;
    int rollback_error = 0;
    bool changed = false;
};
LawrecWifiResult lawrec_wifi_connect(LawrecWifiBackend &backend,
                                    const std::string &ssid, const std::string &password);
