#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
exec docker run --rm --user "$(id -u):$(id -g)" \
  -v "$ROOT:$ROOT" -w "$ROOT" "${K230_DOCKER_IMAGE:-ghcr.io/kendryte/k230_sdk}" \
  bash -c 'set -eu
    mkdir -p out/tests
    g++ -std=c++11 -Wall -Wextra -I little/src/common tests/storage_test.cpp little/src/common/lawrec_storage.cpp -o out/tests/storage_test
    out/tests/storage_test "$PWD/out/tests/storage-$$"
    g++ -std=c++11 -Wall -Wextra -I little/src/common tests/annexb_test.cpp -o out/tests/annexb_test
    out/tests/annexb_test
  '
