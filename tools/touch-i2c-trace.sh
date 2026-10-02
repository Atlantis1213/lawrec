#!/bin/sh
# Observe existing driver transfers; never issue I2C transactions or factory commands.
set -eu
seconds=${1:-5}
event=${2:-/dev/input/event0}
case "$seconds" in ''|*[!0-9]*) exit 2;; esac
[ "$seconds" -ge 1 ] && [ "$seconds" -le 30 ] || exit 2
[ -c "$event" ] || exit 2
command -v evtest >/dev/null
root=/sys/kernel/debug/tracing
[ -d "$root/events/i2c/i2c_reply" ] || {
    echo 'I2C tracepoints unavailable; mount debugfs or use a diagnostic kernel.' >&2
    exit 1
}
out=$(mktemp -d /tmp/lawrec-touch-packets.XXXXXX)
instance="$root/instances/lawrec-touch-$$"
mkdir "$instance"
reader=
cleanup() {
    if [ -n "$reader" ]; then
        kill "$reader" 2>/dev/null || true
        wait "$reader" 2>/dev/null || true
    fi
    echo 0 > "$instance/tracing_on" 2>/dev/null || true
    echo 0 > "$instance/events/enable" 2>/dev/null || true
    rmdir "$instance" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
echo 0 > "$instance/tracing_on"
echo 512 > "$instance/buffer_size_kb"
for name in i2c_write i2c_reply; do
    echo 'adapter_nr == 0 && addr == 56' > "$instance/events/i2c/$name/filter"
    echo 1 > "$instance/events/i2c/$name/enable"
done
echo 'adapter_nr == 0' > "$instance/events/i2c/i2c_result/filter"
echo 1 > "$instance/events/i2c/i2c_result/enable"
cat /proc/interrupts > "$out/irq-before.log"
cat /sys/kernel/debug/canaan_clk/dpipclk/reg_info > "$out/pixel-clock.log" 2>&1 || true
evtest "$event" > "$out/events.log" 2>&1 &
reader=$!
echo 1 > "$instance/tracing_on"
echo "capture=$out duration=$seconds bus=0 addr=0x38 (do not touch unless requested)"
sleep "$seconds"
echo 0 > "$instance/tracing_on"
cat "$instance/trace" > "$out/i2c.log"
cat "$instance/per_cpu/cpu0/stats" > "$out/trace-stats.log"
cat /proc/interrupts > "$out/irq-after.log"
kill "$reader" 2>/dev/null || true
wait "$reader" 2>/dev/null || true
reader=
echo "replies=$(grep -c i2c_reply: "$out/i2c.log" || true)"
cat "$out/trace-stats.log"
echo "Saved $out"
