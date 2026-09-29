#!/bin/sh

PIDFILE=/var/run/lawrec-rtsp.pid

if [ -f "${PIDFILE}" ]; then
    start-stop-daemon -K -p "${PIDFILE}" >/dev/null 2>&1 || true
    rm -f "${PIDFILE}"
fi

killall lawrec_rtsp >/dev/null 2>&1 || true

echo "[lawrec-rtsp] stopped"
