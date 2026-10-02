#include "lawrec_ipv4_apply.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

struct Backend : LawrecIpv4Backend {
    int fail_at = -1;
    unsigned steps = 0;
    unsigned probes = 0;
    int probe_error = 0;
    std::vector<std::vector<std::string>> commands;
    int run(const std::vector<std::string> &args) override {
        commands.push_back(args);
        return int(steps++) == fail_at ? -EIO : 0;
    }
    int dns(const lawrec_ipv4_settings &s) override {
        assert(!strcmp(s.dns1, "192.168.10.1"));
        return int(steps++) == fail_at ? -EROFS : 0;
    }
    int probe(const lawrec_ipv4_settings &s) override {
        assert(!s.dhcp && !strcmp(s.address, "192.168.10.74"));
        ++probes; return probe_error;
    }
};

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(mkdir(argv[1], 0700) == 0);
    std::string dir = argv[1], run = dir+"/run";
    assert(mkdir(run.c_str(), 0700) == 0);
    setenv("LAWREC_SETTINGS_DIR", dir.c_str(), 1);
    setenv("LAWREC_NETWORK_RUN_DIR", run.c_str(), 1);
    lawrec_ipv4_settings s;
    assert(lawrec_settings_ipv4_pending(&s) == 0 && s.dhcp == 1);
    assert(lawrec_settings_ipv4_active(&s) == 0 && s.dhcp == 1);
    assert(lawrec_settings_ipv4_boot(&s) == -ENOENT);
    s.dhcp = 0;
    strcpy(s.address, "192.168.10.74"); strcpy(s.gateway, "192.168.10.1");
    strcpy(s.dns1, "192.168.10.1"); strcpy(s.dns2, "1.1.1.1");
    assert(lawrec_settings_ipv4_validate(&s) == 0);
    assert(lawrec_settings_ipv4_save(&s) == 0);
    assert(lawrec_settings_ipv4_stage_boot() == 0);
    lawrec_ipv4_settings copy;
    assert(lawrec_settings_ipv4_pending(&copy) == 0 && !memcmp(&s, &copy, sizeof(s)));
    assert(lawrec_settings_ipv4_active(&copy) == 0 && copy.dhcp == 1);
    assert(lawrec_settings_ipv4_begin_apply() == 0);
    assert(lawrec_settings_ipv4_active(&copy) == -EIO);
    assert(lawrec_settings_ipv4_note_active(&s) == 0);
    assert(lawrec_settings_ipv4_active(&copy) == 0 && !copy.dhcp);
    lawrec_ipv4_settings dhcp = {1, 1, 24, {}, {}, {}, {}};
    assert(lawrec_settings_ipv4_save(&dhcp) == 0);
    assert(lawrec_settings_ipv4_boot(&copy) == 0 && !copy.dhcp); // Startup frozen before UI editing.
    assert(lawrec_settings_ipv4_active(&copy) == 0 && !copy.dhcp); // Draft does not change active.
    assert(lawrec_settings_ipv4_note_active(&dhcp) == 0);
    assert(lawrec_settings_ipv4_active(&copy) == 0 && copy.dhcp);
    const char *invalid_addresses[] = {"0.1.2.3", "127.0.0.1", "224.0.0.1", "255.255.255.255",
        "192.168.10.0", "192.168.10.255", "192.168.010.74", "bad;command"};
    for (const char *value : invalid_addresses) {
        copy = s; snprintf(copy.address, sizeof(copy.address), "%s", value);
        assert(lawrec_settings_ipv4_save(&copy) == -EINVAL);
    }
    copy = s; memset(copy.address, '1', sizeof(copy.address));
    assert(lawrec_settings_ipv4_save(&copy) == -EINVAL);
    copy = s; strcpy(copy.gateway, "192.168.11.1"); assert(lawrec_settings_ipv4_save(&copy) == -EINVAL);
    copy = s; strcpy(copy.gateway, s.address); assert(lawrec_settings_ipv4_save(&copy) == -EINVAL);
    copy = s; copy.dns1[0] = 0; assert(lawrec_settings_ipv4_save(&copy) == -EINVAL);
    copy = s; copy.prefix = 31; assert(lawrec_settings_ipv4_save(&copy) == -EINVAL);
    copy = s; copy.version = 2; assert(lawrec_settings_ipv4_save(&copy) == -EINVAL);
    assert(lawrec_settings_ipv4_save(nullptr) == -EINVAL);
    assert(lawrec_settings_ipv4_pending(nullptr) == -EINVAL);
    assert(lawrec_settings_ipv4_active(nullptr) == -EINVAL);

    Backend b;
    assert(lawrec_ipv4_apply_static(b, s) == 0 && b.steps == 6 && b.probes == 1);
    for (int error : {-EADDRINUSE, -EPERM, -ENETDOWN, -ECANCELED}) {
        Backend conflict; conflict.probe_error = error;
        assert(lawrec_ipv4_apply_static(conflict, s) == error);
        assert(conflict.probes == 1 && !conflict.steps && conflict.commands.empty());
    }
    assert((b.commands[3] == std::vector<std::string>{"/sbin/ip", "-4", "addr", "replace", "192.168.10.74/24", "dev", "wlan0"}));
    for (const auto &command : b.commands) {
        assert(command[0] == "/sbin/ip");
        bool iface = false;
        for (const auto &arg : command) { assert(arg != "eth0"); iface |= arg == "wlan0"; }
        assert(iface);
    }
    for (int step = 0; step < 6; ++step) {
        Backend failed; failed.fail_at = step;
        assert(lawrec_ipv4_apply_static(failed, s) == (step == 5 ? -EROFS : -EIO));
        assert(failed.steps == unsigned(step+1));
    }
    copy = s; copy.gateway[0] = 0;
    Backend local; assert(lawrec_ipv4_apply_static(local, copy) == 0 && local.steps == 5);
    Backend rejected; assert(lawrec_ipv4_apply_static(rejected, dhcp) == -EINVAL && !rejected.steps && !rejected.probes);
    std::string resolver = lawrec_ipv4_resolver_text(s,
        "nameserver 9.9.9.9 # wlan0\t\r\nsearch old.local # wlan0\n"
        "nameserver 8.8.8.8 # eth0\nsearch example.org\n");
    assert(resolver == "nameserver 192.168.10.1 # wlan0\nnameserver 1.1.1.1 # wlan0\n"
                       "nameserver 8.8.8.8 # eth0\nsearch example.org\n");
    copy = s; copy.dns2[0] = 0;
    assert(lawrec_ipv4_resolver_text(copy, "") == "nameserver 192.168.10.1 # wlan0\n");
    assert(lawrec_ipv4_lease_settings("192.168.10.74", "255.255.255.0", "192.168.10.1",
                                     "192.168.10.1 1.1.1.1", &copy) == 0);
    Backend renew;
    assert(lawrec_ipv4_apply_lease(renew, copy, true) == 0 && renew.steps == 5);
    for (const auto &command : renew.commands)
        assert(command.size() < 4 || command[2] != "addr" || command[3] != "flush");
    assert(lawrec_ipv4_lease_settings("192.168.10.74", "255.0.255.0", "", "", &copy) == -EINVAL);
    assert(lawrec_ipv4_lease_settings("192.168.10.74", "255.255.255.0", "192.168.10.1;rm", "", &copy) == -EINVAL);
    assert(lawrec_ipv4_lease_settings("192.168.10.74", "255.255.255.0", "", "1.1.1.1 bad", &copy) == -EINVAL);
    assert(lawrec_ipv4_lease_settings("192.168.10.74", "255.255.255.0", "", "", &copy) == 0);
    assert(lawrec_ipv4_resolver_text(copy, "nameserver 8.8.8.8 # eth0\n") == "nameserver 8.8.8.8 # eth0\n");

    std::string file = dir+"/lawrec-ipv4.conf";
    struct stat st{};
    assert(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    const char *invalid[] = {
        "version=2\ndhcp=1\nprefix=24\naddress=\ngateway=\ndns1=\ndns2=\n",
        "version=1\ndhcp=1\nprefix=24\naddress=\ngateway=\ndns1=\ndns2=\ndhcp=0\n",
        "version=1\ndhcp=1\nprefix=24\naddress=\ngateway=\ndns1=\n",
        "version=1\ndhcp=1\nprefix=24\naddress=1.1.1.1\ngateway=\ndns1=\ndns2=\n",
        "version=1\ndhcp=9999999999999999\nprefix=24\naddress=\ngateway=\ndns1=\ndns2=\n",
    };
    for (const char *text : invalid) {
        FILE *fp = fopen(file.c_str(), "w"); assert(fp); fputs(text, fp); fclose(fp);
        assert(lawrec_settings_ipv4_pending(&copy) == -EINVAL && copy.dhcp == 1);
    }
    unlink(file.c_str());
    assert(symlink("/dev/zero", file.c_str()) == 0);
    assert(lawrec_settings_ipv4_pending(&copy) == -ELOOP);
    unlink(file.c_str());
    assert(mkfifo(file.c_str(), 0600) == 0);
    assert(lawrec_settings_ipv4_pending(&copy) == -EINVAL);
    unlink(file.c_str());
    unlink((run+"/lawrec-ipv4.conf").c_str());
    unlink((run+"/lawrec-ipv4-boot.conf").c_str());
    assert(rmdir(run.c_str()) == 0 && rmdir(dir.c_str()) == 0);
    puts("ipv4: persistent/active policies, strict validation, incomplete boot guard, scoped commands and failures passed");
}
