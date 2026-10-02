#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker is required'; exit 1; }
# This container must be launched with --network none and CAP_NET_ADMIN.
# Never use --network host: names and link operations are private test fixtures.
test "$(ls /sys/class/net)" = lo
mkdir -p out/tests/arp-transport
trap 'chown -R "$LAWREC_HOST_UID:$LAWREC_HOST_GID" out/tests/arp-transport' EXIT
g++ -std=c++11 -Wall -Wextra -pthread -I little/src/common \
    tests/arp_transport_test.cpp little/src/common/lawrec_arp.cpp -o out/tests/arp-transport/test
out/tests/arp-transport/test
