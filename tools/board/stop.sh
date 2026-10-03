#!/bin/sh
set -eu
. "$(dirname -- "$0")/common.sh"
test -d "$RUN_DIR" || { echo 'No processes registered by start.sh'; exit 0; }
stop_one() {
    target=$1
    if ! owned_alive "$target"; then
        if test -f "$RUN_DIR/$target.pid"; then
            read -r recorded_pid ignored < "$RUN_DIR/$target.pid"
            case "$recorded_pid" in ''|*[!0-9]*) echo "Invalid $target PID record; inspect $RUN_DIR" >&2; return 1;; esac
            if test -d "/proc/$recorded_pid"; then
                echo "Refusing to signal unconfirmed/reused PID $recorded_pid for $target" >&2; return 1
            fi
            rm "$RUN_DIR/$target.pid"
        fi
        return 0
    fi
    saved_pid=$pid
    kill -TERM "$saved_pid" || return 1
    for attempt in 1 2 3 4 5; do
        sleep 1
        if ! owned_alive "$target"; then rm "$RUN_DIR/$target.pid"; return 0; fi
    done
    echo "$target shutdown exceeded 5 seconds. Not using SIGKILL; keep vision running and inspect logs." >&2
    return 1
}
stop_one demo_ui
stop_one media_service
rmdir "$RUN_DIR"
echo 'Linux stopped. Now type q in the vision RT-Smart console; do not use Ctrl+C.'
