# Development Log

## Status

Offline development only. No hardware acceptance has been performed for this repository.

### 2026-10-03: Independent Demo Skeleton

- Changes: new repository, plan, frozen board adapters, three program build
  targets, fixed-width request/status protocol, asynchronous one-page LVGL UI,
  Unix socket command fixture and focused protocol/socket tests.
- Decision: retain three owners (vision/media/UI). Old networking, identity DB,
  playback and complex recovery are not imported. Real media commands return
  ENOSYS until integration; only explicit --mock can fabricate switch states.
- Validation: Docker `bash tools/build.sh big`, `little`, `test` passed. Three
  RISC-V executable targets and two LVGL libraries linked; protocol and socket
  tests checked lengths/version/values/IDs, four mock switches, idempotence and
  stopping RTSP while record remains enabled (mock control only). Initial Linux
  build exposed the SDK wrapper's /opt/toolchain dependency and an unnecessary
  freetype link; fixed Docker mount and omitted freetype (frozen config disables it).
  Final Linux build log: out/build-little.log. No board connection.
- Unverified/risks: camera, channel concurrency, KPU, OSD blending, audio PTS,
  RTSP/MP4 and UI hardware operation. Current targets are not a functional package.
- Next: implement checked VB/three-channel camera/display ownership and SDK IPC,
  then the single MobileRetinaFace model and OSD.
