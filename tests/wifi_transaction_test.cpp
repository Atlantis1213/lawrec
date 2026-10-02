#include "lawrec_wifi_transaction.h"
#include <cassert>
#include <cerrno>
#include <map>
#include <vector>
#include <stdexcept>
#include <cstdio>

struct Fake : LawrecWifiBackend {
    std::map<int, bool> enabled{{0,true}, {2,true}, {3,false}};
    int current = 0, fail_at = 0, calls = 0, fail_wait = 0, fail_dhcp = 0;
    int recovery_dhcp = 0;
    bool fail_remove = false, throw_set = false;
    int prepare_count = 0, prepare_error = 0;
    int begin_count = 0, begin_error = 0, stop_error = 0, recovery_error = 0;
    int commit_error = 0, recovery_commit_error = 0, commits = 0, recovery_stops = 0;
    int disconnected_recoveries = 0;
    bool uncertain = false, dhcp_running = true, throw_prepare = false;
    std::vector<std::string> commands;
    int request(const std::string &cmd, std::string &reply, bool recovery) override {
        commands.push_back(cmd);
        reply = "OK\n";
        if (cmd == "LIST_NETWORKS") {
            reply = "network id / ssid / bssid / flags\n0\told\tany\t";
            reply += current == 0 ? "[CURRENT]\n" : "\n";
            reply += "2\tbackup\tany\t\n3\tdisabled\tany\t[DISABLED]\n";
        } else if (cmd == "ADD_NETWORK") { enabled[7] = false; reply = "7\n"; }
        else if (cmd.compare(0, 12, "SET_NETWORK ") == 0) {
            if (throw_set) throw std::runtime_error("test");
        } else if (cmd.compare(0, 15, "SELECT_NETWORK ") == 0) {
            assert(!dhcp_running);
            current = std::stoi(cmd.substr(15));
            for (auto &n : enabled) n.second = n.first == current;
        } else if (cmd.compare(0, 15, "REMOVE_NETWORK ") == 0) {
            if (fail_remove) return -EIO;
            enabled.erase(std::stoi(cmd.substr(15)));
        } else if (cmd.compare(0, 15, "ENABLE_NETWORK ") == 0) enabled[std::stoi(cmd.substr(15))] = true;
        else if (cmd.compare(0, 16, "DISABLE_NETWORK ") == 0) enabled[std::stoi(cmd.substr(16))] = false;
        else if (cmd == "DISCONNECT") current = -1;
        else assert(!"unexpected command");
        // Even a failed response may follow an applied SELECT.
        if (!recovery && ++calls == fail_at) return -ETIMEDOUT;
        return 0;
    }
    int wait_connected(int id, bool recovery) override {
        assert(current == id);
        return recovery ? 0 : fail_wait;
    }
    int dhcp(bool recovery) override {
        dhcp_running = true;
        if (recovery) { ++recovery_dhcp; return recovery_error; }
        return fail_dhcp;
    }
    int begin() override {
        ++begin_count;
        if (begin_error) return begin_error;
        uncertain = true;
        return 0;
    }
    int before_select() override {
        ++prepare_count;
        assert(uncertain);
        dhcp_running = false; // SIGTERM can take effect even on a returned failure.
        if (throw_prepare) throw std::runtime_error("stop");
        return prepare_error;
    }
    int before_rollback() override {
        ++recovery_stops;
        assert(uncertain);
        if (stop_error) return stop_error;
        dhcp_running = false;
        return 0;
    }
    int restore_disconnected() override {
        ++disconnected_recoveries;
        assert(!dhcp_running && current == -1);
        return recovery_error;
    }
    int commit(bool recovery) override {
        assert(uncertain);
        ++commits;
        int ret = recovery ? recovery_commit_error : commit_error;
        if (!ret) uncertain = false;
        return ret;
    }
    void restored(int previous = 0) {
        assert(current == previous);
        assert(enabled.size() == 3 && enabled[0] && enabled[2] && !enabled[3]);
    }
};

int main() {
    Fake success;
    auto r = lawrec_wifi_connect(success, "Test AP", "pass1234");
    assert(!r.error && !r.rollback_error && r.changed && success.current == 7);
    assert(success.enabled[7] && !success.enabled[0]);
    assert(success.prepare_count == 1);
    assert(success.begin_count == 1 && success.commits == 1 && !success.uncertain);
    Fake prepare; prepare.prepare_error = -EBUSY;
    r = lawrec_wifi_connect(prepare, "Test AP", "pass1234");
    assert(r.error == -EBUSY && !r.changed && prepare.prepare_count == 1);
    assert(!prepare.uncertain && prepare.recovery_stops == 1 && prepare.recovery_dhcp == 1);
    prepare.restored();
    Fake invalid;
    r = lawrec_wifi_connect(invalid, "Test AP", "x\ninvalid");
    assert(r.error == -EINVAL && invalid.commands.empty());
    // Fail each setup operation and SELECT (LIST=1, ADD=2, SET=3..7, SELECT=8).
    for (int step = 3; step <= 8; ++step) {
        Fake f; f.fail_at = step;
        r = lawrec_wifi_connect(f, "Test AP", "pass1234");
        assert(r.error == -ETIMEDOUT && !r.rollback_error && !r.changed);
        assert(!f.uncertain && f.recovery_stops == (step == 8 ? 1 : 0));
        f.restored();
    }
    Fake associate; associate.fail_wait = -ETIMEDOUT;
    r = lawrec_wifi_connect(associate, "Test AP", "pass1234");
    assert(r.error == -ETIMEDOUT && !r.rollback_error && associate.recovery_dhcp == 1);
    associate.restored();
    Fake dhcp; dhcp.fail_dhcp = -ECHILD;
    r = lawrec_wifi_connect(dhcp, "Test AP", "pass1234");
    assert(r.error == -ECHILD && !r.rollback_error && dhcp.recovery_dhcp == 1);
    dhcp.restored();
    Fake cancelled; cancelled.fail_wait = -ECANCELED;
    r = lawrec_wifi_connect(cancelled, "Test AP", "pass1234");
    assert(r.error == -ECANCELED && !r.rollback_error); cancelled.restored();
    Fake cleanup; cleanup.fail_wait = -ETIMEDOUT; cleanup.fail_remove = true;
    r = lawrec_wifi_connect(cleanup, "Test AP", "pass1234");
    assert(r.error == -ETIMEDOUT && r.rollback_error == -EIO);
    assert(cleanup.uncertain && !cleanup.commits);
    Fake disconnected; disconnected.current = -1; disconnected.fail_wait = -ETIMEDOUT;
    r = lawrec_wifi_connect(disconnected, "Test AP", "pass1234");
    assert(r.error && !r.rollback_error && !disconnected.recovery_dhcp); disconnected.restored(-1);
    assert(disconnected.disconnected_recoveries == 1 && !disconnected.uncertain && !disconnected.dhcp_running);
    Fake exception; exception.throw_set = true;
    r = lawrec_wifi_connect(exception, "Test AP", "pass1234");
    assert(r.error == -EFAULT && !r.rollback_error); exception.restored();
    Fake begin; begin.begin_error = -EROFS;
    r = lawrec_wifi_connect(begin, "Test AP", "pass1234");
    assert(r.error == -EROFS && !r.rollback_error && !begin.prepare_count && !begin.recovery_stops);
    begin.restored();
    Fake commit; commit.commit_error = -EROFS;
    r = lawrec_wifi_connect(commit, "Test AP", "pass1234");
    assert(r.error == -EROFS && !r.rollback_error && !r.changed && commit.commits == 2 && !commit.uncertain);
    commit.restored();
    Fake stop; stop.fail_wait = -ETIMEDOUT; stop.stop_error = -EBUSY;
    r = lawrec_wifi_connect(stop, "Test AP", "pass1234");
    assert(r.error == -ETIMEDOUT && r.rollback_error == -EBUSY && stop.uncertain && stop.current == 7);
    assert(stop.enabled.count(7) && !stop.recovery_dhcp && !stop.commits);
    Fake recover; recover.fail_dhcp = -ECHILD; recover.recovery_error = -ETIMEDOUT;
    r = lawrec_wifi_connect(recover, "Test AP", "pass1234");
    assert(r.error == -ECHILD && r.rollback_error == -ETIMEDOUT && recover.uncertain && !recover.commits);
    recover.restored();
    Fake recovery_commit; recovery_commit.fail_wait = -ECANCELED; recovery_commit.recovery_commit_error = -EIO;
    r = lawrec_wifi_connect(recovery_commit, "Test AP", "pass1234");
    assert(r.error == -ECANCELED && r.rollback_error == -EIO && recovery_commit.uncertain);
    recovery_commit.restored();
    Fake prepare_exception; prepare_exception.throw_prepare = true;
    r = lawrec_wifi_connect(prepare_exception, "Test AP", "pass1234");
    assert(r.error == -EFAULT && !r.rollback_error && !prepare_exception.uncertain && prepare_exception.recovery_dhcp == 1);
    prepare_exception.restored();
    puts("wifi transaction: markers, partial stop, candidate hook exclusion, commit/rollback and exceptions passed");
}
