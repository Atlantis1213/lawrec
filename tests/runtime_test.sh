#!/usr/bin/env bash
set -euo pipefail
test "$#" = 1 || exit 2
mkdir -p out/tests
BUNDLE=$(mktemp -d out/tests/runtime.XXXXXX)
mkdir "$BUNDLE/bin"
cp out/little/{media_service,demo_ui,democtl} "$BUNDLE/bin/"
bash tools/runtime-libs.sh out/little "$1" "$BUNDLE"
mv "$BUNDLE/lib/liblvgl.so" "$BUNDLE/lib/withheld-lvgl.so"
if bash tools/runtime-check.sh "$BUNDLE" > "$BUNDLE/missing-library.log" 2>&1; then
    echo 'Dependency checker accepted a missing library' >&2; exit 1
fi
mv "$BUNDLE/lib/withheld-lvgl.so" "$BUNDLE/lib/liblvgl.so"
echo "runtime: actual Linux binary dependencies/version names, missing-library rejection passed; reports=$BUNDLE/meta"
