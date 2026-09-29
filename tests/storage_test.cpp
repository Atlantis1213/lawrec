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
    assert(lawrec_storage_publish(first, final, sizeof(final)) == 0);
    assert(lawrec_storage_list(text, sizeof(text)) == 0 && strstr(text, ".mp4"));
    assert(lawrec_storage_delete("../escape.mp4") == -EINVAL);
    char link[512];
    snprintf(link, sizeof(link), "%s/lawrec_link.mp4", argv[1]);
    assert(symlink(final, link) == 0);
    assert(lawrec_storage_delete("lawrec_link.mp4") == -EINVAL);
    assert(unlink(link) == 0);
    assert(lawrec_storage_delete(strrchr(second, '/') + 1) == -EINVAL);
    assert(lawrec_storage_delete(strrchr(final, '/') + 1) == 0);
    assert(unlink(second) == 0);
    assert(chmod(argv[1], 0500) == 0);
    assert(lawrec_storage_reserve(first, sizeof(first)) == -EACCES);
    assert(chmod(argv[1], 0700) == 0);
    assert(rmdir(argv[1]) == 0);
    puts("storage: unique names, incomplete filtering, publication and deletion passed");
}
