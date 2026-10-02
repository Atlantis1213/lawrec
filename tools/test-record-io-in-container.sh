#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker is required'; exit 1; }
MP4="$SDK/src/common/cdk/user/middleware/mp4_format"
DEST=out/tests/record-io
mkdir -p "$DEST/sdk"
cmake -DLAWREC_MP4_INPUT="$MP4/src/mp4_format.c" \
    -DLAWREC_MP4_OUTPUT="$PWD/$DEST/mp4_format.c" \
    -P little/src/record/prepare_mp4.cmake
# Native build of the same adapted wrapper and unchanged SDK format engines.
for source in "$DEST/mp4_format.c" "$DEST/mov-writer.c" "$DEST/mov-stts.c" "$DEST/mov-elst.c" "$MP4"/src/libmov/source/*.c "$MP4"/src/libflv/source/*.c; do
    case "$source" in "$MP4/src/libmov/source/mov-writer.c"|"$MP4/src/libmov/source/mov-stts.c"|"$MP4/src/libmov/source/mov-elst.c") continue ;; esac
    gcc -std=gnu11 -pthread -I little/src/record/include -I "$MP4/include" -I "$MP4/src/libmov/include" \
        -I "$MP4/src/libflv/include" -I "$MP4/src/libmov/source" -c "$source" -o "$DEST/sdk/$(basename "${source%.c}").o"
done
g++ -std=c++14 -Wall -Wextra -pthread -I "$MP4/include" -I little/src/record/include \
    tests/mp4_io_test.cpp "$DEST"/sdk/*.o \
    -Wl,--wrap=fwrite -Wl,--wrap=fread -Wl,--wrap=fseek -Wl,--wrap=fflush -Wl,--wrap=fclose -Wl,--wrap=ftell \
    -Wl,--wrap=calloc -Wl,--wrap=free \
    -o "$DEST/mp4_io_test"
"$DEST/mp4_io_test" "$PWD/$DEST/io.part"
g++ -std=c++14 -Wall -Wextra -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I little/src/common -I little/src/record/include -I "$MP4/include" \
    -I "$SDK/src/common/cdk/user/mapi/include" -I "$SDK/src/common/cdk/user/mapi/include/api" \
    -I "$SDK/src/common/cdk/user/mapi/include/comm" -I "$SDK/src/big/mpp/include" \
    -I "$SDK/src/big/mpp/include/comm" -I "$SDK/src/big/mpp/userapps/api" \
    tests/record_segment_test.cpp -o "$DEST/record_segment_test"
"$DEST/record_segment_test"
g++ -std=c++14 -Wall -Wextra -pthread -I little/src/common \
    tests/frame_queue_test.cpp -o "$DEST/frame_queue_test"
"$DEST/frame_queue_test"
g++ -std=c++14 -Wall -Wextra -pthread -I little/src/common \
    tests/media_diagnostics_test.cpp -o "$DEST/media_diagnostics_test"
"$DEST/media_diagnostics_test"
g++ -std=c++14 -Wall -Wextra -pthread \
    -I little/src/common -I "$SDK/src/common/cdk/user/mapi/include" \
    -I "$SDK/src/common/cdk/user/mapi/include/api" -I "$SDK/src/common/cdk/user/mapi/include/comm" \
    -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" \
    -I "$SDK/src/big/mpp/userapps/api" \
    tests/audio_test.cpp -o "$DEST/audio_test"
"$DEST/audio_test"
