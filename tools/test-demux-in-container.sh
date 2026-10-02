#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker is required'; exit 1; }
MP4="$SDK/src/common/cdk/user/middleware/mp4_format"
mkdir -p out/tests/demux-sdk
g++ -std=c++11 -Wall -Wextra -pthread -DLAWREC_DEMUX_MOCK -I "$MP4/include" \
    tests/demux_test.cpp little/src/playback/lawrec_demux.cpp -o out/tests/demux_mock
out/tests/demux_mock
# Compile the unmodified SDK C sources with assertions, without invoking SDK
# make or writing objects into its read-only source tree.
for source in "$MP4"/src/*.c "$MP4"/src/libmov/source/*.c "$MP4"/src/libflv/source/*.c; do
    object="out/tests/demux-sdk/$(basename "${source%.c}").o"
    gcc -std=gnu11 -pthread -I "$MP4/include" -I "$MP4/src/libmov/include" \
        -I "$MP4/src/libflv/include" -c "$source" -o "$object"
done
g++ -std=c++11 -Wall -Wextra -pthread -I "$MP4/include" \
    tests/demux_test.cpp little/src/playback/lawrec_demux.cpp \
    out/tests/demux-sdk/*.o -o out/tests/demux_sdk
out/tests/demux_sdk
