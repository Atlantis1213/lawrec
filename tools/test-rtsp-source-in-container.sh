#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv ]] || { echo 'Use tools/test.sh rtsp-source (Docker only)'; exit 2; }
LIVE="$SDK/src/common/cdk/user/thirdparty/live"
mkdir -p out/tests
g++ -std=c++14 -O1 -Wall -Wextra -pthread -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t \
    -I little/src/rtsp/include -I "$LIVE/liveMedia/include" -I "$LIVE/UsageEnvironment/include" \
    -I "$LIVE/BasicUsageEnvironment/include" -I "$LIVE/groupsock/include" \
    tests/rtsp_source_test.cpp little/src/rtsp/src/LiveFrameSource.cpp \
    little/src/rtsp/src/h264LiveFrameSource.cpp little/src/rtsp/src/g711LiveFrameSource.cpp \
    "$LIVE/liveMedia/FramedSource.cpp" "$LIVE/liveMedia/MediaSource.cpp" \
    "$LIVE/liveMedia/Media.cpp" "$LIVE/liveMedia/Base64.cpp" \
    "$LIVE/UsageEnvironment/UsageEnvironment.cpp" "$LIVE/UsageEnvironment/HashTable.cpp" \
    "$LIVE/UsageEnvironment/strDup.cpp" "$LIVE/BasicUsageEnvironment/BasicHashTable.cpp" \
    "$LIVE/BasicUsageEnvironment/BasicUsageEnvironment.cpp" "$LIVE/BasicUsageEnvironment/BasicUsageEnvironment0.cpp" \
    "$LIVE/BasicUsageEnvironment/BasicTaskScheduler.cpp" "$LIVE/BasicUsageEnvironment/BasicTaskScheduler0.cpp" \
    "$LIVE/BasicUsageEnvironment/DelayQueue.cpp" -o out/tests/rtsp_source_test
timeout 20 out/tests/rtsp_source_test
