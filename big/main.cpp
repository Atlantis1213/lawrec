#include "config.h"
#include "protocol.h"
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--build-info") == 0) {
        std::printf("vision SDK skeleton: preview=%u rgb=%u encode=%u protocol=%u\n",
                    demo::preview_channel, demo::rgb_channel, demo::encode_channel, demo::version);
        return 0;
    }
    std::fprintf(stderr, "vision: camera/AI implementation pending; no hardware initialized\n");
    return 1;
}
