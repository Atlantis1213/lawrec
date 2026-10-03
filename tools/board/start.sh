#!/bin/sh
set -eu
. "$(dirname -- "$0")/common.sh"
test "$(id -u)" = 0 || { echo 'Root is required for existing DRM/GPIO/MPP devices' >&2; exit 1; }
if ! mkdir -m 700 "$RUN_DIR"; then
    echo "Already started or stale $RUN_DIR; inspect it, then use stop.sh. No killall." >&2
    exit 1
fi
mkdir -p "$LOG_DIR"
fail() {
    trap - INT TERM
    echo "Startup failed; stopping this script's processes. Logs: $LOG_DIR" >&2
    sh "$ROOT/stop.sh" || true
    exit 1
}
trap fail INT TERM
sh "$ROOT/run.sh" media_service > "$LOG_DIR/media.log" 2>&1 &
media_pid=$!
register_pid media_service "$media_pid" || fail
ready=0
for attempt in 1 2 3 4 5; do
    sleep 1
    owned_alive media_service || fail
    if sh "$ROOT/run.sh" democtl status > "$LOG_DIR/start-status.txt" 2>&1; then ready=1; break; fi
done
test "$ready" = 1 || fail
sh "$ROOT/run.sh" demo_ui > "$LOG_DIR/ui.log" 2>&1 &
ui_pid=$!
register_pid demo_ui "$ui_pid" || fail
sleep 1
owned_alive demo_ui || fail
trap - INT TERM
echo "Linux started; logs=$LOG_DIR. Preview/AI/RTSP/record initially OFF."
echo 'No camera-frame or LCD readiness is inferred from this control handshake.'
