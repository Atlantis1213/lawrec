#pragma once
#include <string>

/* Transport and DHCP are injected so rollback is tested without modifying the
 * host network. Recovery operations must ignore cancellation but stay bounded. */
struct LawrecWifiBackend {
    virtual ~LawrecWifiBackend() = default;
    virtual int request(const std::string &command, std::string &reply, bool recovery) = 0;
    virtual int wait_connected(int id, bool recovery) = 0;
    /* Mark uncertain before stopping DHCP/association. Setup has not changed IP. */
    virtual int begin() { return 0; }
    /* A failed stop can already have sent SIGTERM; recovery is still required. */
    virtual int before_select() { return 0; }
    virtual int dhcp(bool recovery) = 0;
    /* Stop candidate hooks before restoring the previous association. */
    virtual int before_rollback() { return 0; }
    virtual int restore_disconnected() { return 0; }
    /* Clear uncertainty only after all association/IP/cleanup steps succeed. */
    virtual int commit(bool recovery) { (void)recovery; return 0; }
};
struct LawrecWifiResult {
    int error = 0;
    int rollback_error = 0;
    bool changed = false;
};
LawrecWifiResult lawrec_wifi_connect(LawrecWifiBackend &backend,
                                    const std::string &ssid, const std::string &password);
