#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(readlink -f "${K230_SDK_ROOT:-$ROOT/../k230_sdk}")
mkdir -p "$ROOT/out/font-tools"
docker run --rm --user "$(id -u):$(id -g)" -e npm_config_cache=/tmp/npm-cache \
    -v "$ROOT:$ROOT" -w "$ROOT" node:18-bullseye \
    npm install --prefix "$ROOT/out/font-tools" --no-audit --no-fund lv_font_conv@1.5.3
exec docker run --rm --user "$(id -u):$(id -g)" --network none \
    -v "$ROOT:$ROOT" -v "$SDK:$SDK:ro" -w "$ROOT" \
    -e LAWREC_PROJECT_ROOT="$ROOT" -e K230_SDK_ROOT="$SDK" \
    node:18-bullseye node tools/ui-preview/generate-fonts.js
