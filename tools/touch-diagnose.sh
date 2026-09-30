#!/bin/sh
# Read-only diagnostics; never scan a live I2C bus or write pin registers.
echo '=== input devices ==='
cat /proc/bus/input/devices
echo '=== touch devices and bound drivers ==='
for dev in /sys/bus/i2c/devices/*-0038; do
    [ -d "$dev" ] || continue
    echo "$dev"
    cat "$dev/name" 2>/dev/null
    readlink "$dev/driver"
done
echo '=== relevant kernel messages ==='
dmesg | grep -iE 'edt|ft5|touch|i2c|gpio23' | tail -n 100
echo '=== IRQ before: touch and release repeatedly for 10 seconds ==='
grep -iE 'edt|ft5|touch|gpio' /proc/interrupts
sleep 10
echo '=== IRQ after ==='
grep -iE 'edt|ft5|touch|gpio' /proc/interrupts
echo '=== running UI ==='
pidof ui
echo '=== recent application touch trace ==='
tail -n 60 /tmp/touch_trace.log 2>/dev/null
echo '=== recent input initialization ==='
grep 'lawrec indev:' /tmp/lawrec.log 2>/dev/null | tail -n 30
