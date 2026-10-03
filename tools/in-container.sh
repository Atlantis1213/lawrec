#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker required' >&2; exit 1; }
SDK=$K230_SDK_ROOT
BR="$SDK/output/$LAWREC_BOARD/little/buildroot-ext"
export PATH="$SDK/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin:$PATH"
if [ "$1" = verify ]; then
    sha256sum -c patches/frozen.sha256
    cmp patches/st7701.baseline.c "$SDK/src/big/mpp/kernel/connector/src/st7701.c"
    for binary in out/big/vision.elf out/little/media_service out/little/demo_ui \
                  out/little/liblvgl.so out/little/liblv_drivers.so; do
        readelf -h "$binary" | grep -q 'Machine:.*RISC-V'
        readelf -h "$binary" | grep -q 'Class:.*ELF64'
        readelf -h "$binary" | grep -q 'Data:.*little endian'
        sha256sum "$binary"
    done
    echo 'ELF header predicates/frozen bytes passed; any SDK section warnings above remain unresolved, no loader/hardware acceptance'
fi
case "$1" in
big|all)
    cmake -S big -B out/big
    cmake --build out/big -j "$LAWREC_JOBS"
    ;;
esac
case "$1" in
little|all)
    export LVGL_ROOT="${LVGL_ROOT:-$BR/build/lawrec/thirdlib/lvgl}"
    cmake -S little -B out/little -DCMAKE_TOOLCHAIN_FILE="$BR/host/share/buildroot/toolchainfile.cmake"
    cmake --build out/little -j "$LAWREC_JOBS"
    ;;
esac
if [ "$1" = test ]; then
    mkdir -p out/tests
    c++ -std=c++17 -Wall -Wextra -Werror -Icommon tests/protocol_test.cpp -o out/tests/protocol_test
    c++ -std=c++17 -Wall -Wextra -Werror -DDEMO_SOCKET_FIXTURE=1 -Icommon little/media/main.cpp -o out/tests/mock_service
    c++ -std=c++17 -Wall -Wextra -Werror -Icommon tests/socket_test.cpp common/socket.cpp -o out/tests/socket_test
    out/tests/protocol_test
    out/tests/socket_test out/tests/mock_service
    MPP="$SDK/src/big/mpp"
    c++ -std=c++17 -Wall -Wextra -Werror -Icommon -Ibig -I"$MPP/include" \
        -I"$MPP/include/comm" -I"$MPP/userapps/api" \
        tests/camera_test.cpp big/camera.cpp -o out/tests/camera_test
    out/tests/camera_test
    c++ -std=c++17 -Wall -Wextra -Werror -Icommon tests/faces_test.cpp common/faces.cpp \
        "$SDK/src/big/nncase/examples/image_face_detect/anchors_320.cc" -o out/tests/faces_test
    out/tests/faces_test
    c++ -std=c++17 -Wall -Wextra -Werror -pthread -Icommon -Ibig -I"$MPP/include" \
        -I"$MPP/include/comm" -I"$SDK/src/common/cdk/user/component/ipcmsg/include" \
        tests/control_test.cpp big/control.cpp -o out/tests/control_test
    out/tests/control_test
    c++ -std=c++17 -Wall -Wextra -Werror -Icommon -Ibig -I"$MPP/include" \
        -I"$MPP/include/comm" -I"$MPP/userapps/api" \
        tests/osd_test.cpp big/osd.cpp common/faces.cpp -o out/tests/osd_test
    out/tests/osd_test
    c++ -std=c++17 -Wall -Wextra -Werror -pthread -Icommon \
        tests/frames_test.cpp common/frame.cpp common/frame_queue.cpp -o out/tests/frames_test
    out/tests/frames_test
    CDK="$SDK/src/common/cdk/user"
    c++ -std=c++17 -Wall -Wextra -Werror -pthread -Icommon -Ilittle/media -I"$MPP/include" \
        -I"$MPP/include/comm" -I"$MPP/userapps/api" -I"$CDK/mapi/include" \
        -I"$CDK/mapi/include/api" -I"$CDK/mapi/include/comm" \
        tests/source_test.cpp little/media/source.cpp little/media/source_frames.cpp \
        common/frame.cpp common/frame_queue.cpp -o out/tests/source_test
    timeout 10 out/tests/source_test
fi
if [ "$1" = rtsp ]; then
    cmake -S tests/rtsp -B out/tests/rtsp -DCMAKE_BUILD_TYPE=Debug
    cmake --build out/tests/rtsp -j "$LAWREC_JOBS"
    timeout 15 out/tests/rtsp/rtsp_test
fi
