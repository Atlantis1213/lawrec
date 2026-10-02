#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(readlink -f "${K230_SDK_ROOT:-/home/atlantis/k230_sdk}")
case "${1:-all}" in all|rtsp-source|media-lifecycle|package|touch|preview|ui-preview-ipc|time|network|storage|files|demux|playback|arp-transport|media-settings|record-io|dhcp) ;; *) echo 'Usage: bash tools/test.sh [all|rtsp-source|media-lifecycle|package|touch|preview|ui-preview-ipc|time|network|storage|files|demux|playback|arp-transport|media-settings|record-io|dhcp]'; exit 2 ;; esac
if [ "${1:-all}" = arp-transport ]; then
  exec docker run --rm --init --network none --cap-add NET_ADMIN --user 0:0 \
    -e LAWREC_HOST_UID="$(id -u)" -e LAWREC_HOST_GID="$(id -g)" \
    -v "$ROOT:$ROOT" -w "$ROOT" "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" \
    bash tools/test-arp-transport-in-container.sh
fi
exec docker run --rm --init --network none --user "$(id -u):$(id -g)" \
  -v "$SDK:$SDK:ro" -e SDK="$SDK" \
  -v "$ROOT:$ROOT" -w "$ROOT" "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" \
  bash -c 'set -eu
    mkdir -p out/tests
    if [ "$1" = all ] || [ "$1" = rtsp-source ]; then
      bash tools/test-rtsp-source-in-container.sh
      if [ "$1" = rtsp-source ]; then exit 0; fi
    fi
    if [ "$1" = media-lifecycle ]; then
      common_includes="-I little/src/common -I $SDK/src/common/cdk/user/mapi/include -I $SDK/src/common/cdk/user/mapi/include/api -I $SDK/src/common/cdk/user/mapi/include/comm -I $SDK/src/big/mpp/include -I $SDK/src/big/mpp/include/comm -I $SDK/src/big/mpp/userapps/api"
      g++ -std=c++14 -Wall -Wextra -pthread -DCONFIG_BOARD_K230_CANMV_LCKFB=1 $common_includes \
        tests/encoder_test.cpp little/src/common/lawrec_encoder.cpp -o out/tests/encoder_test
      out/tests/encoder_test
      g++ -std=c++14 -Wall -Wextra -pthread $common_includes tests/audio_test.cpp -o out/tests/audio_test
      out/tests/audio_test
      g++ -std=c++14 -Wall -Wextra -pthread $common_includes \
        tests/media_lease_test.cpp little/src/common/lawrec_media.cpp -o out/tests/media_lease_test
      out/tests/media_lease_test
      exit 0
    fi
    if [ "$1" = all ] || [ "$1" = package ]; then
      bash tests/package_test.sh
      if [ "$1" = package ]; then exit 0; fi
    fi
    if [ "$1" = all ] || [ "$1" = storage ] || [ "$1" = files ]; then
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/files_test.cpp \
        little/src/common/lawrec_storage.cpp little/src/common/lawrec_settings.cpp -o out/tests/files_test
      out/tests/files_test "$PWD/out/tests/files-$$"
      if [ "$1" = files ]; then exit 0; fi
    fi
    if [ "$1" = all ] || [ "$1" = ui-preview-ipc ]; then
      g++ -std=c++11 -Wall -Wextra -pthread \
        -I "$SDK/src/common/cdk/user/component/ipcmsg/include" -I "$SDK/src/big/mpp/include" \
        tests/big_ipc_server_test.cpp big/src/common/lawrec_ipc_server.cpp -o out/tests/big_ipc_server_test
      out/tests/big_ipc_server_test
      g++ -std=c++11 -Wall -Wextra -pthread \
        -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" -I "$SDK/src/big/mpp/userapps/api" \
        tests/display_test.cpp big/src/common/lawrec_display.cpp -o out/tests/display_test
      out/tests/display_test
      g++ -std=c++14 -Wall -Wextra -pthread \
        -I little/src/common -I "$SDK/src/common/cdk/user/mapi/include" \
        -I "$SDK/src/common/cdk/user/mapi/include/api" -I "$SDK/src/common/cdk/user/mapi/include/comm" \
        -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" \
        tests/media_lease_test.cpp little/src/common/lawrec_media.cpp -o out/tests/media_lease_test
      out/tests/media_lease_test
      g++ -std=c++11 -Wall -Wextra -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DLV_CONF_PATH="$PWD/little/src/ui/lvgl_port/lv_conf.h" -DLV_LVGL_H_INCLUDE_SIMPLE \
        -I "$SDK/output/k230_canmv_lckfb_defconfig/little/buildroot-ext/build/lawrec/thirdlib/lvgl" \
        -I "$SDK/src/common/cdk/user/component/ipcmsg/include" \
        -I "$SDK/src/big/mpp/include" \
        tests/ui_preview_ipc_test.cpp little/src/control/src/lawrec_control.cpp -o out/tests/ui_preview_ipc_test
      out/tests/ui_preview_ipc_test
      if [ "$1" = ui-preview-ipc ]; then exit 0; fi
    fi
    if [ "$1" = all ] || [ "$1" = record-io ]; then
      bash tools/test-record-io-in-container.sh
      if [ "$1" = record-io ]; then exit 0; fi
    fi
    if [ "$1" = all ] || [ "$1" = preview ]; then
      g++ -std=c++11 -Wall -Wextra -pthread -DCONFIG_BOARD_K230_CANMV_LCKFB=1 \
        -I big/src/preview -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" \
        -I "$SDK/src/big/mpp/userapps/api" \
        tests/preview_test.cpp big/src/preview/lawrec_preview.cpp -o out/tests/preview_test
      out/tests/preview_test
      if [ "$1" = preview ]; then exit 0; fi
    fi
    if [ "$1" = touch ]; then
      gcc -std=gnu11 -Wall -Wextra -I little/src/ui/lvgl_port tests/touch_test.c -o out/tests/touch_test
      out/tests/touch_test
      exit 0
    fi
    if [ "$1" = media-settings ]; then
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/settings_test.cpp little/src/common/lawrec_settings.cpp -o out/tests/settings_test
      out/tests/settings_test "$PWD/out/tests/settings-$$"
      g++ -std=c++14 -Wall -Wextra -pthread -DCONFIG_BOARD_K230_CANMV_LCKFB=1 \
        -I little/src/common -I "$SDK/src/common/cdk/user/mapi/include" \
        -I "$SDK/src/common/cdk/user/mapi/include/api" -I "$SDK/src/common/cdk/user/mapi/include/comm" \
        -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" -I "$SDK/src/big/mpp/userapps/api" \
        tests/encoder_test.cpp little/src/common/lawrec_encoder.cpp -o out/tests/encoder_test
      out/tests/encoder_test
      exit 0
    fi
    if [ "$1" = all ] || [ "$1" = demux ]; then
      bash tools/test-demux-in-container.sh
    fi
    if [ "$1" = all ] || [ "$1" = network ] || [ "$1" = dhcp ]; then
      g++ -std=c++11 -Wall -Wextra -pthread -DLAWREC_DHCP_TESTING -I little/src/common \
        tests/dhcp_test.cpp little/src/common/lawrec_dhcp.cpp little/src/common/lawrec_process.cpp \
        -Wl,--wrap=clock_gettime -o out/tests/dhcp_test
      out/tests/dhcp_test "$PWD/out/tests/dhcp-$$"
      g++ -std=c++11 -Wall -Wextra -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -I little/src/common tests/network_dhcp_status_test.cpp \
        -o out/tests/network_dhcp_status_test
      out/tests/network_dhcp_status_test
      if [ "$1" = dhcp ]; then exit 0; fi
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common \
        tests/arp_test.cpp little/src/common/lawrec_arp.cpp -o out/tests/arp_test
      out/tests/arp_test
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/process_test.cpp little/src/common/lawrec_process.cpp -o out/tests/process_test
      out/tests/process_test
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common \
        tests/ipv4_test.cpp little/src/common/lawrec_settings.cpp little/src/common/lawrec_ipv4_apply.cpp -o out/tests/ipv4_test
      out/tests/ipv4_test "$PWD/out/tests/ipv4-$$"
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common \
        tests/ipv4_transaction_test.cpp little/src/common/lawrec_ipv4_transaction.cpp little/src/common/lawrec_settings.cpp -o out/tests/ipv4_transaction_test
      out/tests/ipv4_transaction_test
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common \
        tests/network_test.cpp little/src/common/lawrec_network.cpp little/src/common/lawrec_settings.cpp \
        little/src/common/lawrec_ipv4_apply.cpp little/src/common/lawrec_ipv4_transaction.cpp little/src/common/lawrec_arp.cpp little/src/common/lawrec_dhcp.cpp little/src/common/lawrec_process.cpp little/src/common/lawrec_wifi_transaction.cpp -o out/tests/network_test
      out/tests/network_test
      g++ -std=c++11 -Wall -Wextra -I little/src/common tests/wifi_transaction_test.cpp little/src/common/lawrec_wifi_transaction.cpp -o out/tests/wifi_transaction_test
      out/tests/wifi_transaction_test
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/settings_test.cpp little/src/common/lawrec_settings.cpp -o out/tests/settings_test
      out/tests/settings_test "$PWD/out/tests/settings-$$"
      if [ "$1" = network ]; then exit 0; fi
    fi
    if [ "$1" = all ] || [ "$1" = storage ]; then
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/record_dir_test.cpp \
        little/src/common/lawrec_settings.cpp little/src/common/lawrec_storage.cpp -o out/tests/record_dir_test
      out/tests/record_dir_test "$PWD/out/tests/record-dir-$$"
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/storage_test.cpp \
        little/src/common/lawrec_storage.cpp little/src/common/lawrec_settings.cpp -o out/tests/storage_test
      out/tests/storage_test "$PWD/out/tests/storage-$$"
      if [ "$1" = storage ]; then
        g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/settings_test.cpp little/src/common/lawrec_settings.cpp -o out/tests/settings_test
        out/tests/settings_test "$PWD/out/tests/settings-$$"
      fi
    fi
    if [ "$1" = all ] || [ "$1" = storage ] || [ "$1" = demux ] || [ "$1" = playback ]; then
      g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common \
        -I "$SDK/src/common/cdk/user/mapi/include" \
        -I "$SDK/src/common/cdk/user/mapi/include/api" \
        -I "$SDK/src/common/cdk/user/mapi/include/comm" \
        -I "$SDK/src/common/cdk/user/middleware/mp4_format/include" \
        -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" \
        -I "$SDK/src/big/mpp/userapps/api" \
        tests/playback_test.cpp little/src/playback/lawrec_demux.cpp little/src/common/lawrec_storage.cpp little/src/common/lawrec_settings.cpp -o out/tests/playback_test
      playback_work=$(mktemp -d "$PWD/out/tests/playback.XXXXXX")
      out/tests/playback_test "$playback_work/recordings"
      rm -rf -- "$playback_work"
      if [ "$1" != all ]; then exit 0; fi
    fi
    g++ -std=c++11 -Wall -Wextra -pthread -ffunction-sections -fdata-sections \
      -Wl,--gc-sections -Wl,--wrap=clock_settime -Wl,--wrap=clock_gettime \
      -I little/src/common tests/time_test.cpp little/src/common/lawrec_time.cpp \
      little/src/control/src/lawrec_control.cpp -o out/tests/time_test
    out/tests/time_test
    if [ "$1" = time ]; then exit 0; fi
    g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/media_clock_test.cpp -o out/tests/media_clock_test
    out/tests/media_clock_test
    g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common tests/frame_queue_test.cpp -o out/tests/frame_queue_test
    out/tests/frame_queue_test
    g++ -std=c++11 -Wall -Wextra -I little/src/common tests/annexb_test.cpp -o out/tests/annexb_test
    out/tests/annexb_test
    gcc -std=gnu11 -Wall -Wextra -I little/src/ui/lvgl_port tests/touch_test.c -o out/tests/touch_test
    out/tests/touch_test
    g++ -std=c++14 -Wall -Wextra -pthread -DCONFIG_BOARD_K230_CANMV_LCKFB=1 \
      -I little/src/common -I "$SDK/src/common/cdk/user/mapi/include" \
      -I "$SDK/src/common/cdk/user/mapi/include/api" -I "$SDK/src/common/cdk/user/mapi/include/comm" \
      -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" -I "$SDK/src/big/mpp/userapps/api" \
      tests/encoder_test.cpp little/src/common/lawrec_encoder.cpp -o out/tests/encoder_test
    out/tests/encoder_test
    g++ -std=c++14 -Wall -Wextra -pthread \
      -I little/src/common -I "$SDK/src/common/cdk/user/mapi/include" -I "$SDK/src/common/cdk/user/mapi/include/api" \
      -I "$SDK/src/common/cdk/user/mapi/include/comm" \
      -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" \
      tests/media_lease_test.cpp little/src/common/lawrec_media.cpp -o out/tests/media_lease_test
    out/tests/media_lease_test
    g++ -std=c++14 -Wall -Wextra -pthread \
      -I little/src/common -I "$SDK/src/common/cdk/user/mapi/include" \
      -I "$SDK/src/common/cdk/user/mapi/include/api" -I "$SDK/src/common/cdk/user/mapi/include/comm" \
      -I "$SDK/src/big/mpp/include" -I "$SDK/src/big/mpp/include/comm" -I "$SDK/src/big/mpp/userapps/api" \
      tests/audio_test.cpp -o out/tests/audio_test
    out/tests/audio_test
  ' lawrec-tests "${1:-all}"
