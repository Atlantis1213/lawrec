#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker required' >&2; exit 1; }
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RELEASE="out/releases/${LAWREC_REVISION:0:12}-$STAMP"
mkdir "$RELEASE" -p
export LAWREC_BUILD_ROOT="$RELEASE/build"
mkdir "$RELEASE/lawrec-demo"
BUNDLE="$RELEASE/lawrec-demo"
bash tools/in-container.sh all > "$RELEASE/build.log" 2>&1
bash tools/in-container.sh verify > "$RELEASE/verify.log" 2>&1
mkdir -p "$BUNDLE/bin" "$BUNDLE/models" "$BUNDLE/docs" "$BUNDLE/meta"
cp "$LAWREC_BUILD_ROOT/big/vision.elf" "$BUNDLE/bin/"
cp "$LAWREC_BUILD_ROOT/little/"{media_service,demo_ui,democtl} "$BUNDLE/bin/"
SYSROOT="$K230_SDK_ROOT/output/$LAWREC_BOARD/little/buildroot-ext/host/riscv64-buildroot-linux-gnu/sysroot"
bash tools/runtime-libs.sh "$LAWREC_BUILD_ROOT/little" "$SYSROOT" "$BUNDLE" > "$RELEASE/runtime.log" 2>&1
MODEL="$K230_SDK_ROOT/src/big/kmodel/door_lock/retinaface.kmodel"
EXPECTED=082f76bec6db39ee7a9c4c83c2d8196fdfa0ba487b7d07831647df01dee8d808
ACTUAL=$(sha256sum "$MODEL"); ACTUAL=${ACTUAL%% *}
test "$ACTUAL" = "$EXPECTED" || { echo 'Unexpected model bytes' >&2; exit 1; }
cp "$MODEL" "$BUNDLE/models/retinaface.kmodel"
cp tools/board/*.sh "$BUNDLE/"
cp README.md "$BUNDLE/"
cp docs/{acceptance.md,sdk-baseline.md,model.md,deployment.md} "$BUNDLE/docs/"
cp "$RELEASE/"{build.log,verify.log,runtime.log} "$BUNDLE/meta/"
cp patches/frozen.sha256 "$BUNDLE/meta/frozen-adapters.sha256"
cp patches/st7701.baseline.c "$BUNDLE/meta/st7701.reference.c"
printf 'commit=%s\nUTC=%s\nDocker=%s\nSDK=%s\nboard=%s\nprotocol=2\nstate=OFFLINE_ONLY_BOARD_ACCEPTANCE_PENDING\n' \
    "$LAWREC_REVISION" "$STAMP" "$LAWREC_IMAGE_ID" "$K230_SDK_ROOT" "$LAWREC_BOARD" > "$BUNDLE/meta/build.txt"
printf 'model_source=%s\nmodel_sha256=%s\n' "$MODEL" "$EXPECTED" >> "$BUNDLE/meta/build.txt"
bash tools/compiler-info.sh "$LAWREC_BUILD_ROOT" > "$BUNDLE/meta/compilers.txt"
chmod +x "$BUNDLE"/*.sh "$BUNDLE"/bin/* "$BUNDLE/lib/ld-linux-riscv64xthead-lp64d.so.1"
(cd "$BUNDLE"; find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
(cd "$BUNDLE"; sha256sum -c SHA256SUMS > /dev/null)
tar -C "$RELEASE" -czf "$RELEASE/lawrec-demo-${LAWREC_REVISION:0:12}.tar.gz" lawrec-demo
sha256sum "$RELEASE"/*.tar.gz > "$RELEASE/archive.sha256"
printf 'Offline application bundle: %s\nNo board connection, install or flash performed.\n' "$RELEASE"
