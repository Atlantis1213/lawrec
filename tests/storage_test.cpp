#include "lawrec_storage.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(setenv("LAWREC_RECORD_DIR", argv[1], 1) == 0);
    char first[512], second[512], final[512], text[4096];
    assert(lawrec_storage_check(nullptr) == -ENOENT);
    assert(mkdir(argv[1], 0700) == 0);
    assert(lawrec_storage_reserve(first, sizeof(first)) == 0);
    assert(lawrec_storage_reserve(second, sizeof(second)) == 0);
    char tiny[1];
    assert(lawrec_storage_reserve(tiny, sizeof(tiny)) == -ENAMETOOLONG);
    assert(strcmp(first, second) != 0);
    assert(lawrec_storage_list(text, sizeof(text)) == 0 && !text[0]);
    assert(lawrec_storage_reserve(nullptr, 16) == -EINVAL);
    assert(lawrec_storage_publish(nullptr, final, sizeof(final)) == -EINVAL);
    assert(lawrec_storage_publish("/tmp/outside.part", final, sizeof(final)) == -EINVAL);
    assert(lawrec_storage_publish(first, final, sizeof(final)) == -EINVAL);
    FILE *initial = fopen(first, "wb"); assert(initial);
    assert(fwrite("test", 1, 4, initial) == 4); assert(fclose(initial) == 0);
    assert(lawrec_storage_publish(first, final, sizeof(final)) == 0);
    initial = fopen(first, "wb"); assert(initial);
    assert(fwrite("replacement", 1, 11, initial) == 11); assert(fclose(initial) == 0);
    assert(lawrec_storage_publish(first, final, sizeof(final)) == -EEXIST);
    assert(unlink(first) == 0);
    assert(lawrec_storage_list(text, sizeof(text)) == 0 && strstr(text, ".mp4"));
    assert(lawrec_storage_delete("../escape.mp4") == -EINVAL);
    assert(lawrec_storage_open_recording("../escape.mp4") == -EINVAL);
    assert(lawrec_storage_open_recording("record.mp4.part") == -EINVAL);
    const char *final_name = strrchr(final, '/') + 1;
    int pinned = lawrec_storage_open_recording(final_name);
    assert(pinned >= 0);
    char link[512];
    snprintf(link, sizeof(link), "%s/lawrec_link.mp4", argv[1]);
    assert(symlink(final, link) == 0);
    assert(lawrec_storage_delete("lawrec_link.mp4") == -EINVAL);
    assert(lawrec_storage_open_recording("lawrec_link.mp4") == -ELOOP);
    assert(unlink(link) == 0);
    assert(lawrec_storage_delete(strrchr(second, '/') + 1) == -EINVAL);
    assert(lawrec_storage_delete(strrchr(final, '/') + 1) == 0);
    char data[4];
    assert(read(pinned, data, sizeof(data)) == sizeof(data) && !memcmp(data, "test", 4));
    close(pinned);
    assert(unlink(second) == 0);
    assert(chmod(argv[1], 0500) == 0);
    assert(lawrec_storage_reserve(first, sizeof(first)) == -EACCES);
    assert(chmod(argv[1], 0700) == 0);
    assert(rmdir(argv[1]) == 0);
    puts("storage: unique names, incomplete filtering, publication and deletion passed");
}
