#!/bin/sh
# No timeout applet is required by this BusyBox-compatible capture.
set -eu
EVTEST=$(command -v evtest)
grep -iE 'edt|ft5|touch|gpio' /proc/interrupts
"$EVTEST" /dev/input/event0 > /tmp/lawrec-touch-evtest.log 2>&1 &
reader=$!
trap 'kill "$reader" 2>/dev/null || true' EXIT HUP INT TERM
sleep 20
kill "$reader" 2>/dev/null || true
wait "$reader" 2>/dev/null || true
trap - EXIT HUP INT TERM
grep -iE 'edt|ft5|touch|gpio' /proc/interrupts
cat /tmp/lawrec-touch-evtest.log
