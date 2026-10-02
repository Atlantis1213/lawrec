#include "lawrec_ipv4_transaction.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

struct Backend : LawrecIpv4TransactionBackend {
    std::vector<std::string> calls;
    std::string fail, rollback_fail, throwing;
    bool recovery = false, uncertain = false;
    int error_code = -EIO;
    int step(const std::string &name) {
        calls.push_back(name);
        if (name == throwing) throw std::runtime_error("injected");
        if (name == fail || name == rollback_fail) return error_code;
        return 0;
    }
    int preflight(const lawrec_ipv4_settings &s) override {
        assert(s.version == 1); return step("preflight");
    }
    int begin() override {
        int ret = step("begin");
        if (!ret) uncertain = true;
        return ret;
    }
    int stop_dhcp() override {
        recovery = calls.size() > 2;
        return step(recovery ? "restore-stop" : "stop");
    }
    int apply(const lawrec_ipv4_settings &s, bool restore) override {
        recovery = restore;
        return step(std::string(restore ? "restore-" : "apply-") + (s.dhcp ? "dhcp" : "static"));
    }
    int commit(const lawrec_ipv4_settings &s) override {
        int ret = step(std::string(recovery ? "restore-commit-" : "commit-") + (s.dhcp ? "dhcp" : "static"));
        if (!ret) uncertain = false;
        return ret;
    }
};

int main() {
    lawrec_ipv4_settings dhcp{1, 1, 24, {}, {}, {}, {}};
    lawrec_ipv4_settings fixed{1, 0, 24, "192.168.10.74", "192.168.10.1", "1.1.1.1", ""};
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        auto previous = reverse ? fixed : dhcp;
        auto desired = reverse ? dhcp : fixed;
        const std::string mode = desired.dhcp ? "dhcp" : "static";
        const std::string old = previous.dhcp ? "dhcp" : "static";
        Backend ok;
        auto result = lawrec_ipv4_switch(ok, previous, desired);
        assert(!result.error && !result.rollback_error && result.changed && !ok.uncertain);
        assert((ok.calls == std::vector<std::string>{"preflight", "begin", "stop", "apply-"+mode, "commit-"+mode}));
        for (const auto &stage : std::vector<std::string>{"preflight", "begin", "stop", "apply-"+mode, "commit-"+mode}) {
            Backend failed; failed.fail = stage;
            result = lawrec_ipv4_switch(failed, previous, desired);
            assert(result.error == -EIO && !result.rollback_error && !failed.uncertain);
            if (stage == "preflight" || stage == "begin") assert(!result.changed);
            else {
                assert(result.changed);
                assert(failed.calls.back() == "restore-commit-"+old);
            }
        }
        for (const auto &stage : std::vector<std::string>{"restore-stop", "restore-"+old, "restore-commit-"+old}) {
            Backend failed; failed.fail = "apply-"+mode; failed.rollback_fail = stage;
            result = lawrec_ipv4_switch(failed, previous, desired);
            assert(result.error == -EIO && result.rollback_error == -EIO && failed.uncertain);
            assert(failed.calls.back() == stage);
        }
        Backend exception; exception.throwing = "apply-"+mode;
        result = lawrec_ipv4_switch(exception, previous, desired);
        assert(result.error == -EFAULT && !result.rollback_error && !exception.uncertain);
        Backend cancelled; cancelled.fail = "apply-"+mode; cancelled.error_code = -ECANCELED;
        result = lawrec_ipv4_switch(cancelled, previous, desired);
        assert(result.error == -ECANCELED && !result.rollback_error && !cancelled.uncertain);
        assert(cancelled.calls.back() == "restore-commit-"+old);
        Backend conflict; conflict.fail = "preflight"; conflict.error_code = -EADDRINUSE;
        result = lawrec_ipv4_switch(conflict, previous, desired);
        assert(result.error == -EADDRINUSE && !result.changed && !result.rollback_error && !conflict.uncertain);
        assert((conflict.calls == std::vector<std::string>{"preflight"}));
    }
    fixed.prefix = 31;
    Backend invalid;
    auto result = lawrec_ipv4_switch(invalid, dhcp, fixed);
    assert(result.error == -EINVAL && invalid.calls.empty());
    puts("ipv4 transaction: DHCP/static, preflight, apply/commit failure, rollback failure and exception passed; no network changes");
}
