# Lawrec Edge Vision Demo

Independent implementation of [the development plan](docs/plan.md). The old
`lawrec` project is reference material, not a build dependency or a second source tree.

## Current Stage

Three-channel camera/preview, single MobileRetinaFace AI2D/KPU integration,
face decoding/OSD, SDK control IPC and a single asynchronous LVGL page now
cross-compile. **All hardware operation remains unverified.**
`media_service` forwards preview/AI/status to vision. RTSP/record still return
`-ENOSYS`: shared H.264/G.711A, live555 and MP4 integration is not finished.
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
bash tools/build.sh verify
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

Camera follows the SDK sample's pre-start VI-to-VO binding order. Preview ON/OFF
only enables/disables video layer1; the camera binding and RGB/YUV feeds remain
alive until shutdown. RGB CHN1 explicitly reserves SDK dump output. Existing
panel/PHY/kernel source and LVGL adapter bytes remain unchanged. This is a
single candidate, not an automatic fallback pipeline or proven black-screen fix.

VICAP CHN0/CHN2 use YUV semiplanar 420, matching SDK sample_vicap. The VO
layer retains the SDK sample's YVU planar convention; do not copy that enum
into the capture channel attributes. Preview ON means a checked layer-enable
request succeeded, not proof of sensor frames or a visible LCD image.

## Model And OSD

One SDK `src/big/kmodel/door_lock/retinaface.kmodel`, not mbface or an identity DB.
SHA256: `082f76bec6db39ee7a9c4c83c2d8196fdfa0ba487b7d07831647df01dee8d808`.
The same bytes occur as SDK mobile_retinaface/test.kmodel and ROMFS retinaface.
Model file is not committed; supply it on the board only at final acceptance.
Invocation: `vision.elf /bin/retinaface.kmodel` (requires the frozen kernel).

The SDK reference ONNX is `src/big/nncase/examples/models/mobile_retinaface.onnx`,
SHA256 `b0abe364a3f82ce1714589479494ef440d9fdf3f02218a77cfd20bc7679df336`.
Compilation reference is examples/scripts/mobile_retinaface.py: RGB uint8 NCHW,
swapRB=false, built-in mean [123,117,104], std=1. Provenance is the local SDK,
not a freshly reproduced model compilation or hardware numerical validation.

Runtime validates 320x320 input and nine float32 NCHW heads, applies AI2D
letterboxing (1280x720 to 320x180 with 70-pixel top/bottom padding), runs KPU,
then stable two-class softmax, threshold 0.35 and NMS 0.5. Generated 4200 priors
are checked against the SDK anchor table. LCD output is limited to 32 faces.
OSD uses SDK OSD2/insert CHN5 with two private ARGB buffers (3000 KiB), separate
from this SDK Linux DRM's OSD4..7. Layer blending and double-buffer scanout
are pending board acceptance. Neither OSD nor UI reinitializes the panel.

UI operation failures persist across passive polling; asynchronous vision errors
are not replaced with zero by the service. Successful polling is not evidence
of frame readiness, and unavailable codec operations never claim success.
