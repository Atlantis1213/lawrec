#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(cd "${K230_SDK_ROOT:-$ROOT/../k230_sdk}" && pwd -P)
BOARD=${LAWREC_BOARD:-k230_canmv_lckfb_defconfig}
cd "$ROOT"
if [ -n "$(git status --porcelain)" ]; then
    echo 'Commit the reviewed source first so the package has a reproducible source revision.'
    exit 1
fi
bash tools/test.sh
bash tools/build.sh big
bash tools/build.sh little
REV=$(git rev-parse --short HEAD)
DEST="$ROOT/out/releases/v1.0.0-rc1-$REV"
mkdir -p "$DEST/app/lawrec" "$DEST/lib" "$DEST/etc/init.d"
TARGET="$SDK/output/$BOARD/little/buildroot-ext/target"
cp -a "$TARGET/app/lawrec/ui" "$DEST/app/lawrec/"
cp out/big/lawrec.elf "$DEST/lawrec.elf"
cp little/S99lawrec "$DEST/etc/init.d/"
find "$TARGET/usr/lib" -maxdepth 1 -name 'liblvgl*.so*' -exec cp -a '{}' "$DEST/lib/" \;
cp docs/v1_验收与部署.md "$DEST/"
{
    git rev-parse HEAD
    printf 'board=%s\nsdk=%s\n' "$BOARD" "$SDK"
    docker image inspect "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" --format 'docker={{.Id}}'
    sha256sum "$SDK/.config"
    printf 'status=release-candidate; hardware acceptance pending\n'
} > "$DEST/BUILD.txt"
(cd "$DEST" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -czf "$DEST.tar.gz" -C "$(dirname "$DEST")" "$(basename "$DEST")"
sha256sum "$DEST.tar.gz"
printf 'Package: %s.tar.gz\n' "$DEST"
