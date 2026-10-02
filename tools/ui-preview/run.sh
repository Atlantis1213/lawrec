#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv ]] || { echo "Use bash tools/render-ui.sh"; exit 1; }
cmake -S tools/ui-preview -B out/ui-preview/build
cmake --build out/ui-preview/build -j 4
mkdir -p "$1"
out/ui-preview/build/ui-preview "$1"
cp tools/ui-preview/index.html "$1/index.html"
