#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(readlink -f "${K230_SDK_ROOT:-$ROOT/../k230_sdk}")
DEST=${1:-out/ui-preview/current}
[[ "$DEST" == out/* && "$DEST" != *..* ]] || { echo "Use an output path under out/"; exit 2; }
exec docker run --rm --user "$(id -u):$(id -g)" --network none \
    -v "$ROOT:$ROOT" -v "$SDK:$SDK:ro" -w "$ROOT" \
    -e LAWREC_PROJECT_ROOT="$ROOT" -e K230_SDK_ROOT="$SDK" \
    "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" \
    bash tools/ui-preview/run.sh "$DEST"
