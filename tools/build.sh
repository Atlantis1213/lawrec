#!/usr/bin/env bash
set -euo pipefail
PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
SDK_ROOT=$(cd "${K230_SDK_ROOT:-${PROJECT_ROOT}/../k230_sdk}" && pwd -P)
IMAGE=${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}
BOARD=${LAWREC_BOARD:-k230_canmv_lckfb_defconfig}
case "${1:-}" in
    little|big|touch-ui|touch-clock-phy|touch-dsi-panel|touch-vo-vtth-off|touch-vo-config|touch-timestamp|touch-phy-2lane|touch-panel-lckfb) TARGET=$1 ;;
    *) echo "Usage: bash tools/build.sh {little|big|touch-ui|touch-clock-phy|touch-dsi-panel|touch-vo-vtth-off|touch-vo-config|touch-timestamp|touch-phy-2lane|touch-panel-lckfb}"; exit 2 ;;
esac
if [ "$TARGET" = touch-ui ]; then
    # Application outputs retain the normal developer ownership. Only SDK
    # firmware/image staging needs root in its separate Docker invocation.
    bash "$PROJECT_ROOT/tools/build.sh" big
    bash "$PROJECT_ROOT/tools/build.sh" little
fi
DOCKER_USER="$(id -u):$(id -g)"
# The existing SDK firmware/image staging tree contains root-owned files.
case "$TARGET" in touch-ui|touch-clock-phy|touch-dsi-panel|touch-vo-vtth-off|touch-vo-config|touch-timestamp|touch-phy-2lane|touch-panel-lckfb) DOCKER_USER=0:0 ;; esac
exec docker run --rm --user "$DOCKER_USER" \
    -e LAWREC_HOST_UID="$(id -u)" -e LAWREC_HOST_GID="$(id -g)" \
    -e K230_IN_DOCKER=1 -e K230_SDK_ROOT="$SDK_ROOT" \
    -e LAWREC_PROJECT_ROOT="$PROJECT_ROOT" -e LAWREC_BOARD="$BOARD" \
    -v "$SDK_ROOT:$SDK_ROOT" -v "$PROJECT_ROOT:$PROJECT_ROOT" \
    -v "$SDK_ROOT:/home/atlantis/k230_sdk" \
    -v "$SDK_ROOT/toolchain:/opt/toolchain" -w "$PROJECT_ROOT" \
    "$IMAGE" bash tools/build-in-container.sh "$TARGET"
