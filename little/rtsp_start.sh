#!/bin/sh

RTSP_DIR=/app/lawrec/rtsp
RTSP_BIN=${RTSP_DIR}/lawrec_rtsp
if [ -x /sharefs/lawrec_rtsp ]; then
    RTSP_BIN=/sharefs/lawrec_rtsp
fi
PIDFILE=/var/run/lawrec-rtsp.pid
LOGFILE=/tmp/lawrec-rtsp.log

SENSOR_TYPE=${LAWREC_RTSP_SENSOR_TYPE:-52}
VIDEO_TYPE=${LAWREC_RTSP_VIDEO_TYPE:-h264}
VIDEO_WIDTH=${LAWREC_RTSP_VIDEO_WIDTH:-1280}
VIDEO_HEIGHT=${LAWREC_RTSP_VIDEO_HEIGHT:-720}
AUDIO_INPUT=${LAWREC_RTSP_AUDIO_INPUT:-0}
SESSION_NUM=${LAWREC_RTSP_SESSION_NUM:-1}

mkdir -p /var/run

if [ ! -x "${RTSP_BIN}" ]; then
    echo "[lawrec-rtsp] missing ${RTSP_BIN}" >&2
    exit 1
fi

if [ -f "${PIDFILE}" ] && kill -0 "$(cat "${PIDFILE}")" 2>/dev/null; then
    echo "[lawrec-rtsp] already running pid=$(cat "${PIDFILE}")"
    exit 0
fi

: >"${LOGFILE}"
echo "[lawrec-rtsp] hint: start /app/sample_sys_init.elf on big core first" >>"${LOGFILE}"
echo "[lawrec-rtsp] launch ${RTSP_BIN} -s ${SENSOR_TYPE} -n ${SESSION_NUM} -t ${VIDEO_TYPE} -w ${VIDEO_WIDTH} -h ${VIDEO_HEIGHT} -a ${AUDIO_INPUT}" >>"${LOGFILE}"

start-stop-daemon -S -b -m -p "${PIDFILE}" --exec "${RTSP_BIN}" -- \
    -s "${SENSOR_TYPE}" \
    -n "${SESSION_NUM}" \
    -t "${VIDEO_TYPE}" \
    -w "${VIDEO_WIDTH}" \
    -h "${VIDEO_HEIGHT}" \
    -a "${AUDIO_INPUT}" >>"${LOGFILE}" 2>&1

sleep 1

if [ -f "${PIDFILE}" ] && kill -0 "$(cat "${PIDFILE}")" 2>/dev/null; then
    echo "[lawrec-rtsp] started pid=$(cat "${PIDFILE}")"
    exit 0
fi

echo "[lawrec-rtsp] failed to start, see ${LOGFILE}" >&2
exit 1
