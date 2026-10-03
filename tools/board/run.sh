#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
case "${1:-}" in media_service|demo_ui|democtl) program=$1; shift;;
    *) echo 'Usage: run.sh {media_service|demo_ui|democtl} [args]' >&2; exit 2;;
esac
# Use the matching packaged SDK loader without replacing /lib or system libraries.
unset LD_PRELOAD LD_LIBRARY_PATH
exec "$ROOT/lib/ld-linux-riscv64xthead-lp64d.so.1" --library-path "$ROOT/lib" "$ROOT/bin/$program" "$@"
