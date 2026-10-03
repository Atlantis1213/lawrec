# Final Demo Deployment And Short Acceptance

Status: offline application candidate, all hardware gates pending. No SD image
is generated, no SDK/kernel/network settings are changed, and no board is accessed.
Use the already passing 27 MHz/4-lane/ST7701/VTTH=1 firmware, not touch test kernels.
The package is not compatible with an arbitrary replacement firmware.

## Prepare The Application Bundle

On the development machine, from a clean committed worktree:

```sh
bash tools/build.sh package
```

This rebuilds business applications/LVGL in a new release directory, verifies
ELF structure and frozen adaptations, collects the recursive DT_NEEDED closure
from that SDK's sysroot, verifies/copies the single model and creates hashes.
`out/releases/<commit>-<UTC>/lawrec-demo-<commit>.tar.gz` is an **application
archive, not a flash image**. Build logs/compiler versions/SDK path/image ID/
source commit and runtime-library sources are inside `meta/`.

Only at the final board acceptance, stop any old media/UI and type q to stop
the old big application before copying/extracting. Do not start the old lawrec
init service or another camera owner at the same time. Prefer extracting the
archive to a new empty `/sharefs/lawrec-demo` directory; do not overwrite a live
application. On Linux, check before starting:

```sh
cd /sharefs/lawrec-demo
sha256sum -c SHA256SUMS
```

Keep at least 32 MiB free for the short clip. The programs use fixed RTSP IPv4
8554 and `/var/run/lawrec-demo.sock`. They use the board's existing network.
No replacement of `/lib`, `/usr/lib`, SD boot files or system init scripts occurs.
Private Linux libraries include the matching SDK loader; run.sh uses it explicitly
with a private library path. The matching board kernel/devices remain prerequisites.

## Start And Stop

On the **RT-Smart big-core console**, start just this one camera owner:

```sh
/sharefs/lawrec-demo/bin/vision.elf /sharefs/lawrec-demo/models/retinaface.kmodel
```

Wait for `[vision] ready result=0`, then on **Linux**:

```sh
cd /sharefs/lawrec-demo
sh start.sh
sh run.sh democtl status
```

No mbface argument. Preview/AI/RTSP/record initially OFF. start.sh starts only
media_service + demo_ui, logs under `/tmp/lawrec-demo`, and records PID/start
time under `/var/run/lawrec-demo`. It refuses duplicate script starts; service
also owns a singleton socket lock. An IPC handshake is not proof of LCD frames.
Scripts do not start RT-Smart through an unverified remote shell.

Stop in this order:

```sh
sh stop.sh
```

After Linux media has really exited, type **q** in the RT-Smart vision console.
Do not use Ctrl+C or killall. stop.sh only signals the PID/start-time/command
identities it registered, stops UI before media, and waits up to five seconds
per process. It refuses unknown/reused PIDs and never SIGKILLs an MP4 writer.
If shutdown times out, keep vision alive, inspect logs and retry stop.sh; do not
release camera/VB under a possible active callback. There is no auto-recovery.

## One Short Functional Pass

1. Tap Preview: observe **moving** camera content, not just an ON label. Tap
   Face AI: confirm boxes/count/landmarks and nonzero real timing/FPS.
2. Tap RTSP and Record with both preview and AI on. On a computer:

```sh
vlc --rtsp-tcp rtsp://<board-ip>:8554/lawrec
```

3. Confirm picture and microphone sound. Stop RTSP while Record remains active;
   restart RTSP, then stop Record while RTSP keeps playing. A new record auto
   stops after 15s of source PTS. No long soak or repeated 10x stress is needed.
4. Copy one finalized `/sharefs/lawrec_records/clip-*.mp4` to the computer;
   verify picture/sound and roughly 10-20s duration. `.mp4.part` means unfinished
   or failed, not a successful recording. Save one status line and the logs.
5. Stop Linux with stop.sh, then q on the big core. Record actual results in
   docs/acceptance.md; leave failures pending and keep their first error/log.

Optional command-line controls for isolating a UI issue:

```sh
sh run.sh democtl preview on
sh run.sh democtl ai on
sh run.sh democtl rtsp on
sh run.sh democtl record on
sh run.sh democtl status
sh run.sh democtl record off
sh run.sh democtl rtsp off
```

Indicators are measured callback rates, not guaranteed LCD/client rates. CPU/RSS
cover media_service only; UINT32_MAX in democtl means unavailable. VB is configured
pool budget. Current DSI/PHY/touch code is frozen; do not start timing experiments
again when diagnosing a camera/codec error.

The old CHN0 NOTREADY dump and vblank logs are not a new Demo run. First check
the new vision/media logs and actual moving LCD output. No offline test proves
sensor delivery, OSD blending, KPU execution, hardware microphone/PTSes or VLC.
