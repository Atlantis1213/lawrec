#!/usr/bin/env bash
set -euo pipefail
PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
SDK_ROOT=$(cd "${K230_SDK_ROOT:-${PROJECT_ROOT}/../k230_sdk}" && pwd -P)
IMAGE=${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}
BOARD=${LAWREC_BOARD:-k230_canmv_lckfb_defconfig}
case "${1:-}" in
    little|big) TARGET=$1 ;;
    *) echo "Usage: bash tools/build.sh {little|big}"; exit 2 ;;
esac
exec docker run --rm --user "$(id -u):$(id -g)" \
    -e K230_IN_DOCKER=1 -e K230_SDK_ROOT="$SDK_ROOT" \
    -e LAWREC_PROJECT_ROOT="$PROJECT_ROOT" -e LAWREC_BOARD="$BOARD" \
    -v "$SDK_ROOT:$SDK_ROOT" -v "$PROJECT_ROOT:$PROJECT_ROOT" \
    -v "$SDK_ROOT:/home/atlantis/k230_sdk" \
    -v "$SDK_ROOT/toolchain:/opt/toolchain" -w "$PROJECT_ROOT" \
    "$IMAGE" bash tools/build-in-container.sh "$TARGET"
