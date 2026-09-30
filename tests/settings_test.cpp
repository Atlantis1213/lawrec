#include "lawrec_settings.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
    assert(argc >= 2);
    setenv("LAWREC_SETTINGS_DIR", argv[1], 1);
    if (argc == 3) {
        assert(lawrec_settings_port() == atoi(argv[2]));
        return 0;
    }
    assert(mkdir(argv[1], 0700) == 0);
    assert(lawrec_settings_port() == 8554);
    assert(lawrec_settings_save_port(0) == -EINVAL);
    assert(lawrec_settings_save_port(65536) == -EINVAL);
    assert(lawrec_settings_save_port(9554) == 0);
    assert(lawrec_settings_port() == 8554);
    auto check_child = [&](const char *expected) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) { execl(argv[0], argv[0], argv[1], expected, (char *)nullptr); _exit(127); }
        int status;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    };
    check_child("9554");
    char path[1024];
    snprintf(path, sizeof(path), "%s/lawrec-rtsp-port", argv[1]);
    FILE *fp = fopen(path, "w"); assert(fp);
    fputs("9554garbage\n", fp); fclose(fp);
    check_child("8554");
    unlink(path);
    assert(symlink("/dev/zero", path) == 0);
    check_child("8554");
    unlink(path);
    char addresses[4096];
    assert(lawrec_settings_save_wifi("hotspot", "valid123") == 0);
    snprintf(path, sizeof(path), "%s/wpa_supplicant.conf", argv[1]);
    struct stat st{};
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    fp = fopen(path, "r"); assert(fp);
    char config[1024] = {};
    assert(fread(config, 1, sizeof(config)-1, fp) > 0); fclose(fp);
    assert(strstr(config, "ssid=686f7473706f74\n"));
    assert(strstr(config, "psk=\"valid123\"\n"));
    assert(lawrec_settings_save_wifi("", "valid123") == -EINVAL);
    assert(lawrec_settings_save_wifi("hotspot", "short") == -EINVAL);
    assert(lawrec_settings_save_wifi("hotspot", "injection\npass") == -EINVAL);
    assert(lawrec_settings_save_wifi("hotspot", "quote\"123") == -EINVAL);
    assert(lawrec_settings_save_wifi("hotspot", "slash\\123") == -EINVAL);
    char unchanged[1024] = {};
    fp = fopen(path, "r"); assert(fp);
    assert(fread(unchanged, 1, sizeof(unchanged)-1, fp) > 0); fclose(fp);
    assert(!strcmp(config, unchanged));
    unlink(path);
    assert(lawrec_network_addresses(nullptr, 10) == -EINVAL);
    assert(lawrec_network_addresses(addresses, sizeof(addresses)) == 0);
    assert(strlen(addresses));
    rmdir(argv[1]);
    puts("settings tests passed");
}
