#include "lawrec_wifi_transaction.h"
#include <vector>
#include <sstream>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace {
struct Network { int id; bool enabled; };
bool number(const std::string &s, int &id) {
    char *end;
    errno = 0;
    long value = strtol(s.c_str(), &end, 10);
    if (errno || end == s.c_str() || value < 0 || value > INT_MAX) return false;
    while (*end == '\n') ++end;
    if (*end) return false;
    id = value; return true;
}
int ok(LawrecWifiBackend &b, const std::string &cmd, bool recovery = false) {
    std::string reply;
    int ret = b.request(cmd, reply, recovery);
    return ret ? ret : reply == "OK\n" ? 0 : -EPROTO;
}
}

LawrecWifiResult lawrec_wifi_connect(LawrecWifiBackend &b, const std::string &ssid,
                                    const std::string &password)
{
    LawrecWifiResult result;
    if (ssid.empty() || ssid.size() > 32 || password.size() < 8 || password.size() > 63) {
        result.error = -EINVAL; return result;
    }
    for (unsigned char c : password) {
        if (c < 32 || c > 126 || c == '"' || c == '\\') {
            result.error = -EINVAL; return result;
        }
    }
    std::string reply;
    int ret = b.request("LIST_NETWORKS", reply, false);
    if (ret) { result.error = ret; return result; }
    std::istringstream lines(reply);
    std::string line;
    std::getline(lines, line);
    if (line.compare(0, 10, "network id") != 0) { result.error = -EPROTO; return result; }
    std::vector<Network> old;
    int current = -1;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        size_t first = line.find('\t'), last = line.rfind('\t');
        int id;
        if (first == std::string::npos || first == last || !number(line.substr(0, first), id)) {
            result.error = -EPROTO; return result;
        }
        if (old.size() >= 128) { result.error = -E2BIG; return result; }
        std::string flags = line.substr(last+1);
        // Persistent P2P profiles are not ordinary station networks.
        if (flags.find("P2P-PERSISTENT") != std::string::npos) continue;
        old.push_back({id, flags.find("DISABLED") == std::string::npos});
        if (flags.find("CURRENT") != std::string::npos) current = id;
    }
    ret = b.request("ADD_NETWORK", reply, false);
    int added = -1;
    if (ret || !number(reply, added)) {
        result.error = ret ? ret : -EPROTO;
        // An ADD timeout may have created a disabled profile. We cannot safely
        // guess its id and delete a network owned by somebody else.
        result.rollback_error = result.error;
        return result;
    }
    const std::string id = std::to_string(added);
    bool selected = false, begun = false;
    try {
        const char digits[] = "0123456789abcdef";
        std::string hex;
        for (unsigned char c : ssid) { hex += digits[c >> 4]; hex += digits[c & 15]; }
        ret = ok(b, "SET_NETWORK " + id + " ssid " + hex);
        if (!ret) ret = ok(b, "SET_NETWORK " + id + " psk \"" + password + "\"");
        if (!ret) ret = ok(b, "SET_NETWORK " + id + " key_mgmt WPA-PSK");
        if (!ret) ret = ok(b, "SET_NETWORK " + id + " proto RSN");
        if (!ret) ret = ok(b, "SET_NETWORK " + id + " scan_ssid 1");
        if (!ret) { ret = b.begin(); begun = !ret; }
        if (!ret) ret = b.before_select();
        if (!ret) {
            // A response timeout does not prove SELECT was not applied.
            selected = true;
            ret = ok(b, "SELECT_NETWORK " + id);
        }
        if (!ret) ret = b.wait_connected(added, false);
        if (!ret) ret = b.dhcp(false);
        if (!ret) ret = b.commit(false);
        if (!ret) { result.changed = true; return result; }
    } catch (...) { ret = -EFAULT; }
    result.error = ret;
    auto record = [&](int error) { if (error && !result.rollback_error) result.rollback_error = error; };
    // Never stop recovery just because the user closed the UI during a switch.
    // Keep the original failure separate from the rollback result.
    try {
        if (begun) {
            int stopped = b.before_rollback();
            record(stopped);
            // Do not select/remove an active profile while its lease hook can
            // still mutate wlan0. Leave the uncertainty marker for recovery.
            if (stopped) return result;
        }
        if (selected) {
            if (current >= 0) record(ok(b, "SELECT_NETWORK " + std::to_string(current), true));
            else record(ok(b, "DISCONNECT", true));
        }
        record(ok(b, "REMOVE_NETWORK " + id, true));
        if (selected) {
            for (const auto &n : old) {
                record(ok(b, (n.enabled ? "ENABLE_NETWORK " : "DISABLE_NETWORK ") +
                          std::to_string(n.id) + (n.enabled ? " no-connect" : ""), true));
            }
        }
        if (begun) {
            if (current >= 0) {
                int associated = b.wait_connected(current, true);
                record(associated);
                if (!associated) record(b.dhcp(true));
            } else record(b.restore_disconnected());
            if (!result.rollback_error) record(b.commit(true));
        }
    } catch (...) { record(-EFAULT); }
    return result;
}
