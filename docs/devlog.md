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

### 2026-10-03: Three-Channel Camera Candidate

- Changes: one Camera owner with checked VB, GC2093, VICAP and connector/layer
  calls. CHN0=800x480 YUV preview, CHN1=1280x720 planar RGB AI,
  CHN2=1280x720 YUV encode; explicit independent pool sizes and shared stream
  block constants (692224 bytes, 30 blocks). Common pool budget 47666 KiB excludes
  AI tensors, OSD and SDK metadata. Implemented shutdown error retention.
- Decision: bind CHN0 before VICAP init/start as in SDK sample_vicap. Keep binding
  alive during preview OFF; only gate layer1. Reserve user dump output only for
  RGB CHN1 using kd_mpi_vicap_set_dump_reserved. No claim this fixes the old
  hardware black screen; avoids copying its dynamic bind/unbind design.
- Validation: Docker big cross-build passed with real camera/MAPI SDK symbols.
  MAPI's built-in registration also requires libvvi (SDK dependency, no VVI demo
  pipeline added). Docker test passed with a narrow API fixture checking
  RGB/YUV formats/buffer sizes, pre-start binding, idempotent preview toggle,
  start/config failure cleanup and retaining VB when stream stop fails.
  Logs: out/build-camera.log, out/test-camera.log. Docker verify passed the
  ELF64/RISC-V/little-endian header predicates and all frozen byte checks;
  log out/verify-camera.log retains readelf's SDK section-table warning
  (section 326 sh_entsize=8, expected 24). This is not a full ELF structural pass
  or loader acceptance; do not hide it or alter the passing firmware.
- Unverified/risks: actual CHN2 delivery, three-channel concurrent hardware FPS,
  VO layering and shutdown. SDK status/AI model/OSD and real media not yet
  integrated. SDK baseline includes pre-existing CRLF/trailing spaces;
  deliberately preserved rather than reformatted during import.
- Next: SDK IPC between media and vision; initialize one MobileRetinaFace model,
  AI2D/KPU timings and rotated OSD overlay, then shared video/audio sources.
