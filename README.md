# Lawrec Edge Vision Demo

Independent implementation of [the development plan](docs/plan.md). The old
`lawrec` project is reference material, not a build dependency or a second source tree.

## Current Stage

Three-channel camera/preview, single MobileRetinaFace AI2D/KPU integration,
face decoding/OSD, SDK control IPC and a single asynchronous LVGL page now
cross-compile. **All hardware operation remains unverified.**
`media_service` forwards preview/AI/status to vision and runs a real asynchronous
live555 H.264/G.711A RTSP consumer and an independent MP4 recorder from the
same encoder/audio source. Both are wired to the one-page UI. Offline mux/decode
and loopback RTP tests are not hardware/VLC acceptance. Actual LVGL rendering
and measured metrics are integrated; a deployment bundle is still being prepared.
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
bash tools/build.sh rtsp
bash tools/build.sh media
bash tools/build.sh ui
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

## Shared Media Source

`MediaSource` starts VENC0 from YUV CHN2 and inner-codec mono-right AI/AENC0
only for the first subscriber. The last subscriber releases codecs; it never
reinitializes vision's camera/VB. Linux MAPI's existing workaround sets its
client-ready flag only; media_init/media_deinit must not reset remote pools.

H.264 is 1280x720/30 FPS/4000 kbps with a 30-frame GOP. G.711A is 8 kHz mono,
320 samples per packet (40 ms). I2S uses two physical slots with mono-right
selection, not a stereo output track. SDK buffer data is copied before returning
from callbacks. A complete access unit and its PTS are shared as immutable data
between separate RTSP and recorder video/audio queues, never one competing queue.

Each video queue is limited to 90 frames/8 MiB; audio to 100 packets/128 KiB.
RTSP congestion drops complete queued access units and waits for SPS/PPS/IDR;
IDR requests run outside callbacks. Record overflow reports an error rather
than silently dropping frames. A slow consumer cannot close the other queue.
Queues distinguish finishing accepted tails from immediate discard.

The SDK reader only calls back with at most 1 MiB of video data; the application
matches that bound and caches bounded SPS/PPS. Packet lengths/counts, access-unit
structure and per-track monotonic PTS are checked. RTP mapping uses a single
audio/video epoch; recorder helpers clip G.711A on exact 125-us sample boundaries.
These are offline logic checks. Clock alignment, first-sample audio PTS semantics,
packet lengths and actual concurrent CHN2/AI audio delivery remain board gates.

Source teardown calls SDK outside callback locks. Partial SDK initialization is
tracked where the SDK starts a local reader/FIFO before reporting remote failure.
An unconfirmed cleanup prevents a new subscription; service shutdown exits without
destroying a callback owner still potentially referenced by SDK threads. This is
basic resource safety, not reconnect/recovery. Source counters describe received
callbacks, not received RTP or completed MP4 writes.

## RTSP

RTSP ON checks vision availability, then starts an independent worker. It binds
IPv4 port 8554 before allocating codecs, waits up to three seconds for an accepted
complete IDR with SPS/PPS and an audio packet, and only then reports running.
No client is required for running; this state is server/source readiness, not a
claim of viewer reception. The two tracks reuse one source per track across clients.

The reactor never waits on a codec queue. NALs from the same copied access unit
retain one PTS; only the final NAL marks an RTP frame end. Both tracks share one
wall-clock mapping. Oversized NALs fail rather than truncate. New track sources
discard old live backlog, wait for a fresh IDR and request it outside callbacks.
Recorder queues are unaffected. SDP uses the source's cached SPS/PPS and static
PCMA payload 8 (8 kHz mono), without a nested pre-read event loop.

UI shows WAIT while start/stop is pending. STOP does not depend on a fresh vision
reply; SDK detach runs after live555 clients and objects close. A three-second
first-frame/stalled-callback deadline and codec/delivery errors report failed.
Shutdown joins the worker; SDK cleanup errors remain visible and block restart.
There is no automatic reconnect or retry.

`bash tools/build.sh rtsp` compiles unmodified SDK live555 sources natively inside
network-disabled Docker. A small fake codec adapter supplies synthetic access
units/audio; real RTSP DESCRIBE/SETUP/PLAY and interleaved RTP validate SDP, bytes,
multislice markers, shared PTS, bounds, bind conflict and stop/port release. It
does not decode H.264 or establish camera/audio/KPU/VLC hardware success.

## MP4 Recording

Record ON reserves a unique file under `/sharefs/lawrec_records`, subscribes to
the shared source and starts on a complete IDR. The worker writes H264 and PCMA,
not the SDK wrapper's sometimes-substituted PCMU. It reports recording only
after both tracks have a successfully written sample. No initial frames/audio
within three seconds fails visibly. Record STOP does not require a vision ACK.

Each clip ends automatically at 15 seconds of source PTS, or earlier on STOP.
Normal stop detaches just the record feed, drains its bounded accepted tail and
clips G711A to the video window on 125-us sample boundaries. Audio waits for a
confirmed video boundary; it cannot be written arbitrarily ahead of video.
Both tracks share the first IDR epoch. The muxer accepts milliseconds, so audio
sample data is exact but timestamp/final-duration rounding is bounded by 1 ms.
The fixed encoder uses no reordered B frames; PTS is also DTS.

All SDK temporary frames are already copied before the recorder accesses them.
Only the worker writes/closes MP4, never codec callbacks or the UI. `.mp4.part`
is renamed to `.mp4` only after both tracks and finalization succeed; failed
partial recordings are logged, not presented as successful files. Write/seek/
flush/close failures are retained. No fast-start relocation, fsync/power-loss
recovery, playback page or automated recovery is implemented.

Use SDK libmov/libflv rather than reimplementing a container. `tools/mp4.cmake`
builds private normalized source copies in `out/`; `patches/mp4-end-time.cmake`
adds an explicit final track boundary. Stock libmov otherwise writes a 1-ms
last sample delta and estimates a different header duration. The small patch
aligns stts/mdhd durations and avoids reading an uninitialized next-sample
sentinel. No SDK source, installed archive, panel or touch adapter is changed.

`bash tools/build.sh media` caches native x264/FFmpeg builds from existing SDK
sources, wholly in Docker/out. Real x264 creates a short 720p/no-B sample; actual
SDK muxer/readback and independent FFmpeg decode check a sample-clipped 0.5s file
and an automatically finalized 15s file. A narrow codec adapter accelerates PTS
delivery and checks one codec owner shared by RTSP/record, both stop orders,
write/close errors and an absent-frame deadline. This is not a 15s wall-clock
soak or evidence of actual K230 codec/audio/synchronization performance.

## Metrics And Actual Page Render

Protocol v2 is 24-byte requests / 84-byte status; rebuild both cores and UI
together. Camera/AI stats come from vision. Callback frame/byte deltas provide
measured H264 FPS and kbps over one-second monotonic windows, not configured
30 FPS/4000 kbps or client throughput. Source restarts reset the rate baseline.
Queues are the sum of both independent consumers' current depths.

CPU is `/proc/self/stat` utime+stime for **media_service only**, 100% is one CPU;
RSS is that same process's resident pages. UI/RT-Smart CPU/RSS are not included.
Missing measurements display `--`, not zero. VB is the configured common+OSD
pool budget, excluding KPU tensors and metadata, not a measured allocator total.

`bash tools/build.sh ui` builds SDK LVGL with the exact frozen config/fonts and
the actual board page module, rendering `out/ui-preview/{idle,running,error}.png`.
It checks bounds, transparent preview center, button commands, busy gating and
media STOP after backend failure. The background grid and populated metrics
are synthetic; this is not an LCD/camera/DRM/touch hardware test or an HTML mockup.

## Static Big ELF Layout

The SDK example's `*(*.got)` accidentally includes `.rela.got`, making `.got`
an invalid RELA section (8-byte entries versus required 24). The application
now owns a derived linker script with exact GOT patterns, collected `.rodata.*`,
explicit RX/RW page-start PT_LOADs and TLS. A remaining all-zero R_RISCV_NONE
record stays in its own correctly typed RELA section on the writable mapping.
The SDK loader categorizes allocated non-PROGBITS as data, so this section must
not share the text mapping's last page. No section is stripped to hide errors.

Docker `verify` requires clean readelf output and checks ELF table/entry sizes,
PT_LOAD bounds/alignment, RT-Smart entry/address/mapping rules, no dynamic loader
or unresolved strong symbols, and only harmless zero relocations. It rejects a
copy mutated back to the old bad RELA size. This is structural compatibility,
not execution of the hardware loader or proof the old board black screen is fixed.
