#!/bin/sh
# Read-only, bounded evdev capture. Does not reset touch or change display state.
set -eu
label=${1:-idle}
seconds=${2:-10}
event=${3:-/dev/input/event0}
case "$label" in ''|*[!a-zA-Z0-9_-]*) echo 'Invalid label' >&2; exit 2;; esac
case "$seconds" in ''|*[!0-9]*) echo 'Invalid duration' >&2; exit 2;; esac
[ "$seconds" -ge 1 ] && [ "$seconds" -le 30 ] || exit 2
[ -c "$event" ] || { echo "Missing $event" >&2; exit 1; }
command -v evtest >/dev/null
out=$(mktemp -d "/tmp/lawrec-touch-${label}.XXXXXX")
reader=
cleanup() {
    if [ -n "$reader" ]; then
        kill "$reader" 2>/dev/null || true
        wait "$reader" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
echo "capture=$out event=$event seconds=$seconds"
{
    date
    uptime
    ps
    cat /proc/bus/input/devices
    for dev in /sys/bus/i2c/devices/*-0038; do
        [ -d "$dev" ] || continue
        echo "device=$dev driver=$(readlink "$dev/driver")"
        # DT u32 cells are big-endian: use byte dumps, not host-endian integers.
        for prop in compatible interrupts reset-gpios touchscreen-size-x touchscreen-size-y; do
            file="$dev/of_node/$prop"
            [ -r "$file" ] || continue
            echo "DT $prop"
            od -An -tx1 "$file"
        done
        node=$(readlink -f "$dev/of_node")
        if [ -n "$node" ] && [ -r "$node/../clock-frequency" ]; then
            echo 'DT I2C clock-frequency (requested rate, not measured bus clock)'
            od -An -tx1 "$node/../clock-frequency"
        fi
    done
} > "$out/context.log" 2>&1
for phase in before after; do
    cat /proc/interrupts > "$out/irq-$phase.log"
    cat /proc/stat > "$out/cpu-$phase.log"
    dmesg > "$out/dmesg-$phase.log"
    # Missing debugfs is recorded; do not mount or change kernel settings here.
    cat /sys/kernel/debug/dri/0/state > "$out/drm-$phase.log" 2>&1 || true
    cat /sys/kernel/debug/clk/clk_summary > "$out/clk-$phase.log" 2>&1 || true
    if [ "$phase" = before ]; then
        evtest "$event" > "$out/events.log" 2>&1 &
        reader=$!
        sleep "$seconds"
        kill "$reader" 2>/dev/null || true
        wait "$reader" 2>/dev/null || true
        reader=
    fi
done
echo "SYN_REPORT count=$(grep -c SYN_REPORT "$out/events.log" || true)"
echo "SYN_DROPPED count=$(grep -c 'SYN_DROPPED.*value' "$out/events.log" || true)"
echo 'Touch IRQ before/after:'
grep -iE 'edt|ft5|touch' "$out/irq-before.log" "$out/irq-after.log" || true
echo "Full evidence: $out (event count alone does not prove ghost touches)"
