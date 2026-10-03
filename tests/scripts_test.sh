#!/usr/bin/env bash
set -euo pipefail
# No SDK/device emulation: exercise only PID scope and start-time reuse checks.
ROOT=$(pwd -P)
export LAWREC_RUN_DIR
LAWREC_RUN_DIR=$(mktemp -d)
trap 'rm -rf "$LAWREC_RUN_DIR"' EXIT
source tools/board/common.sh
ROOT=$(pwd -P)
pid=$$
ticks=$(start_ticks "$pid")
test -n "$ticks"
printf '%s %s\n' "$pid" "$ticks" > "$RUN_DIR/demo_ui.pid"
if owned_alive demo_ui; then echo 'Unrelated shell must not be owned' >&2; exit 1; fi
printf '%s 0\n' "$pid" > "$RUN_DIR/media_service.pid"
if owned_alive media_service; then echo 'Reused PID must not be owned' >&2; exit 1; fi
if sh tools/board/stop.sh > "$RUN_DIR/refusal.txt" 2>&1; then echo 'Must refuse unknown live PID' >&2; exit 1; fi
kill -0 "$pid"
grep -q 'Refusing to signal' "$RUN_DIR/refusal.txt"
echo 'scripts: POSIX syntax, unrelated/reused PID refusal and no signal sent passed'
