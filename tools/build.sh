#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(cd "${K230_SDK_ROOT:-$ROOT/../k230_sdk}" && pwd -P)
IMAGE=${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}
TARGET=${1:-all}
case "$TARGET" in all|big|little|probe|test|camera|rtsp|board-rtsp|clip-check|media|ui|fonts|elf|verify|package|bundle-check) ;; *) echo 'Usage: tools/build.sh {all|big|little|probe|test|camera|rtsp|board-rtsp|clip-check|media|ui|fonts|elf|verify|package|bundle-check}' >&2; exit 2;; esac
NETWORK=none
if [ "$TARGET" = board-rtsp ]; then
    test -n "${LAWREC_RTSP_URL:?Explicit board RTSP URL required}"
    NETWORK=host
fi
FONT_MOUNTS=()
if [ "$TARGET" = fonts ]; then
    test -d "${LV_FONT_CONVERTER_ROOT:?Set an existing node_modules directory containing lv_font_conv 1.5.3}"
    test -x "${LV_FONT_NODE_BIN:?Set an existing Linux x86_64 Node binary}"
    FONT_MOUNTS=(-v "$LV_FONT_CONVERTER_ROOT:/opt/font-converter:ro" -v "$LV_FONT_NODE_BIN:/opt/font-node:ro")
fi
REVISION=$(git -C "$ROOT" rev-parse HEAD)
if [ "$TARGET" = package ] && [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
    echo 'Package requires a clean committed worktree' >&2; exit 1
fi
IMAGE_ID=$(docker image inspect --format '{{.Id}}' "$IMAGE")
mkdir -p "$ROOT/out"
exec docker run --rm --network "$NETWORK" --user "$(id -u):$(id -g)" \
    -e K230_SDK_ROOT="$SDK" -e LAWREC_BOARD=k230_canmv_lckfb_defconfig \
    -e LAWREC_JOBS="${LAWREC_JOBS:-4}" -e LVGL_ROOT="${LVGL_ROOT:-}" \
    -e LAWREC_REVISION="$REVISION" -e LAWREC_IMAGE_ID="$IMAGE_ID" \
    -e LAWREC_RTSP_URL="${LAWREC_RTSP_URL:-}" -e LAWREC_CLIP="${LAWREC_CLIP:-}" \
    -v "$SDK:$SDK:ro" -v "$SDK/toolchain:/opt/toolchain:ro" -v "$ROOT:$ROOT" -w "$ROOT" \
    "${FONT_MOUNTS[@]}" \
    "$IMAGE" bash tools/in-container.sh "$TARGET"
