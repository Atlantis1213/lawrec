#!/usr/bin/env bash
# Cheap packaging tests only; fake payloads are never run or installed.
set -euo pipefail
[[ -f /.dockerenv ]] || { echo 'Use bash tools/test.sh package'; exit 1; }
ROOT=$PWD
source tools/package-lib.sh
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PROJECT="$WORK/project"
TARGET="$WORK/target"
BUNDLE="$WORK/bundle"
SDK="$WORK/sdk"
for path in .config README.md src/big/nncase/examples/cmake/Riscv64.cmake \
    src/big/nncase/examples/cmake/link.lds src/big/mpp/kernel/connector/src/st7701.c \
    src/big/mpp/userapps/src/connector/mpi_connector.c \
    src/little/linux/arch/riscv/boot/dts/kendryte/k230_canmv_lckfb.dts \
    src/little/linux/drivers/input/touchscreen/edt-ft5x06.c; do
    mkdir -p "$SDK/$(dirname "$path")"
    printf 'fixture SDK\n' > "$SDK/$path"
done
lawrec_sdk_capture "$SDK" "$WORK/sdk-first"
(cd "$WORK/sdk-first" && sha256sum -c SDK.sha256 >/dev/null)
printf 'changed PHY\n' >> "$SDK/src/big/mpp/kernel/connector/src/st7701.c"
lawrec_sdk_capture "$SDK" "$WORK/sdk-changed"
if cmp "$WORK/sdk-first/SDK.sha256" "$WORK/sdk-changed/SDK.sha256" >/dev/null; then exit 1; fi
mkdir -p "$PROJECT/little" "$PROJECT/tools" "$PROJECT/docs" "$PROJECT/out/big" \
    "$TARGET/app/lawrec/ui/data" "$TARGET/usr/lib" "$TARGET/etc/init.d"
git -C "$PROJECT" init -q
printf '/out/\n/.env\n' > "$PROJECT/.gitignore"
printf 'fixture\n' > "$PROJECT/little/module.c"
git -C "$PROJECT" add .gitignore little/module.c
git -C "$PROJECT" -c user.name=Fixture -c user.email=fixture@example.invalid commit -qm fixture
printf 'new module\n' > "$PROJECT/little/new.c"
lawrec_source_manifest "$PROJECT" "$WORK/first"
lawrec_source_manifest "$PROJECT" "$WORK/same"
lawrec_assert_same_source "$WORK/first" "$WORK/same"
grep -q 'little/new.c' "$WORK/first/SOURCE.sha256"
printf 'changed\n' >> "$PROJECT/little/new.c"
lawrec_source_manifest "$PROJECT" "$WORK/changed"
if lawrec_assert_same_source "$WORK/first" "$WORK/changed" >/dev/null 2>&1; then exit 1; fi
chmod 755 "$PROJECT/little/module.c"
lawrec_source_manifest "$PROJECT" "$WORK/mode"
if lawrec_assert_same_source "$WORK/changed" "$WORK/mode" >/dev/null 2>&1; then exit 1; fi
printf 'not a real secret\n' > "$PROJECT/auth.json"
if lawrec_source_manifest "$PROJECT" "$WORK/secret" >/dev/null 2>&1; then exit 1; fi
rm "$PROJECT/auth.json"
ln -s module.c "$PROJECT/little/link.c"
if lawrec_source_manifest "$PROJECT" "$WORK/link" >/dev/null 2>&1; then exit 1; fi
rm "$PROJECT/little/link.c"
for name in S99lawrec S45wifi; do
    cp "$ROOT/little/$name" "$PROJECT/little/$name"
    cp "$ROOT/little/$name" "$TARGET/etc/init.d/$name"
done
cp "$ROOT/tools/verify-app-package.sh" "$PROJECT/tools/"
for name in v1_验收与部署.md 持续开发验收清单.md; do printf 'fixture\n' > "$PROJECT/docs/$name"; done
printf 'linked guide\n' > "$PROJECT/docs/guide.md"
printf 'fake UI\n' > "$TARGET/app/lawrec/ui/ui"
chmod 755 "$TARGET/app/lawrec/ui/ui"
printf 'fake ELF\n' > "$PROJECT/out/big/lawrec.elf"
printf 'fake library\n' > "$TARGET/usr/lib/liblvgl.real"
ln -s liblvgl.real "$TARGET/usr/lib/liblvgl.so"
printf 'fake driver\n' > "$TARGET/usr/lib/liblv_drivers.so"
printf 'stale script\n' > "$TARGET/etc/init.d/S45wifi"
if lawrec_stage_apps "$PROJECT" "$TARGET" "$WORK/stale" >/dev/null 2>&1; then exit 1; fi
cp "$ROOT/little/S45wifi" "$TARGET/etc/init.d/S45wifi"
lawrec_stage_apps "$PROJECT" "$TARGET" "$BUNDLE"
test ! -L "$BUNDLE/lib/liblvgl.so"
test -s "$BUNDLE/docs/guide.md"
mkdir -p "$BUNDLE/sdk-inputs" "$BUNDLE/source"
printf 'fixture SDK\n' > "$BUNDLE/sdk-inputs/input.c"
(cd "$BUNDLE/sdk-inputs" && sha256sum ./input.c > SDK.sha256)
printf 'development only\n' > "$BUNDLE/BUILD.txt"
lawrec_source_manifest "$PROJECT" "$BUNDLE/source"
tar -C "$PROJECT" --null --verbatim-files-from -czf "$BUNDLE/source.tar.gz" -T "$BUNDLE/source/SOURCE.paths"
lawrec_package_manifest "$BUNDLE"
sh "$BUNDLE/verify.sh" >/dev/null
tar -czf "$WORK/package.tar.gz" -C "$WORK" bundle
mkdir "$WORK/extracted"
tar -xzf "$WORK/package.tar.gz" -C "$WORK/extracted"
sh "$WORK/extracted/bundle/verify.sh" >/dev/null
printf 'unexpected\n' > "$BUNDLE/extra.txt"
if sh "$BUNDLE/verify.sh" >/dev/null 2>&1; then exit 1; fi
rm "$BUNDLE/extra.txt"
chmod 644 "$BUNDLE/etc/init.d/S45wifi"
if sh "$BUNDLE/verify.sh" >/dev/null 2>&1; then exit 1; fi
chmod 755 "$BUNDLE/etc/init.d/S45wifi"
printf 'tampered\n' >> "$BUNDLE/lib/liblv_drivers.so"
if sh "$BUNDLE/verify.sh" >/dev/null 2>&1; then exit 1; fi
echo 'Package: source/content/mode drift, credentials, symlinks, stale scripts, tampering and extracted bundle checks passed'
bash tools/verify-elf-in-container.sh --reject-host
