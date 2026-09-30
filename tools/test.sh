#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(readlink -f "${K230_SDK_ROOT:-/home/atlantis/k230_sdk}")
exec docker run --rm --user "$(id -u):$(id -g)" \
  -v "$SDK:$SDK:ro" -e SDK="$SDK" \
  -v "$ROOT:$ROOT" -w "$ROOT" "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" \
  bash -c 'set -eu
    mkdir -p out/tests
    g++ -std=c++11 -Wall -Wextra -I little/src/common tests/wifi_transaction_test.cpp little/src/common/lawrec_wifi_transaction.cpp -o out/tests/wifi_transaction_test
    out/tests/wifi_transaction_test
    g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/process_test.cpp little/src/common/lawrec_process.cpp -o out/tests/process_test
    out/tests/process_test
    g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/network_test.cpp little/src/common/lawrec_network.cpp little/src/common/lawrec_process.cpp little/src/common/lawrec_wifi_transaction.cpp -o out/tests/network_test
    out/tests/network_test
    g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/settings_test.cpp little/src/common/lawrec_settings.cpp -o out/tests/settings_test
    out/tests/settings_test "$PWD/out/tests/settings-$$"
    g++ -std=c++11 -Wall -Wextra -I little/src/common tests/storage_test.cpp little/src/common/lawrec_storage.cpp -o out/tests/storage_test
    out/tests/storage_test "$PWD/out/tests/storage-$$"
    g++ -std=c++11 -Wall -Wextra -I little/src/common tests/annexb_test.cpp -o out/tests/annexb_test
    out/tests/annexb_test
    gcc -std=gnu11 -Wall -Wextra -I little/src/ui/lvgl_port tests/touch_test.c -o out/tests/touch_test
    out/tests/touch_test
    g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common \
      -I "$SDK/src/common/cdk/user/mapi/include" \
      -I "$SDK/src/common/cdk/user/mapi/include/api" \
      -I "$SDK/src/common/cdk/user/mapi/include/comm" \
      -I "$SDK/src/common/cdk/user/middleware/mp4_format/include" \
      -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" \
      -I "$SDK/src/big/mpp/userapps/api" \
      tests/playback_test.cpp little/src/common/lawrec_storage.cpp -o out/tests/playback_test
    out/tests/playback_test "$PWD/out/tests/playback-$$"
  '
