#include "lawrec_settings.h"
#include "lawrec_storage.h"
#include "lawrec_config.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 4 || argc == 5);
    setenv("LAWREC_SETTINGS_DIR", argv[1], 1);
    if (argc >= 4) {
        char path[LAWREC_RECORD_DIR_MAX+1];
        int error = atoi(argv[3]);
        assert(lawrec_settings_record_dir_current(path, sizeof(path)) == error);
        assert(!strcmp(path, argv[2]));
        if (error) {
            char file[128], list[128];
            assert(lawrec_storage_check(nullptr) == error);
            assert(lawrec_storage_list(list, sizeof(list)) == error);
            assert(lawrec_storage_reserve(file, sizeof(file)) == error);
            assert(lawrec_storage_open_recording("lawrec_test.mp4") == error);
            assert(lawrec_storage_delete("lawrec_test.mp4") == error);
        } else if (argc == 5) {
            char file[128];
            assert(lawrec_storage_reserve(file, sizeof(file)) == 0);
            assert(strlen(file) < 128);
            assert(unlink(file) == 0);
        }
        return 0;
    }
    unsetenv("LAWREC_RECORD_DIR");
    assert(mkdir(argv[1], 0700) == 0);
    std::string root = argv[1], config = root+"/lawrec-record-dir", records = root+"/records";
    assert(records.size() <= LAWREC_RECORD_DIR_MAX);
    assert(mkdir(records.c_str(), 0700) == 0);
    auto check_child = [&](const std::string &expected, int error = 0, bool reserve = false) {
        std::string number = std::to_string(error);
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            execl(argv[0], argv[0], argv[1], expected.c_str(), number.c_str(),
                  reserve ? "reserve" : nullptr, (char *)nullptr);
            _exit(127);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    };
    auto write_config = [&](const std::string &value) {
        FILE *fp = fopen(config.c_str(), "wb"); assert(fp);
        assert(fwrite(value.data(), 1, value.size(), fp) == value.size());
        assert(fclose(fp) == 0);
    };
    char current[LAWREC_RECORD_DIR_MAX+1], pending[LAWREC_RECORD_DIR_MAX+1];
    assert(lawrec_settings_record_dir_pending(pending, sizeof(pending)) == 0);
    assert(!strcmp(pending, LAWREC_RECORD_DEFAULT_OUTPUT_DIR));
    assert(lawrec_settings_record_dir_current(current, sizeof(current)) == 0);
    assert(lawrec_settings_record_dir_save(records.c_str()) == 0);
    assert(lawrec_settings_record_dir_pending(pending, sizeof(pending)) == 0 && records == pending);
    assert(lawrec_settings_record_dir_current(current, sizeof(current)) == 0 &&
           !strcmp(current, LAWREC_RECORD_DEFAULT_OUTPUT_DIR));
    check_child(records, 0, true);
    struct stat st{};
    assert(stat(config.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    const char *invalid[] = {"", "relative", "/", "/a/", "/a//b", "/a/../b", "/a/./b",
                            "/a\nb", "/with space", "/bad;command", "/bad\\path"};
    for (const char *value : invalid) {
        assert(lawrec_settings_record_dir_save(value) == -EINVAL);
        assert(lawrec_settings_record_dir_pending(pending, sizeof(pending)) == 0 && records == pending);
    }
    assert(lawrec_settings_record_dir_save(nullptr) == -EINVAL);
    assert(lawrec_settings_record_dir_pending(nullptr, sizeof(pending)) == -EINVAL);
    char tiny[1];
    assert(lawrec_settings_record_dir_pending(tiny, sizeof(tiny)) == -ENAMETOOLONG && !tiny[0]);
    for (const std::string &value : {records+"\n/extra\n", records+"\n\n", "/"+std::string(80, 'a'),
                                   records+std::string("\0trailing", 9)}) {
        write_config(value);
        assert(lawrec_settings_record_dir_pending(pending, sizeof(pending)) == -EINVAL);
        check_child(LAWREC_RECORD_DEFAULT_OUTPUT_DIR, -EINVAL);
    }
    write_config(records+"\r\n"); check_child(records);
    assert(unlink(config.c_str()) == 0);
    assert(symlink("/dev/zero", config.c_str()) == 0);
    check_child(LAWREC_RECORD_DEFAULT_OUTPUT_DIR, -ELOOP);
    assert(unlink(config.c_str()) == 0);
    assert(mkfifo(config.c_str(), 0600) == 0);
    check_child(LAWREC_RECORD_DEFAULT_OUTPUT_DIR, -EINVAL);
    assert(unlink(config.c_str()) == 0);
    assert(lawrec_settings_record_dir_save(records.c_str()) == 0);
    setenv("LAWREC_RECORD_DIR", root.c_str(), 1); check_child(root);
    setenv("LAWREC_RECORD_DIR", "relative", 1); check_child(LAWREC_RECORD_DEFAULT_OUTPUT_DIR, -EINVAL);
    unsetenv("LAWREC_RECORD_DIR");

    uint64_t free_bytes;
    assert(lawrec_storage_validate_dir(records.c_str(), &free_bytes) == 0 && free_bytes >= 128ULL*1024*1024);
    DIR *dir = opendir(records.c_str()); assert(dir);
    while (dirent *e = readdir(dir)) assert(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."));
    closedir(dir); // Probe was removed and no user recordings were created.
    assert(lawrec_storage_validate_dir(nullptr, nullptr) == -EINVAL);
    assert(lawrec_storage_validate_dir((root+"/missing").c_str(), nullptr) == -ENOENT);
    assert(lawrec_storage_validate_dir(config.c_str(), nullptr) == -ENOTDIR);
    assert(lawrec_storage_validate_dir("/dev/shm", nullptr) == -ENODEV);
    std::string link = root+"/link";
    assert(symlink(records.c_str(), link.c_str()) == 0);
    assert(lawrec_storage_validate_dir(link.c_str(), nullptr) < 0);
    assert(lawrec_storage_validate_dir((link+"/nested").c_str(), nullptr) < 0);
    assert(unlink(link.c_str()) == 0);
    assert(chmod(records.c_str(), 0500) == 0);
    assert(lawrec_storage_validate_dir(records.c_str(), nullptr) == -EACCES);
    assert(chmod(records.c_str(), 0700) == 0);
    std::string maximum = root+"/"+std::string(LAWREC_RECORD_DIR_MAX-root.size()-1, 'a');
    assert(maximum.size() == LAWREC_RECORD_DIR_MAX && mkdir(maximum.c_str(), 0700) == 0);
    assert(lawrec_settings_record_dir_save(maximum.c_str()) == 0);
    check_child(maximum, 0, true); // Generated pathname fits the SDK's 128 bytes.
    assert(rmdir(maximum.c_str()) == 0);
    assert(unlink(config.c_str()) == 0 && rmdir(records.c_str()) == 0 && rmdir(root.c_str()) == 0);
    puts("record directory: strict legacy read/save, immutable snapshot/restart, fail-closed config, persistent write probe, symlinks and pathname bound passed");
}
