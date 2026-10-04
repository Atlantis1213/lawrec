#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker required' >&2; exit 1; }
ROOT=$(pwd -P)
NATIVE="$ROOT/out/tests/native"
SDK_MEDIA="$K230_SDK_ROOT/src/big/mpp/middleware/src"
mkdir -p "$NATIVE"
# These SDK tools are test-only. Never clean/configure the read-only originals.
if [ ! -f "$NATIVE/x264/libx264.a" ]; then
    mkdir -p "$NATIVE/x264"
    cp -a "$SDK_MEDIA/x264/src/." "$NATIVE/x264/"
    (
        cd "$NATIVE/x264"
        ./configure --disable-asm --disable-opencl --disable-lavf --disable-swscale \
            --disable-ffms --disable-cli --enable-static --bit-depth=8
        make clean
        make -j "$LAWREC_JOBS" lib-static
    )
fi
if [ ! -x "$NATIVE/generate-h264" ] || [ tests/media/generate_h264.cpp -nt "$NATIVE/generate-h264" ]; then
    c++ -std=c++17 -Wall -Wextra -Werror -I"$NATIVE/x264" \
        tests/media/generate_h264.cpp "$NATIVE/x264/libx264.a" -pthread -lm -o "$NATIVE/generate-h264"
fi
if [ ! -x "$NATIVE/ffmpeg/ffmpeg" ] || [ ! -x "$NATIVE/ffmpeg/ffprobe" ]; then
    mkdir -p "$NATIVE/ffmpeg"
    cp -a "$SDK_MEDIA/ffmpeg/src/." "$NATIVE/ffmpeg/"
    (
        cd "$NATIVE/ffmpeg"
        ./configure --disable-everything --disable-autodetect --disable-doc \
            --disable-debug --disable-x86asm --disable-avdevice --disable-postproc \
            --disable-swscale --enable-ffmpeg --enable-ffprobe \
            --enable-decoder=h264,pcm_alaw --enable-parser=h264 \
            --enable-demuxer=h264,mov --enable-protocol=file --enable-muxer=null \
            --enable-encoder=wrapped_avframe,pcm_s16le --enable-filter=anull,null,aresample
        make clean
        make -j "$LAWREC_JOBS" ffmpeg ffprobe
    )
fi
mkdir -p out/tests/media
"$NATIVE/generate-h264" out/tests/media/sample.h264
