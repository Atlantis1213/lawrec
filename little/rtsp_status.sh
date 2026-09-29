#!/bin/sh

PIDFILE=/var/run/lawrec-rtsp.pid
LOGFILE=/tmp/lawrec-rtsp.log

if [ -f "${PIDFILE}" ] && kill -0 "$(cat "${PIDFILE}")" 2>/dev/null; then
    echo "running pid=$(cat "${PIDFILE}")"
else
    echo "stopped"
fi

if [ -f "${LOGFILE}" ]; then
    echo "--- ${LOGFILE} ---"
    tail -n 20 "${LOGFILE}"
fi
