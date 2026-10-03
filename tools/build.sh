#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(cd "${K230_SDK_ROOT:-$ROOT/../k230_sdk}" && pwd -P)
IMAGE=${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}
TARGET=${1:-all}
case "$TARGET" in all|big|little|test|rtsp|media|ui|elf|verify|package|bundle-check) ;; *) echo 'Usage: tools/build.sh {all|big|little|test|rtsp|media|ui|elf|verify|package|bundle-check}' >&2; exit 2;; esac
REVISION=$(git -C "$ROOT" rev-parse HEAD)
if [ "$TARGET" = package ] && [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
    echo 'Package requires a clean committed worktree' >&2; exit 1
fi
IMAGE_ID=$(docker image inspect --format '{{.Id}}' "$IMAGE")
mkdir -p "$ROOT/out"
exec docker run --rm --network none --user "$(id -u):$(id -g)" \
    -e K230_SDK_ROOT="$SDK" -e LAWREC_BOARD=k230_canmv_lckfb_defconfig \
    -e LAWREC_JOBS="${LAWREC_JOBS:-4}" -e LVGL_ROOT="${LVGL_ROOT:-}" \
    -e LAWREC_REVISION="$REVISION" -e LAWREC_IMAGE_ID="$IMAGE_ID" \
    -v "$SDK:$SDK:ro" -v "$SDK/toolchain:/opt/toolchain:ro" -v "$ROOT:$ROOT" -w "$ROOT" \
    "$IMAGE" bash tools/in-container.sh "$TARGET"
