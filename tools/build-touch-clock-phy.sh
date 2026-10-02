#!/usr/bin/env bash
# Diagnostic firmware only: does not flash the board or alter autostart.
set -euo pipefail
test -f /.dockerenv || { echo 'Docker is required'; exit 1; }
SDK=${K230_SDK_ROOT:?}
PROJECT=${LAWREC_PROJECT_ROOT:?}
BOARD=${LAWREC_BOARD:?}
test "$BOARD" = k230_canmv_lckfb_defconfig
DRIVER="$SDK/src/big/mpp/kernel/connector/src/st7701.c"
STAGE=${1:-clock-phy}
TEST_ELF_NAME=lawrec-touch-clock-phy.elf
ELF_MARKER='CLOCK_PHY_ONLY candidate'
case "$STAGE" in
clock-phy)
    MARKER='[touch-test] CLOCK_PHY_ONLY: skip DSI/panel/VO'
    TEST_ELF="$PROJECT/out/big/lawrec.elf"
    ;;
dsi-panel)
    MARKER='[touch-test] DSI_PANEL_ONLY: skip VO'
    # Keep the exact application used for the passing CLOCK/PHY board test.
    TEST_ELF="$PROJECT/out/touch-clock-phy/20261001T064344Z/artifacts/lawrec-touch-clock-phy.elf"
    test -s "$TEST_ELF"
    ;;
vo-vtth-off)
    MARKER='[touch-test] VO_ENABLED_VTTH_OFF: VO enabled, VTTH requested off'
    TEST_ELF="$PROJECT/out/touch-clock-phy/20261001T064344Z/artifacts/lawrec-touch-clock-phy.elf"
    test -s "$TEST_ELF"
    ;;
vo-config)
    MARKER='[touch-test] VO_CONFIG_ONLY: skip connector_set_vo_enable'
    TEST_ELF="$PROJECT/out/touch-clock-phy/20261001T064344Z/artifacts/lawrec-touch-clock-phy.elf"
    test -s "$TEST_ELF"
    ;;
timestamp)
    MARKER='[touch-test] TIMESTAMP_ONLY: no VO enable'
    TEST_ELF="$PROJECT/out/touch-clock-phy/20261001T064344Z/artifacts/lawrec-touch-clock-phy.elf"
    test -s "$TEST_ELF"
    ;;
phy-2lane)
    MARKER='[touch-test] PHY_2LAN: ST7701 requested two lanes'
    TEST_ELF_NAME=lawrec-touch-lckfb-24750.elf
    ELF_MARKER='[touch-test] LCKFB_24750_AB:'
    TEST_ELF="$PROJECT/out/touch-lckfb-24750/20261001T085131Z/artifacts/$TEST_ELF_NAME"
    test -s "$TEST_ELF"
    grep -Fq 'mipi_phy_attr.phy_lan_num = K_DSI_2LAN;' "$DRIVER"
    grep -Fq '[touch-test] TIMESTAMP_ONLY: no VO enable' "$DRIVER"
    ;;
panel-lckfb)
    MARKER='[touch-test] TIMESTAMP_ONLY: no VO enable'
    TEST_ELF_NAME=lawrec-touch-lckfb-timing.elf
    ELF_MARKER='[touch-test] LCKFB_TIMING_AB:'
    TEST_ELF="$PROJECT/out/touch-lckfb-timing/20261001T083850Z/artifacts/$TEST_ELF_NAME"
    test -s "$TEST_ELF"
    grep -Fq 'mipi_phy_attr.phy_lan_num = K_DSI_4LAN;' "$DRIVER"
    grep -Fq 'SEND(0xC1, 0x04, 0x02);' "$DRIVER"
    cmp "$PROJECT/big/main.cc" "$PROJECT/out/touch-lckfb-timing/20261001T083850Z/artifacts/main.cc"
    ;;
*) echo 'Expected clock-phy, dsi-panel, vo-vtth-off, vo-config, timestamp, phy-2lane or panel-lckfb' >&2; exit 2 ;;
esac
grep -Fq "$MARKER" "$DRIVER"
grep -q 'autostart disabled' "$SDK/src/big/rt-smart/userapps/root/bin/init.sh"
IMAGES="$SDK/output/$BOARD/images"
RUN="$PROJECT/out/touch-$STAGE/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RUN/backup" "$RUN/artifacts"
trap 'chown -R "${LAWREC_HOST_UID:?}:${LAWREC_HOST_GID:?}" "$RUN" "$PROJECT/out/big"' EXIT
exec > >(tee "$RUN/build.log") 2>&1
echo "DIAGNOSTIC ONLY: $RUN"
for file in big-core/rtt_system.bin little-core/linux_system.bin sysimage-sdcard.img sysimage-sdcard.img.gz; do
    test -f "$IMAGES/$file"
    mkdir -p "$RUN/backup/$(dirname "$file")"
    cp --reflink=auto --sparse=always "$IMAGES/$file" "$RUN/backup/$file"
done
cp "$SDK/.config" "$RUN/backup/sdk.config"
cp "$DRIVER" "$RUN/artifacts/st7701.c"
cp "$PROJECT/big/main.cc" "$RUN/artifacts/main.cc"

# Do not regenerate Linux configuration or replace its touchscreen adaptation.
make -C "$SDK" CONF="$BOARD" prepare_memory
make -C "$SDK" CONF="$BOARD" mpp-kernel
make -C "$SDK" CONF="$BOARD" big-core-opensbi
if [ "$STAGE" = clock-phy ]; then
    bash "$PROJECT/tools/build-in-container.sh" big
fi
make -C "$SDK" CONF="$BOARD" build-image

RTT="$SDK/src/big/rt-smart/kernel/bsp/maix3/rtthread.elf"
strings "$RTT" | grep -F "$MARKER"
if [ "$STAGE" = panel-lckfb ]; then
    "$SDK/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-objdump" \
        -d --disassemble=st7701_480x800_init "$RTT" > "$RUN/artifacts/panel-init.disasm"
    grep -q '<st7701_480x800_init>:' "$RUN/artifacts/panel-init.disasm"
fi
strings "$TEST_ELF" | grep -F "$ELF_MARKER"
cp "$RTT" "$RUN/artifacts/rtthread.elf"
cp "$SDK/src/big/rt-smart/kernel/bsp/maix3/rtthread.bin" "$RUN/artifacts/rtthread.bin"
cp "$TEST_ELF" "$RUN/artifacts/$TEST_ELF_NAME"
cmp "$TEST_ELF" "$RUN/artifacts/$TEST_ELF_NAME"
cp "$IMAGES/big-core/rtt_system.bin" "$RUN/artifacts/rtt_system-touch-$STAGE.bin"
cp --reflink=auto --sparse=always "$IMAGES/sysimage-sdcard.img" "$RUN/artifacts/sysimage-touch-$STAGE.img"
cp "$IMAGES/sysimage-sdcard.img.gz" "$RUN/artifacts/sysimage-touch-$STAGE.img.gz"

# Verify the exact packed bytes, rather than just a successful build message.
size=$(stat -c %s "$IMAGES/big-core/rtt_system.bin")
dd if="$RUN/artifacts/sysimage-touch-$STAGE.img" of="$RUN/rtt-from-image.bin" \
    bs=1M skip=10 count="$size" iflag=count_bytes status=none
cmp "$RUN/rtt-from-image.bin" "$RUN/artifacts/rtt_system-touch-$STAGE.bin"
rm "$RUN/rtt-from-image.bin"
(cd "$RUN/artifacts" && sha256sum *.bin *.elf *.img *.gz > SHA256SUMS)
echo "PASS: packed RTT bytes match; board must print '$MARKER'"
echo "Artifacts: $RUN/artifacts"
