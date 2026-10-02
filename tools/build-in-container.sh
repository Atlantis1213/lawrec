#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker is required'; exit 1; }
case "$1" in
touch-ui)
    bash tools/build-touch-ui.sh
    ;;
touch-clock-phy)
    bash tools/build-touch-clock-phy.sh
    ;;
touch-dsi-panel)
    bash tools/build-touch-clock-phy.sh dsi-panel
    ;;
touch-vo-vtth-off)
    bash tools/build-touch-clock-phy.sh vo-vtth-off
    ;;
touch-vo-config)
    bash tools/build-touch-clock-phy.sh vo-config
    ;;
touch-timestamp)
    bash tools/build-touch-clock-phy.sh timestamp
    ;;
touch-phy-2lane)
    bash tools/build-touch-clock-phy.sh phy-2lane
    ;;
touch-panel-lckfb)
    bash tools/build-touch-clock-phy.sh panel-lckfb
    ;;
little)
    make -C "$K230_SDK_ROOT/output/$LAWREC_BOARD/little/buildroot-ext" lawrec-reconfigure
    ;;
big)
    export PATH="$K230_SDK_ROOT/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin:$PATH"
    cmake -S big -B out/big -DK230_SDK_ROOT="$K230_SDK_ROOT"
    cmake --build out/big -j "${LAWREC_JOBS:-4}"
    ;;
*) exit 2 ;;
esac
