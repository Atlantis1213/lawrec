#include "lawrec_settings.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <initializer_list>

int main(int argc, char **argv)
{
    assert(argc >= 2);
    setenv("LAWREC_SETTINGS_DIR", argv[1], 1);
    if (argc == 7) {
        assert(lawrec_settings_port() == atoi(argv[2]));
        assert(lawrec_settings_bitrate() == atoi(argv[3]));
        assert(lawrec_settings_segment_seconds() == atoi(argv[4]));
        assert(lawrec_settings_frame_rate() == atoi(argv[5]));
        assert(lawrec_settings_audio_enabled() == atoi(argv[6]));
        return 0;
    }
    assert(mkdir(argv[1], 0700) == 0);
    assert(lawrec_settings_port() == 8554);
    assert(lawrec_settings_frame_rate() == 30);
    assert(lawrec_settings_save_port(0) == -EINVAL);
    assert(lawrec_settings_save_port(65536) == -EINVAL);
    assert(lawrec_settings_save_port(9554) == 0);
    assert(lawrec_settings_port() == 8554);
    auto check_child = [&](const char *expected, const char *bitrate = "4000", const char *segment = "0",
                           const char *fps = "30", const char *audio = "0") {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) { execl(argv[0], argv[0], argv[1], expected, bitrate, segment, fps, audio, (char *)nullptr); _exit(127); }
        int status;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    };
    check_child("9554");
    lawrec_media_settings pending;
    assert(lawrec_settings_media_pending(&pending) == 0);
    assert(pending.version == LAWREC_MEDIA_SETTINGS_VERSION && pending.rtsp_port == 9554 &&
           pending.video_bitrate_kbps == 4000 && pending.video_frame_rate == 30);
    pending.rtsp_port = 10554; pending.video_bitrate_kbps = 6000;
    pending.record_segment_seconds = 60;
    pending.audio_enabled = 1;
    pending.video_frame_rate = 15;
    assert(lawrec_settings_media_save(&pending) == 0);
    check_child("10554", "6000", "60", "15", "1");
    assert(lawrec_settings_port() == 8554 && lawrec_settings_bitrate() == 4000);
    assert(lawrec_settings_frame_rate() == 30);
    assert(lawrec_settings_save_port(9554) == 0);
    assert(lawrec_settings_media_pending(&pending) == 0 && pending.audio_enabled == 1 && pending.video_frame_rate == 15);
    assert(lawrec_settings_audio_enabled() == 0);
    pending.audio_enabled = 2;
    assert(lawrec_settings_media_save(&pending) == -EINVAL);
    pending.audio_enabled = 1;
    check_child("9554", "6000", "60", "15", "1");
    pending.version = 3;
    assert(lawrec_settings_media_save(&pending) == -EINVAL);
    pending.version = 1;
    assert(lawrec_settings_media_save(&pending) == -EINVAL);
    pending.version = LAWREC_MEDIA_SETTINGS_VERSION; pending.video_bitrate_kbps = 999;
    assert(lawrec_settings_media_save(&pending) == -EINVAL);
    pending.video_bitrate_kbps = 8001;
    assert(lawrec_settings_media_save(&pending) == -EINVAL);
    assert(lawrec_settings_media_save(nullptr) == -EINVAL);
    pending.video_bitrate_kbps = 4000; pending.record_segment_seconds = 59;
    assert(lawrec_settings_media_save(&pending) == -EINVAL);
    pending.record_segment_seconds = 3601;
    assert(lawrec_settings_media_save(&pending) == -EINVAL);
    pending.record_segment_seconds = 60;
    for (unsigned fps : {0u, 1u, 14u, 16u, 25u, 31u, 60u, 0xffffffffu}) {
        pending.video_frame_rate = fps;
        assert(lawrec_settings_media_save(&pending) == -EINVAL);
    }
    check_child("9554", "6000", "60", "15", "1");
    char path[1024];
    snprintf(path, sizeof(path), "%s/lawrec-media.conf", argv[1]);
    FILE *fp = fopen(path, "w"); assert(fp);
    fputs("9554garbage\n", fp); fclose(fp);
    check_child("8554");
    assert(lawrec_settings_media_pending(&pending) == -EINVAL);
    assert(pending.rtsp_port == 8554 && pending.video_bitrate_kbps == 4000);
    assert(lawrec_settings_save_port(9555) == -EINVAL);
    const char *invalid[] = {
        "version=2\nrtsp_port=9554\nvideo_bitrate_kbps=4000\n",
        "version=1\nrtsp_port=9554\nvideo_bitrate_kbps=4000\nrtsp_port=9666\n",
        "version=1\nrtsp_port=9554\n",
        "version=1\nrtsp_port=9554\nvideo_bitrate_kbps=4000\nunknown=1\n",
        "version=1\nrtsp_port=99999999999999999999999\nvideo_bitrate_kbps=4000\n",
        "version=1\nrtsp_port=9554\nvideo_bitrate_kbps=4000\nvideo_frame_rate=15\n",
        "version=2\nrtsp_port=9554\nvideo_bitrate_kbps=4000\nrecord_segment_seconds=0\naudio_enabled=0\nvideo_frame_rate=60\n",
        "version=2\nrtsp_port=9554\nvideo_bitrate_kbps=4000\nrecord_segment_seconds=0\naudio_enabled=0\nvideo_frame_rate=15\nvideo_frame_rate=30\n",
        "version=3\nrtsp_port=9554\nvideo_bitrate_kbps=4000\nrecord_segment_seconds=0\naudio_enabled=0\nvideo_frame_rate=30\n",
    };
    for (const char *text : invalid) {
        fp = fopen(path, "w"); assert(fp); fputs(text, fp); fclose(fp);
        assert(lawrec_settings_media_pending(&pending) == -EINVAL);
        check_child("8554");
    }
    const char *legacy = "version=1\nrtsp_port=9554\nvideo_bitrate_kbps=4000\n";
    fp = fopen(path, "w"); assert(fp); fputs(legacy, fp); fclose(fp);
    assert(lawrec_settings_media_pending(&pending) == 0 && pending.version == LAWREC_MEDIA_SETTINGS_VERSION);
    assert(pending.video_frame_rate == 30 && !pending.audio_enabled && !pending.record_segment_seconds);
    check_child("9554");
    fp = fopen(path, "r"); assert(fp); char original[256]{};
    assert(fread(original, 1, sizeof(original)-1, fp) > 0); fclose(fp);
    assert(!strcmp(original, legacy)); // Reading v1 does not rewrite it.
    legacy = "version=1\nrtsp_port=9554\nvideo_bitrate_kbps=6000\nrecord_segment_seconds=180\naudio_enabled=1\n";
    fp = fopen(path, "w"); assert(fp); fputs(legacy, fp); fclose(fp);
    check_child("9554", "6000", "180", "30", "1");
    assert(lawrec_settings_media_pending(&pending) == 0 && pending.video_frame_rate == 30);
    pending.video_frame_rate = 15;
    assert(lawrec_settings_media_save(&pending) == 0);
    check_child("9554", "6000", "180", "15", "1");
    unlink(path);
    assert(symlink("/dev/zero", path) == 0);
    check_child("8554");
    unlink(path);
    snprintf(path, sizeof(path), "%s/lawrec-rtsp-port", argv[1]);
    fp = fopen(path, "w"); assert(fp); fputs("9666\n", fp); fclose(fp);
    check_child("9666");
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
    puts("settings: v1/v2 migration, FPS persistence/validation, immutable snapshot, port-only preservation and WiFi validation passed");
}
