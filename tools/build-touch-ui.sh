#!/usr/bin/env bash
# UI integration candidate, not a hardware-accepted release. Never flashes.
set -euo pipefail
test -f /.dockerenv || { echo 'Docker is required'; exit 1; }
SDK=${K230_SDK_ROOT:?}
PROJECT=${LAWREC_PROJECT_ROOT:?}
BOARD=${LAWREC_BOARD:?}
test "$BOARD" = k230_canmv_lckfb_defconfig
DRIVER="$SDK/src/big/mpp/kernel/connector/src/st7701.c"
BASELINE="$PROJECT/out/touch-panel-lckfb/20261001T094656Z/artifacts/st7701.c"
MARKER='[lawrec-display] ST7701 normal VO enable requested, VTTH=1'
grep -Fq "$MARKER" "$DRIVER"
grep -Fq 'mipi_phy_attr.phy_lan_num = K_DSI_4LAN;' "$DRIVER"
! grep -Fq 'TIMESTAMP_ONLY' "$DRIVER"
cmp <(sed -n '/static void st7701_480x800_init(/,/static void st7701_480x854_init(/p' "$DRIVER") \
    <(sed -n '/static void st7701_480x800_init(/,/static void st7701_480x854_init(/p' "$BASELINE")
grep -Fq 'connector_info.resolution.pclk = 27000;' "$PROJECT/big/main.cc"
grep -Fq 'connector_info.dsi_test_mode = 0;' "$PROJECT/big/main.cc"
! grep -Fq '[touch-test]' "$PROJECT/big/main.cc"
grep -q 'autostart disabled' "$SDK/src/big/rt-smart/userapps/root/bin/init.sh"
strings "$PROJECT/out/big/lawrec.elf" | grep -F 'build touch-ui-lckfb-27m-4lane'
IMAGES="$SDK/output/$BOARD/images"
TARGET="$SDK/output/$BOARD/little/buildroot-ext/target"
RUN="$PROJECT/out/touch-ui/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RUN/backup" "$RUN/artifacts"
trap 'chown -R "${LAWREC_HOST_UID:?}:${LAWREC_HOST_GID:?}" "$RUN"' EXIT
exec > >(tee "$RUN/build.log") 2>&1
echo "UI integration candidate: $RUN"
for file in big-core/rtt_system.bin sysimage-sdcard.img sysimage-sdcard.img.gz; do
    mkdir -p "$RUN/backup/$(dirname "$file")"
    cp --reflink=auto --sparse=always "$IMAGES/$file" "$RUN/backup/$file"
done
cp "$SDK/src/big/rt-smart/userapps/root/bin/fastboot_app.elf" "$RUN/backup/fastboot_app.elf"
cp "$SDK/.config" "$RUN/backup/sdk.config"
cp "$DRIVER" "$RUN/artifacts/st7701.c"
cp "$PROJECT/big/main.cc" "$RUN/artifacts/main.cc"
cp "$PROJECT/out/big/lawrec.elf" "$SDK/src/big/rt-smart/userapps/root/bin/fastboot_app.elf"

# Preserve the Linux touch adaptation. Build only apps, MPP/RTT and packaging.
make -C "$SDK" CONF="$BOARD" prepare_memory
make -C "$SDK" CONF="$BOARD" mpp-kernel
# The kernel target only recompiles romfs.c, not its root/bin inputs. Generate
# it explicitly; otherwise /bin/fastboot_app.elf silently stays at the old build.
python3 "$SDK/src/big/rt-smart/tools/mkromfs.py" \
    "$SDK/src/big/rt-smart/userapps/root" \
    "$SDK/src/big/rt-smart/kernel/bsp/maix3/applications/romfs.c"
make -C "$SDK" CONF="$BOARD" big-core-opensbi
make -C "$SDK" CONF="$BOARD" build-image
bash "$PROJECT/tools/verify-sd-image.sh"
RTT="$SDK/src/big/rt-smart/kernel/bsp/maix3/rtthread.elf"
strings "$RTT" | grep -F "$MARKER"
cp "$PROJECT/out/big/lawrec.elf" "$RUN/artifacts/lawrec.elf"
cp "$RTT" "$RUN/artifacts/rtthread.elf"
cp "$IMAGES/big-core/rtt_system.bin" "$RUN/artifacts/rtt_system-touch-ui.bin"
cp --reflink=auto --sparse=always "$IMAGES/sysimage-sdcard.img" "$RUN/artifacts/sysimage-touch-ui.img"
cp "$IMAGES/sysimage-sdcard.img.gz" "$RUN/artifacts/sysimage-touch-ui.img.gz"
cp -a "$TARGET/app/lawrec/ui" "$RUN/artifacts/ui"
cp "$TARGET/etc/init.d/S99lawrec" "$TARGET/etc/init.d/S45wifi" "$RUN/artifacts/"
(cd "$RUN/artifacts" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
echo "PASS: image content checked; normal touch UI requires board acceptance"
echo "Artifacts: $RUN/artifacts"
