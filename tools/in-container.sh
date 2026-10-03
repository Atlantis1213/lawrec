#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker required' >&2; exit 1; }
SDK=$K230_SDK_ROOT
BR="$SDK/output/$LAWREC_BOARD/little/buildroot-ext"
export PATH="$SDK/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin:$PATH"
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
    c++ -std=c++17 -Wall -Wextra -Werror -Icommon little/media/main.cpp -o out/tests/mock_service
    c++ -std=c++17 -Wall -Wextra -Werror -Icommon tests/socket_test.cpp common/socket.cpp -o out/tests/socket_test
    out/tests/protocol_test
    out/tests/socket_test out/tests/mock_service
fi
