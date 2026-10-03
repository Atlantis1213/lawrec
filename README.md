# Lawrec Edge Vision Demo

Independent implementation of [the development plan](docs/plan.md). The old
`lawrec` project is reference material, not a build dependency or a second source tree.

## Current Stage

Repository, fixed-width control protocol, Unix socket mock, Docker cross-build
and a single LVGL page. **Camera, AI/OSD and real media still need integration.**
`media_service` returns `-ENOSYS` in normal mode until its hardware backend exists.
`--mock SOCKET` is exclusively an offline control fixture; never deploy it as media.

## Ownership

- `vision.elf` (RT-Smart): VB, GC2093, VICAP, VO, RGB AI and OSD.
- `media_service` (Linux): one H.264/G.711A source shared by RTSP and MP4.
- `demo_ui` (Linux): touch, one page, bounded asynchronous socket requests.

Candidate channel plan: CHN0 800x480 YUV preview, CHN1 1280x720 planar RGB AI,
CHN2 1280x720 YUV encoder. Three-channel hardware concurrency is **not verified**.
Network configuration, face identity databases, playback and recovery mechanisms
are excluded. No automatic deployment, SDK mutation or board connection.

## Build

```sh
export K230_SDK_ROOT=/home/atlantis/k230_sdk
bash tools/build.sh all
bash tools/build.sh test
```

Uses K230 SDK Docker with networking disabled and SDK mounted read-only.
Requires the SDK's existing Buildroot host/sysroot, libdisp and LVGL 8.3.1 source
tree. `LVGL_ROOT` may select another existing SDK LVGL 8.3.1 source directory
inside that SDK mount. No compiler runs on the host.
Artifacts live under `out/big` and `out/little`; never committed.

## Hardware Baseline

See [SDK baseline](docs/sdk-baseline.md) and [acceptance](docs/acceptance.md).
The imported input/display adapter is unchanged, including portrait X reflection.
ST7701 is a frozen reference, not a new driver to install. UI only configures its
DRM overlay; it never calls connector power/reset/init.

Final RTSP URL: `rtsp://<board-ip>:8554/lawrec`. VLC example after acceptance:
`vlc --rtsp-tcp rtsp://<board-ip>:8554/lawrec`.
Final record directory: `/sharefs/lawrec_records`. Applications are not yet a
deployable full pipeline; startup scripts/package/model instructions follow integration.
