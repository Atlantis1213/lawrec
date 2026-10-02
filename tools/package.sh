#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SDK=$(cd "${K230_SDK_ROOT:-$ROOT/../k230_sdk}" && pwd -P)
BOARD=${LAWREC_BOARD:-k230_canmv_lckfb_defconfig}
cd "$ROOT"
MODE=rc
case "${1:-}" in
    '') ;;
    --snapshot) MODE=snapshot ;;
    *) echo 'Usage: bash tools/package.sh [--snapshot]'; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo 'Usage: bash tools/package.sh [--snapshot]'; exit 2; }
[[ "$BOARD" = k230_canmv_lckfb_defconfig ]] || { echo 'This bundle supports only the verified LCKFB board configuration.'; exit 1; }
if [ "$MODE" = rc ] && [ -n "$(git status --porcelain)" ]; then
    echo 'Commit the reviewed source first so the package has a reproducible source revision.'
    exit 1
fi
source tools/package-lib.sh
mkdir -p "$ROOT/out"
WORK=$(mktemp -d "$ROOT/out/package.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
lawrec_source_manifest "$ROOT" "$WORK/before"
lawrec_sdk_capture "$SDK" "$WORK/sdk-before"
git status --porcelain=v1 > "$WORK/git-state.txt"
# A development snapshot is explicitly not a reviewed release revision. Build
# both apps, but don't silently turn focused packaging checks into full acceptance.
TEST_MODE=all
[[ "$MODE" != snapshot ]] || TEST_MODE=package
bash tools/test.sh "$TEST_MODE" > "$WORK/tests.log" 2>&1 || { cat "$WORK/tests.log"; exit 1; }
bash tools/build.sh big > "$WORK/big-build.log" 2>&1 || { cat "$WORK/big-build.log"; exit 1; }
bash tools/build.sh little > "$WORK/little-build.log" 2>&1 || { cat "$WORK/little-build.log"; exit 1; }
REV=$(git rev-parse --short HEAD)
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NAME="v1.0.0-rc1-$REV-$STAMP"
[[ "$MODE" != snapshot ]] || NAME="lawrec-dev-$STAMP-$REV"
mkdir -p "$ROOT/out/releases"
DEST="$WORK/$NAME"
TARGET="$SDK/output/$BOARD/little/buildroot-ext/target"
lawrec_stage_apps "$ROOT" "$TARGET" "$DEST"
mkdir "$DEST/evidence"
cp "$WORK/tests.log" "$WORK/big-build.log" "$WORK/little-build.log" "$DEST/evidence/"
docker run --rm --user "$(id -u):$(id -g)" --network none \
    -v "$ROOT:$ROOT" -w "$ROOT" "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" \
    bash tools/verify-elf-in-container.sh "$DEST" > "$DEST/evidence/elf-check.log" 2>&1 || {
        cat "$DEST/evidence/elf-check.log"; exit 1
    }
cp -a "$WORK/before" "$DEST/source"
cp "$WORK/git-state.txt" "$DEST/source/GIT_STATE.txt"
tar --null --verbatim-files-from -czf "$DEST/source.tar.gz" -T "$WORK/before/SOURCE.paths"
# The full SDK is an external dependency. Save the locally adapted display/touch
# inputs, rather than claiming that .config alone identifies this unversioned tree.
lawrec_sdk_capture "$SDK" "$WORK/sdk-after"
cmp "$WORK/sdk-before/SDK.sha256" "$WORK/sdk-after/SDK.sha256" || { echo 'SDK display/touch inputs changed during build.'; exit 1; }
cp -a "$WORK/sdk-before" "$DEST/sdk-inputs"
SDK_REV=$(git -C "$SDK" rev-parse HEAD 2>/dev/null || printf 'unavailable-unversioned-sdk')
{
    printf 'source_base=%s\nsource_mode=%s\ncreated_utc=%s\n' "$(git rev-parse HEAD)" "$MODE" "$STAMP"
    printf 'source_digest=%s\n' "$(sha256sum "$WORK/before/SOURCE.sha256" | cut -d ' ' -f1)"
    printf 'board=%s\nsdk=%s\n' "$BOARD" "$SDK"
    printf 'sdk_revision=%s\nsdk_capture=selected-display-touch-inputs; full SDK required separately\n' "$SDK_REV"
    docker image inspect "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" --format 'docker={{.Id}}'
    printf 'test_scope=%s\n' "$([[ "$MODE" = rc ]] && echo all || echo package-integrity-only)"
    printf 'status=development-candidate; hardware acceptance pending\n'
    printf 'artifact=applications-only; not an SD image; no kernel install\n'
    printf 'kernel_requires=passing LCKFB panel sequence, 27MHz, 324Mbps, PHY4LAN, normal VO/VTTH\n'
} > "$DEST/BUILD.txt"
# Compare both before and after archive creation so an IDE edit during either
# compilation or packaging cannot silently associate different source with binaries.
lawrec_source_manifest "$ROOT" "$WORK/after"
lawrec_assert_same_source "$WORK/before" "$WORK/after"
lawrec_package_manifest "$DEST"
sh "$DEST/verify.sh" > "$WORK/verification.log"
mkdir "$WORK/unpacked"
tar -czf "$WORK/$NAME.tar.gz" -C "$WORK" "$NAME"
tar -xzf "$WORK/$NAME.tar.gz" -C "$WORK/unpacked"
sh "$WORK/unpacked/$NAME/verify.sh" >> "$WORK/verification.log"
mkdir "$WORK/source-check"
tar -xzf "$DEST/source.tar.gz" -C "$WORK/source-check"
(cd "$WORK/source-check" && sha256sum -c "$DEST/source/SOURCE.sha256") >> "$WORK/verification.log"
[[ ! -e "$ROOT/out/releases/$NAME" && ! -e "$ROOT/out/releases/$NAME.tar.gz" ]]
mv "$DEST" "$ROOT/out/releases/$NAME"
mv "$WORK/$NAME.tar.gz" "$ROOT/out/releases/$NAME.tar.gz"
cp "$WORK/verification.log" "$ROOT/out/releases/$NAME.verify.log"
sha256sum "$ROOT/out/releases/$NAME.tar.gz" > "$ROOT/out/releases/$NAME.tar.gz.sha256"
cat "$ROOT/out/releases/$NAME.tar.gz.sha256"
printf 'Package: %s.tar.gz\nNo install/deploy/reboot performed.\n' "$ROOT/out/releases/$NAME"
