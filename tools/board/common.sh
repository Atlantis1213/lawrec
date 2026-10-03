#!/bin/sh
# Only processes registered by this start.sh are candidates for stop.sh.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
RUN_DIR=${LAWREC_RUN_DIR:-/var/run/lawrec-demo}
LOG_DIR=${LAWREC_LOG_DIR:-/tmp/lawrec-demo}
start_ticks() {
    sed 's/^.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $20}'
}
owned_alive() {
    name=$1
    test -f "$RUN_DIR/$name.pid" || return 1
    read -r pid saved_ticks < "$RUN_DIR/$name.pid" || return 1
    case "$pid" in ''|*[!0-9]*) return 1;; esac
    test "$pid" -gt 1 && test -d "/proc/$pid" || return 1
    test "$(start_ticks "$pid")" = "$saved_ticks" || return 1
    tr '\000' '\n' < "/proc/$pid/cmdline" | grep -Fx "$ROOT/bin/$name" > /dev/null
}
register_pid() {
    name=$1 child=$2
    ticks=$(start_ticks "$child")
    test -n "$ticks" || return 1
    printf '%s %s\n' "$child" "$ticks" > "$RUN_DIR/$name.pid"
}
