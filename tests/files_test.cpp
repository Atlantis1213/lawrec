// Real storage scan/delete APIs on an isolated Docker directory; no codec or UI mocks.
#include "lawrec_storage.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

static void write_file(const std::string &path, const char *data)
{
    FILE *file = fopen(path.c_str(), "wb"); assert(file);
    assert(fwrite(data, 1, strlen(data), file) == strlen(data));
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && setenv("LAWREC_RECORD_DIR", argv[1], 1) == 0);
    const std::string root = argv[1];
    assert(mkdir(root.c_str(), 0700) == 0);
    lawrec_recording_entry page[7]{}, first[7]{};
    size_t count = 99; int more = 99;
    assert(!lawrec_storage_page(nullptr, 0, page, 7, &count, &more) && !count && !more);
    assert(lawrec_storage_page(nullptr, 1, page, 7, &count, &more) == -EINVAL);
    assert(lawrec_storage_page("../escape.mp4", 0, page, 7, &count, &more) == -EINVAL);
    assert(lawrec_storage_page(nullptr, 0, page, 33, &count, &more) == -EINVAL);
    // More than the old 1000-file cutoff, created in ascending directory order.
    const int total = 1005;
    for (int i = 0; i < total; ++i) {
        char name[48]; snprintf(name, sizeof(name), "lawrec_%04d.mp4", i);
        write_file(root + "/" + name, "x");
    }
    const std::string long_name = "lawrec_" + std::string(130, 'a') + ".mp4";
    const char *invalid[] = {"lawrec_zero.mp4", "lawrec_ghost.part", "other.mp4", "lawrec_new\nline.mp4", "lawrec_back\\slash.mp4"};
    for (const char *name : invalid) write_file(root + "/" + name, !strcmp(name, "lawrec_zero.mp4") ? "" : "x");
    write_file(root + "/" + long_name, "x");
    assert(symlink("lawrec_1004.mp4", (root + "/lawrec_symlink.mp4").c_str()) == 0);
    assert(mkdir((root + "/lawrec_directory.mp4").c_str(), 0700) == 0);
    assert(!lawrec_storage_page(nullptr, 0, first, 7, &count, &more) && count == 7 && more);
    char legacy[4096];
    assert(lawrec_storage_list(legacy, sizeof(legacy)) == -ENOSPC);
    assert(!strcmp(first[0].name, "lawrec_1004.mp4") && !strcmp(first[6].name, "lawrec_0998.mp4"));
    assert(!lawrec_storage_page(first[6].name, 0, page, 7, &count, &more) && count == 7 && more);
    assert(!strcmp(page[0].name, "lawrec_0997.mp4"));
    std::string anchor = page[0].name;
    assert(!lawrec_storage_page(anchor.c_str(), 1, page, 7, &count, &more) && count == 7 && !more);
    for (size_t i = 0; i < count; ++i) assert(!strcmp(page[i].name, first[i].name));
    int expected = total - 1, visited = 0;
    anchor.clear();
    do {
        assert(!lawrec_storage_page(anchor.empty() ? nullptr : anchor.c_str(), 0, page, 7, &count, &more));
        assert(count && count <= 7);
        for (size_t i = 0; i < count; ++i) {
            char name[48]; snprintf(name, sizeof(name), "lawrec_%04d.mp4", expected--);
            assert(!strcmp(page[i].name, name) && page[i].bytes == 1);
            ++visited;
        }
        anchor = page[count - 1].name;
    } while (more);
    assert(visited == total && expected == -1);
    assert(!lawrec_storage_page(anchor.c_str(), 1, page, 7, &count, &more) && count == 7 && more);
    assert(!lawrec_storage_page(nullptr, 0, page, 7, &count, &more));
    const auto confirmed = page[0];
    write_file(root + "/" + confirmed.name, "modified");
    assert(lawrec_storage_delete_matching(&confirmed) == -ESTALE);
    assert(access((root + "/" + confirmed.name).c_str(), F_OK) == 0);
    assert(!lawrec_storage_page(nullptr, 0, page, 7, &count, &more));
    const auto replacement_snapshot = page[0];
    assert(rename((root + "/" + confirmed.name).c_str(), (root + "/saved.part").c_str()) == 0);
    write_file(root + "/" + confirmed.name, "modified");
    assert(lawrec_storage_delete_matching(&replacement_snapshot) == -ESTALE);
    assert(!lawrec_storage_page(nullptr, 0, page, 7, &count, &more));
    assert(!lawrec_storage_delete_matching(&page[0]));
    for (int i = 0; i < total - 1; ++i) {
        char name[48]; snprintf(name, sizeof(name), "lawrec_%04d.mp4", i);
        assert(unlink((root + "/" + name).c_str()) == 0);
    }
    for (const char *name : invalid) assert(unlink((root + "/" + name).c_str()) == 0);
    assert(unlink((root + "/" + long_name).c_str()) == 0);
    assert(unlink((root + "/lawrec_symlink.mp4").c_str()) == 0);
    assert(unlink((root + "/saved.part").c_str()) == 0);
    assert(rmdir((root + "/lawrec_directory.mp4").c_str()) == 0 && rmdir(root.c_str()) == 0);
    puts("Files: 1005-file forward/backward pages, unsafe/unfinished filtering and confirmed identity deletion passed");
}
