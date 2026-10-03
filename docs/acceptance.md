# Acceptance

## Offline Gates

- [x] Three RISC-V skeleton programs cross-compile/link in K230 Docker.
- [x] Protocol lengths/version/commands and Unix socket mock checks.
- [x] SDK camera/buffer/stride API cross-build and focused ownership/order fixture.
- [x] AI2D/KPU SDK integration cross-build; model head checks, priors, NMS and coordinates.
- [x] SDK IPC cross-build; malformed bodies, pending command/ACK and retained errors fixture.
- [x] OSD cross-build; buffer retention on failed enable/disable fixture.
- [x] VENC/AENC real API cross-build; callback copying, separate consumer queues,
  GOP recovery, first/last ownership and cleanup fixture; shared PTS/clip logic.
- [x] live555 worker/API cross-build and real SDK loopback DESCRIBE/SETUP/PLAY,
  interleaved H264 multislice RTP markers/PCMA bytes, PTS/bounds and port/deadline checks.
- [x] Independent queues, first/last source ownership, PTS and actual MP4 close checks.
- [x] Real x264 sample + SDK muxer/readback + independent FFmpeg decode,
  0.5s sample-clipped and 15s automatically finalized H264/G711A files; shared-source stop orders.
- [x] Single-page actual LVGL rendering, transparency/layout/commands/busy/error checks.
- [x] AI times/FPS, callback video FPS/bitrate, queue depth, media CPU/RSS and configured VB budget integration.
- [x] Clean readelf output, full ELF table sizes and static RT-Smart mapping/entry/relocation checks;
  this is not execution of the board loader.
- [x] Commit-based clean application package, ELF/library/model hashes and scoped startup scripts.

Current Chinese UI candidate: code commit `1cfd6b830686`;
archive `out/releases/1cfd6b830686-20261003T091916Z/lawrec-demo-1cfd6b830686.tar.gz`.
The earlier d598a9206047 archive contains the English UI, not the current page.
Its bundled checklist is the pre-package snapshot; build/verify/runtime/hash
completion is recorded in its meta files and in the final devlog entry below.
Documentation-only commits do not change this candidate's application bytes.

## Board Gates (All Pending)

After offline work is complete, start vision, media, UI in that order. Stop in
reverse order, finishing the recorder before releasing codecs/camera.

- [ ] GC2093 frames and LCD preview; existing touch remains correct.
- [ ] Chinese labels readable on the physical 3.1-inch panel; four controls and
  details/return can be tapped without overlap or touch-mapping changes.
- [ ] AI face boxes/landmarks/count on LCD; AI switch does not stop encoding.
- [ ] VLC H.264 + G.711A at rtsp://<board-ip>:8554/lawrec.
- [ ] Audio packets match mono 8 kHz/320 bytes/40 ms; video/audio SDK clocks align,
  source PTS identifies the first audio sample, and CHN2 feeds VENC while RGB AI runs.
- [ ] 10-20 second MP4 plays with sound on a computer after normal stop.
- [ ] Preview + AI + RTSP + record together; stopping one consumer leaves others alive.
- [ ] Measured AI2D/KPU/post/total latency, AI FPS, bitrate, queues, CPU/RSS/VB.

Compiling or exercising mock switches cannot check any board gate. Do not reflash,
change networking or change the passing touch/display driver during offline development.

## Existing Board Report: Not A New-Demo Pass

The user's old `/app/lawrec/ui/ui` report contains a successful VI0->VO1 bind,
CHN0 dump error `0xa0158010`, one CHN1 1280x720 YUV frame and Linux
`CRTC vblank wait timed out`. This is old-program evidence, not a run of vision.

- `0xa0158010` maps to VICAP NOTREADY (16), not an allocation error or proof
  that the whole camera is stopped. Bound CHN0's user dump and its VO feed
  are different delivery paths. One CHN1 frame proves only that frame existed.
- New capture follows sample_vicap's supported semiplanar YUV format and
  binds before VICAP init/start. Preview OFF only gates the layer; it does
  not repeatedly unbind or insert an idle frame over the capture stream.
- A Linux vblank warning means DRM's wait missed its event. The inspected
  driver's enable_vblank changes a software flag; its IRQ handler then calls
  drm_crtc_handle_vblank. This warning alone neither proves a crash nor identifies
  the failing VI/VO stage. Check actual IRQ progression and visible output at
  final acceptance; no kernel/VTTH/clock changes or disabled-wait workaround.
- With new programs, confirm preview ON/OFF does not unbind VI0->VO1, verify
  visible camera motion, then enable AI independently to verify RGB delivery.
  UI button/state acknowledgement alone is insufficient. No long tests needed.

The black-screen root cause has not been confirmed on hardware. These changes
remove known source/configuration differences; they do not certify a repair.
