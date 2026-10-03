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

### 2026-10-03: Preview Report Triage And AI/Control Integration

- Changes: corrected VICAP CHN0 to YUV semiplanar 420 while retaining the SDK
  VO layer's distinct YVU planar convention. Old code used the VO enum for
  capture and logged an ISP default-format fallback. The new pipeline retains
  its pre-start binding and layer-only preview switches. Recorded the old board
  report in acceptance without claiming a hardware root cause or repair.
- Changes: added one model owner, AI2D letterbox, KPU runtime, nine-head face
  decoder, bounded NMS and clockwise portrait mapping; dedicated OSD2/CHN5
  boxes, landmarks and count with two private buffers. Added SDK IPC snapshots
  and main-thread command ACK; Linux forwards preview/AI/status, leaving codecs
  explicitly ENOSYS. UI keeps failed-operation feedback across polling.
- Safety: SDK buffers remain allocated after an unconfirmed OSD disable, including
  insert-success/enable-failure. Shutdown skips camera/VB/MAPI teardown when OSD
  ownership is retained. Successful service polls preserve asynchronous AI errors.
- Decision: preserve all frozen display/touch/kernel bytes and their existing
  vblank behavior. CHN0 NOTREADY and a CHN1 frame do not establish visible preview;
  vblank warning alone does not establish a UI crash. No SDK mutation, board
  connection, deployment or old-project edits.
- Build issue: initial Linux IPC link failed because slave/lib uses GCC12 vector1/Zve
  attributes, incompatible with Linux's GCC10/V2.6 linker. Inspected archives in
  Docker and matched the old Linux build's explicit host/lib selection. Kept
  toolchain/SDK unchanged; no metadata stripping or incompatible-link suppression.
- Validation: Docker `bash tools/build.sh big`, `little`, `test`, `verify` passed.
  Pure decoder test compares all 4200 priors with SDK anchors and checks logits,
  NMS, padding, NaN/Inf and portrait mapping. Narrow camera fixture checks capture
  versus VO formats/order; IPC fixture checks malformed body/module, busy slot,
  delayed ACK, shutdown and retained errors; OSD fixture checks layer, transparent
  pixels and resource retention. These are not hardware simulations or acceptance.
  Logs: out/build-integration.log, out/build-ipc-fixed.log,
  out/test-integration.log and out/verify-integration.log.
- Remaining: SDK header 'KMDL' warning and the pre-existing ELF section sh_entsize
  issue (now section 2305) remain. Verify passes header predicates/frozen hashes,
  not full ELF structural or loader validation. Actual model loading, three-channel
  delivery, KPU, OSD scanout/blending, vblank events and preview remain unverified.
- Next: one shared H.264/G.711A source and bounded independent RTSP/MP4 queues;
  real media, final page rendering and deployment bundle remain unfinished.
