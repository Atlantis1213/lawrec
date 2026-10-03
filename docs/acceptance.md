# Acceptance

## Offline Gates

- [x] Three RISC-V skeleton programs cross-compile/link in K230 Docker.
- [x] Protocol lengths/version/commands and Unix socket mock checks.
- [ ] SDK three-channel, buffer, stride and MAPI API integration.
- [ ] AI2D/model input, decoder, NMS, rotation/coordinate checks.
- [ ] Independent queues, first/last source ownership, PTS and MP4 close checks.
- [ ] Real live555/library/sample stream checks and single-page LVGL rendering.
- [ ] Commit-based application package, ELF/library/model hashes and startup scripts.

## Board Gates (All Pending)

After offline work is complete, start vision, media, UI in that order. Stop in
reverse order, finishing the recorder before releasing codecs/camera.

- [ ] GC2093 frames and LCD preview; existing touch remains correct.
- [ ] AI face boxes/landmarks/count on LCD; AI switch does not stop encoding.
- [ ] VLC H.264 + G.711A at rtsp://<board-ip>:8554/lawrec.
- [ ] 10-20 second MP4 plays with sound on a computer after normal stop.
- [ ] Preview + AI + RTSP + record together; stopping one consumer leaves others alive.
- [ ] Measured AI2D/KPU/post/total latency, AI FPS, bitrate, queues, CPU/RSS/VB.

Compiling or exercising mock switches cannot check any board gate. Do not reflash,
change networking or change the passing touch/display driver during offline development.
