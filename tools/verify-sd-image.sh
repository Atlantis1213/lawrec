#!/usr/bin/env bash
# Run inside the SDK Docker image; verify bytes from the final SD image.
set -euo pipefail
SDK=${K230_SDK_ROOT:?}
PROJECT=${LAWREC_PROJECT_ROOT:-/home/atlantis/lawrec}
IMAGES="$SDK/output/k230_canmv_lckfb_defconfig/images"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
dd if="$IMAGES/sysimage-sdcard.img" of="$TMP/rootfs.ext4" bs=1M skip=128 count=128 status=none
cmp "$TMP/rootfs.ext4" "$IMAGES/little-core/rootfs.ext4"
e2fsck -fn "$TMP/rootfs.ext4"
for path in app/lawrec/ui/ui usr/lib/liblvgl.so usr/lib/liblv_drivers.so etc/init.d/S99lawrec etc/init.d/S50sshd mnt/k_ipcm.ko mnt/k_virt-tty.ko mnt/sharefs; do
    debugfs -R "dump /$path $TMP/file" "$TMP/rootfs.ext4" >/dev/null 2>&1
    test -s "$TMP/file"
    cmp "$TMP/file" "$IMAGES/little-core/rootfs/$path"
    rm "$TMP/file"
    echo "PASS /$path"
done
cmp "$IMAGES/little-core/rootfs/app/lawrec/ui/ui" "$SDK/output/k230_canmv_lckfb_defconfig/little/buildroot-ext/target/app/lawrec/ui/ui"
cmp "$PROJECT/out/big/lawrec.elf" "$SDK/src/big/rt-smart/userapps/root/bin/fastboot_app.elf"
for name in null console; do
    debugfs -R "stat /dev/$name" "$TMP/rootfs.ext4" 2>/dev/null | grep 'Type: character special'
done
for name in service xiaodemo; do
    debugfs -R "stat /app/lawrec/$name" "$TMP/rootfs.ext4" 2>&1 | grep 'File not found'
done
for entry in '10 big-core/rtt_system.bin' '30 little-core/linux_system.bin'; do
    read -r offset file <<< "$entry"
    size=$(stat -c %s "$IMAGES/$file")
    dd if="$IMAGES/sysimage-sdcard.img" of="$TMP/partition" bs=1M skip="$offset" count="$size" iflag=count_bytes status=none
    cmp "$TMP/partition" "$IMAGES/$file"
    echo "PASS $file in SD image"
done
grep -q 'autostart disabled' "$SDK/src/big/rt-smart/userapps/root/bin/init.sh"
sha256sum "$IMAGES/sysimage-sdcard.img" "$IMAGES/sysimage-sdcard.img.gz"
